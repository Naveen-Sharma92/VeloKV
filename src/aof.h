// Append-only file persistence: every change is written to the file as a RESP
// command, synced to disk according to the fsync policy, and replayed through
// the normal command path at startup to rebuild the keyspace.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fd.h"

class Keyspace;

// When written data is forced from the OS page cache to the disk. Data that
// reached the page cache already survives the process crashing; fsync is about
// surviving the machine crashing or losing power.
enum class FsyncPolicy {
    Always,    // fsync before replying: safest, slowest
    EverySec,  // fsync about once a second: lose at most ~1 s of writes
    No         // leave it to the OS (Linux writes back within ~30 s)
};

// "always", "everysec", "no"; nullopt for anything else.
std::optional<FsyncPolicy> parseFsyncPolicy(std::string_view name);

struct AofLoadResult {
    std::size_t commands = 0;        // commands replayed
    std::size_t truncatedBytes = 0;  // size of an incomplete last entry that was cut off
};

// Replays the AOF at `path` into `db` through the normal command path, with
// expiry paused. A missing file is an empty database. An incomplete final
// command (a crash in the middle of a write) is dropped with a warning and the
// file is truncated back to the last complete command. Anything else that does
// not parse, or a command that fails, throws std::runtime_error: we refuse to
// start from a corrupt file rather than silently lose data.
AofLoadResult loadAof(const std::string& path, Keyspace& db);

class Aof {
public:
    // Opens (or creates) the file for appending. Throws std::system_error.
    Aof(const std::string& path, FsyncPolicy policy);

    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    // Adds one command to the in-memory buffer. Nothing touches the disk yet.
    void append(const std::vector<std::string>& args);

    // Writes the buffer to the file (and fsyncs under `always`). The server calls
    // this before sending replies, so a client never sees "OK" for a write that
    // is not at least in the OS page cache. Throws std::system_error on failure.
    void flush();

    // Called on every server tick: under `everysec`, fsyncs if something was
    // written since the last fsync and at least a second has passed.
    void tick();

    // Flush and fsync now, whatever the policy. Used at shutdown.
    void sync();

private:
    void fsyncNow();

    Fd fd_;
    FsyncPolicy policy_;
    std::string buffer_;
    bool unsynced_ = false;  // written to the file but not yet fsynced
    std::chrono::steady_clock::time_point lastFsync_;
};
