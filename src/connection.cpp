// Socket I/O for one client: a single non-blocking read per readiness event, and
// writes that stop at EAGAIN and leave the rest in the output buffer.
#include "connection.h"

#include <sys/socket.h>

#include <cerrno>

namespace {
// Same size Redis uses for its read buffer: large enough for a deep pipeline of
// small commands, small enough to live on the stack.
constexpr std::size_t kReadChunk = 16 * 1024;
}  // namespace

Connection::IoStatus Connection::readOnce() {
    // Only one read per event: epoll is level-triggered, so if more data is
    // waiting the socket is reported again on the next loop iteration. This keeps
    // one fast client from starving the others.
    char buf[kReadChunk];
    const ssize_t n = ::recv(fd_.get(), buf, sizeof buf, 0);
    if (n > 0) {
        in_.append(buf, static_cast<std::size_t>(n));
        return IoStatus::Ok;
    }
    if (n == 0) return IoStatus::PeerClosed;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IoStatus::Ok;
    return IoStatus::Error;
}

bool Connection::flush() {
    while (sent_ < out_.size()) {
        // MSG_NOSIGNAL: writing to a socket the peer has reset must return EPIPE
        // instead of raising SIGPIPE, which would kill the whole process. main()
        // also ignores SIGPIPE; this keeps the class safe on its own (e.g. in tests).
        const ssize_t n = ::send(fd_.get(), out_.data() + sent_, out_.size() - sent_, MSG_NOSIGNAL);
        if (n > 0) {
            sent_ += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // kernel buffer full
        return false;
    }

    if (sent_ == out_.size()) {
        out_.clear();
        sent_ = 0;
    } else if (sent_ > out_.size() / 2) {
        // Drop the sent prefix only once it outweighs what is left, so each byte is
        // moved at most about once instead of on every partial write.
        out_.erase(0, sent_);
        sent_ = 0;
    }
    return true;
}
