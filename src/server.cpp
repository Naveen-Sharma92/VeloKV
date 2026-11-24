// Listening socket setup, the epoll event loop and its periodic tick.
// Level-triggered epoll; every socket is non-blocking, so no call in the loop
// can stall the other clients.
#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "aof.h"
#include "commands.h"
#include "keyspace.h"
#include "resp.h"

namespace {

constexpr int kMaxEventsPerWait = 128;

// How often onTick() runs. It also bounds how long a shutdown request waits to
// be noticed. Redis's default (hz 10) is the same.
constexpr std::chrono::milliseconds kTick{100};

// Upper bound on keys deleted by the active sweep per tick, so a mass expiry
// (say, a million keys sharing one deadline) is spread over several ticks
// instead of freezing every client at once. Lazy expiry still hides expired
// keys from commands in the meantime.
constexpr std::size_t kMaxExpiredPerTick = 20000;

[[noreturn]] void throwErrno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

Server::Server(Keyspace& db, Aof* aof, const std::string& bindAddress, std::uint16_t port,
               std::size_t maxQueryBuffer)
    : db_(db), aof_(aof), maxQueryBuffer_(maxQueryBuffer) {
    if (aof_ != nullptr) propagate_ = [this](const Args& change) { aof_->append(change); };

    listenFd_ = Fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listenFd_.valid()) throwErrno("socket");

    // Without this, restarting the server fails with "Address already in use"
    // for as long as connections from the previous run sit in TIME_WAIT.
    const int one = 1;
    if (::setsockopt(listenFd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        throwErrno("setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 bind address: " + bindAddress);
    }
    if (::bind(listenFd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) < 0) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(),
                                "bind " + bindAddress + ":" + std::to_string(port));
    }
    if (::listen(listenFd_.get(), SOMAXCONN) < 0) throwErrno("listen");

    socklen_t len = sizeof addr;
    if (::getsockname(listenFd_.get(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        throwErrno("getsockname");
    }
    port_ = ntohs(addr.sin_port);

    epollFd_ = Fd(::epoll_create1(EPOLL_CLOEXEC));
    if (!epollFd_.valid()) throwErrno("epoll_create1");

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listenFd_.get();
    if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, listenFd_.get(), &ev) < 0) {
        throwErrno("epoll_ctl(ADD listener)");
    }
}

void Server::run(const std::atomic<bool>& stopRequested) {
    using Steady = std::chrono::steady_clock;
    std::array<epoll_event, kMaxEventsPerWait> events{};
    // The tick is scheduled on the monotonic clock, so a wall-clock jump (NTP, a
    // manual date change) cannot stall it or make it fire in a burst.
    auto nextTick = Steady::now() + kTick;

    while (!stopRequested.load()) {
        // Sleep until the next tick at most. Under constant traffic epoll_wait
        // returns early every time, so the tick is also checked after each batch.
        const auto untilTick = std::chrono::ceil<std::chrono::milliseconds>(nextTick - Steady::now());
        const int timeoutMs = static_cast<int>(std::clamp<long long>(untilTick.count(), 0, kTick.count()));

        const int n = ::epoll_wait(epollFd_.get(), events.data(), kMaxEventsPerWait, timeoutMs);
        if (n < 0 && errno != EINTR) throwErrno("epoll_wait");
        // EINTR means a signal (e.g. SIGTERM) arrived; the loop condition re-checks the flag.

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listenFd_.get()) {
                acceptClients();
            } else {
                handleClientEvent(fd, events[i].events);
            }
        }

        if (Steady::now() >= nextTick) {
            onTick();
            nextTick = Steady::now() + kTick;
        }
    }
}

void Server::onTick() {
    db_.expireDue(kMaxExpiredPerTick);  // expired keys reach the AOF as DELs
    if (aof_ != nullptr) {
        aof_->flush();
        aof_->tick();
    }
}

