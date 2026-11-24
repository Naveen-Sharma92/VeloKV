// AOF writing and replay. The writer batches commands in memory and writes them
// with one write() per event-loop step; replay streams the file through the
// same incremental RESP parser the network uses.
#include "aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "commands.h"
#include "keyspace.h"
#include "resp.h"

namespace {

[[noreturn]] void throwErrno(const std::string& what) {
    const int err = errno;
    throw std::system_error(err, std::generic_category(), what);
}

std::runtime_error corruptAt(const std::string& path, std::size_t offset, const std::string& why) {
    return std::runtime_error("AOF " + path + " is corrupt at byte " + std::to_string(offset) + ": " + why +
                              " (refusing to start; fix or move the file)");
}

}  // namespace

std::optional<FsyncPolicy> parseFsyncPolicy(std::string_view name) {
    if (name == "always") return FsyncPolicy::Always;
    if (name == "everysec") return FsyncPolicy::EverySec;
    if (name == "no") return FsyncPolicy::No;
    return std::nullopt;
}

// ------------------------------------------------------------------ replay

AofLoadResult loadAof(const std::string& path, Keyspace& db) {
    Fd fd(::open(path.c_str(), O_RDWR | O_CLOEXEC));
    if (!fd.valid()) {
        if (errno == ENOENT) return {};  // first start: nothing to load
        throwErrno("open " + path);
    }

    AofLoadResult result;
    std::string buffer;      // bytes read but not yet replayed
    std::size_t offset = 0;  // file offset of buffer[0] = end of the last complete command
    std::string reply;       // replies are produced and thrown away
    char chunk[64 * 1024];

    db.setLoading(true);
    for (;;) {
        const ssize_t n = ::read(fd.get(), chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            throwErrno("read " + path);
        }
        if (n == 0) break;
        buffer.append(chunk, static_cast<std::size_t>(n));

        std::size_t pos = 0;
        while (pos < buffer.size()) {
            // We only ever write RESP arrays; anything else (e.g. the zero bytes
            // some filesystems leave after a power cut) is corruption.
            if (buffer[pos] != '*') throw corruptAt(path, offset + pos, "expected '*'");
            resp::ParseResult cmd = resp::parseCommand(std::string_view(buffer).substr(pos));
            if (cmd.status == resp::ParseStatus::Incomplete) break;  // read more
            if (cmd.status == resp::ParseStatus::Error) throw corruptAt(path, offset + pos, cmd.error);

            if (!cmd.args.empty()) {
                // Same code path as a client command, without propagation: we are
                // reading the log, not adding to it.
                reply.clear();
                executeCommand(db, cmd.args, reply);
                if (!reply.empty() && reply[0] == '-') {
                    throw corruptAt(path, offset + pos, "command failed on replay: " + reply.substr(1, reply.size() - 3));
                }
                ++result.commands;
            }
            pos += cmd.consumed;
        }
        buffer.erase(0, pos);
        offset += pos;
    }
    db.setLoading(false);

    if (!buffer.empty()) {
        // The process (or machine) died in the middle of appending the last
        // command. Everything before it is intact, so drop the fragment and cut
        // the file back, so that new commands are appended after a complete one.
        std::fprintf(stderr, "velokv: warning: AOF %s ends with an incomplete command (%zu bytes); truncating to %zu bytes\n",
                     path.c_str(), buffer.size(), offset);
        if (::ftruncate(fd.get(), static_cast<off_t>(offset)) < 0) throwErrno("truncate " + path);
        result.truncatedBytes = buffer.size();
    }
    return result;
}

// ------------------------------------------------------------------ writer

Aof::Aof(const std::string& path, FsyncPolicy policy)
    : fd_(::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)),
      policy_(policy),
      lastFsync_(std::chrono::steady_clock::now()) {
    if (!fd_.valid()) throwErrno("open " + path);
}

void Aof::append(const std::vector<std::string>& args) {
    resp::appendArrayHeader(buffer_, args.size());
    for (const auto& arg : args) resp::appendBulkString(buffer_, arg);
}

void Aof::flush() {
    std::size_t written = 0;
    while (written < buffer_.size()) {
        const ssize_t n = ::write(fd_.get(), buffer_.data() + written, buffer_.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            // E.g. disk full. Stop rather than keep acknowledging writes we cannot
            // persist; the partial entry this may leave is repaired on restart.
            throwErrno("write AOF");
        }
        written += static_cast<std::size_t>(n);
    }
    if (written > 0) {
        buffer_.clear();
        unsynced_ = true;
    }
    if (policy_ == FsyncPolicy::Always && unsynced_) fsyncNow();
}

void Aof::tick() {
    if (policy_ != FsyncPolicy::EverySec || !unsynced_) return;
    if (std::chrono::steady_clock::now() - lastFsync_ >= std::chrono::seconds(1)) fsyncNow();
}

void Aof::sync() {
    flush();
    if (unsynced_) fsyncNow();
}

void Aof::fsyncNow() {
    // fdatasync skips metadata that is not needed to read the data back (such as
    // the modification time), so it is cheaper than fsync. Redis uses it too.
    if (::fdatasync(fd_.get()) < 0) throwErrno("fdatasync AOF");
    unsynced_ = false;
    lastFsync_ = std::chrono::steady_clock::now();
}
