// Entry point: parses command-line options, sets up signal handling, and runs
// the server until SIGINT or SIGTERM asks it to stop.
#include <signal.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <string>

#include "aof.h"
#include "keyspace.h"
#include "server.h"

namespace {

// Set from a signal handler. A lock-free atomic is one of the few things a
// signal handler may safely touch.
std::atomic<bool> gStopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void onStopSignal(int) { gStopRequested.store(true); }

struct Options {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = 6380;
    std::string aofPath;  // empty: no persistence
    FsyncPolicy fsync = FsyncPolicy::EverySec;
};

void printUsage(std::ostream& out, const char* prog) {
    out << "Usage: " << prog << " [--port N] [--bind ADDR] [--aof FILE] [--fsync POLICY]\n"
        << "  --port N         TCP port to listen on (default 6380; 0 = pick a free port)\n"
        << "  --bind ADDR      IPv4 address to listen on (default 127.0.0.1)\n"
        << "  --aof FILE       persist every change to this append-only file and\n"
        << "                   replay it at startup (default: no persistence)\n"
        << "  --fsync POLICY   always | everysec | no (default everysec)\n";
}

bool parsePort(const char* text, std::uint16_t& port) {
    const char* end = text + std::strlen(text);
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text, end, value);
    if (ec != std::errc() || ptr != end || ptr == text || value > 65535) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

// Returns false (after printing why) if the arguments are invalid.
bool parseArgs(int argc, char** argv, Options& opts, bool& helpRequested) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            helpRequested = true;
            return true;
        }
        if (i + 1 >= argc) {
            std::cerr << "velokv: missing value for " << arg << '\n';
            return false;
        }
        const char* value = argv[++i];
        if (arg == "--port") {
            if (!parsePort(value, opts.port)) {
                std::cerr << "velokv: invalid port '" << value << "'\n";
                return false;
            }
        } else if (arg == "--bind") {
            opts.bindAddress = value;
        } else if (arg == "--aof") {
            opts.aofPath = value;
        } else if (arg == "--fsync") {
            const auto policy = parseFsyncPolicy(value);
            if (!policy) {
                std::cerr << "velokv: invalid fsync policy '" << value << "' (use always, everysec or no)\n";
                return false;
            }
            opts.fsync = *policy;
        } else {
            std::cerr << "velokv: unknown option " << arg << '\n';
            return false;
        }
    }
    return true;
}

void installSignalHandlers() {
    // A client that disconnects while we are writing to it must cost us that one
    // connection (write() returns EPIPE), not the whole process.
    ::signal(SIGPIPE, SIG_IGN);

    struct sigaction sa {};
    sa.sa_handler = onStopSignal;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    bool helpRequested = false;
    if (!parseArgs(argc, argv, opts, helpRequested)) {
        printUsage(std::cerr, argv[0]);
        return 2;
    }
    if (helpRequested) {
        printUsage(std::cout, argv[0]);
        return 0;
    }

    installSignalHandlers();

    try {
        // Declared before the keyspace so that it is destroyed after it: the
        // keyspace's expiry hook refers to it.
        std::optional<Aof> aof;
        Keyspace db;

        if (!opts.aofPath.empty()) {
            const auto start = std::chrono::steady_clock::now();
            const AofLoadResult loaded = loadAof(opts.aofPath, db);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::cerr << "velokv: loaded " << loaded.commands << " commands from " << opts.aofPath << " ("
                      << db.size() << " keys) in " << ms.count() << " ms" << std::endl;

            aof.emplace(opts.aofPath, opts.fsync);
            // A key that expires is logged as a DEL, so replaying the file later
            // reproduces exactly when it disappeared relative to other commands.
            db.setExpiredHook([&aof](const std::string& key) { aof->append({"DEL", key}); });
        }

        Server server(db, aof ? &*aof : nullptr, opts.bindAddress, opts.port);
        std::cerr << "velokv: listening on " << opts.bindAddress << ':' << server.port() << std::endl;
        server.run(gStopRequested);

        std::cerr << "velokv: shutting down" << std::endl;
        if (aof) aof->sync();
    } catch (const std::exception& e) {
        std::cerr << "velokv: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