void Server::acceptClients() {
    // Drain the whole accept queue: under a burst of connections this costs one
    // epoll_wait instead of one per client.
    for (;;) {
        const int fd = ::accept4(listenFd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            // E.g. EMFILE (out of descriptors): keep serving existing clients.
            std::perror("velokv: accept4");
            return;
        }
        Fd clientFd(fd);

        // Replies are small and latency-sensitive. Nagle's algorithm would hold a
        // small reply back until the previous one is ACKed.
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::perror("velokv: epoll_ctl(ADD client)");
            continue;  // clientFd closes the socket
        }
        const auto it = conns_.try_emplace(fd, std::move(clientFd)).first;
        it->second.setRegisteredEvents(EPOLLIN);
    }
}

void Server::handleClientEvent(int fd, std::uint32_t events) {
    const auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Connection& conn = it->second;

    // EPOLLHUP and EPOLLERR are always reported; a read surfaces the actual cause
    // (EOF or an error such as ECONNRESET) through the normal path.
    if (!conn.closing() && (events & (EPOLLIN | EPOLLHUP | EPOLLERR))) {
        switch (conn.readOnce()) {
            case Connection::IoStatus::Ok:
                processInput(conn);
                break;
            case Connection::IoStatus::PeerClosed:
                conn.closeWhenFlushed();
                break;
            case Connection::IoStatus::Error:
                closeConnection(fd);
                return;
        }
    }

    // Persist first, reply second: once a client sees a reply to a write, the
    // change is already in the AOF (at least in the OS page cache).
    if (aof_ != nullptr) aof_->flush();

    // Write straight away instead of waiting for EPOLLOUT: the socket is almost
    // always writable, and this saves a full loop iteration per reply.
    if (conn.hasPendingOutput() && !conn.flush()) {
        closeConnection(fd);
        return;
    }

    if (conn.closing() && !conn.hasPendingOutput()) {
        closeConnection(fd);
        return;
    }

    if (!updateInterest(conn)) closeConnection(fd);
}

void Server::processInput(Connection& conn) {
    std::string& in = conn.input();
    std::size_t pos = 0;

    // One read can hold half a command or many pipelined ones, so run every
    // complete command and leave a partial one in the buffer for the next read.
    while (pos < in.size()) {
        resp::ParseResult cmd = resp::parseCommand(std::string_view(in).substr(pos));
        if (cmd.status == resp::ParseStatus::Incomplete) break;
        if (cmd.status == resp::ParseStatus::Error) {
            // We no longer know where the next command starts, so nothing else
            // from this client can be trusted: reply with the error and hang up.
            resp::appendError(conn.output(), "ERR " + cmd.error);
            conn.closeWhenFlushed();
            break;
        }
        pos += cmd.consumed;
        if (!cmd.args.empty()) executeCommand(db_, cmd.args, conn.output(), propagate_);
    }

    // Erase once per read, not once per command: a pipeline of N commands then
    // costs one memmove instead of N.
    in.erase(0, pos);

    // What is left is at most one partial command. Cap it so one client cannot
    // make us buffer without limit (e.g. by declaring many huge arguments).
    if (in.size() > maxQueryBuffer_) {
        std::fprintf(stderr, "velokv: closing client: query buffer over %zu bytes\n", maxQueryBuffer_);
        conn.closeWhenFlushed();
    }
}

bool Server::updateInterest(Connection& conn) {
    // Ask for EPOLLOUT only while output is pending. With level-triggered epoll an
    // always-registered EPOLLOUT would fire on every iteration and spin the CPU.
    // Once closing we stop asking for EPOLLIN: we will not read any more, and an
    // EOF stays readable forever, which would also spin.
    std::uint32_t wanted = 0;
    if (!conn.closing()) wanted |= EPOLLIN;
    if (conn.hasPendingOutput()) wanted |= EPOLLOUT;
    if (wanted == conn.registeredEvents()) return true;

    epoll_event ev{};
    ev.events = wanted;
    ev.data.fd = conn.fd();
    if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_MOD, conn.fd(), &ev) < 0) {
        std::perror("velokv: epoll_ctl(MOD)");
        return false;
    }
    conn.setRegisteredEvents(wanted);
    return true;
}

void Server::closeConnection(int fd) {
    // close() alone would remove the fd from epoll only if no other copy of the
    // descriptor exists (e.g. one inherited by a forked child), so remove it
    // explicitly first.
    ::epoll_ctl(epollFd_.get(), EPOLL_CTL_DEL, fd, nullptr);
    conns_.erase(fd);  // Fd's destructor closes the socket
}
