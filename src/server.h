// The network front end: owns the listening socket, the epoll instance and every
// client connection, and runs the single-threaded event loop that accepts
// clients, reads requests, writes replies and closes connections.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "commands.h"
#include "connection.h"
#include "fd.h"

class Aof;
class Keyspace;

class Server {
public:
    // Redis's default client-query-buffer-limit.
    static constexpr std::size_t kDefaultMaxQueryBuffer = 1024 * 1024 * 1024;

    // Binds and listens immediately, so a busy port or bad address fails at
    // startup with a clear error. Port 0 lets the kernel pick a free port.
    // maxQueryBuffer caps the unprocessed bytes held for one client. The server
    // runs commands against `db` and logs their changes to `aof` (nullptr: no
    // persistence); both must outlive it.
    Server(Keyspace& db, Aof* aof, const std::string& bindAddress, std::uint16_t port,
           std::size_t maxQueryBuffer = kDefaultMaxQueryBuffer);

    // Not copyable or movable: propagate_ captures `this`.
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // The port actually bound (differs from the requested one when it was 0).
    std::uint16_t port() const { return port_; }

    // Runs the event loop until `stopRequested` becomes true. It is checked at
    // least once per tick, so shutdown takes at most about one tick.
    void run(const std::atomic<bool>& stopRequested);

private:
    // Periodic work, run about every 100 ms: the active expiry sweep and the
    // AOF's once-a-second fsync.
    void onTick();
    void acceptClients();
    void handleClientEvent(int fd, std::uint32_t events);
    void processInput(Connection& conn);
    bool updateInterest(Connection& conn);
    void closeConnection(int fd);

    Keyspace& db_;
    Aof* aof_;
    Propagate propagate_;  // sends command effects to the AOF (empty if none)
    Fd listenFd_;
    Fd epollFd_;
    std::uint16_t port_ = 0;
    std::size_t maxQueryBuffer_;
    // Keyed by fd because that is what an epoll event hands back to us.
    std::unordered_map<int, Connection> conns_;
};
