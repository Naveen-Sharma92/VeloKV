// Per-client state: the client's socket, bytes received but not yet processed,
// and output that is waiting for the socket to accept it. Knows how to move bytes
// between the socket and these buffers; knows nothing about epoll or the protocol.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "fd.h"

class Connection {
public:
    enum class IoStatus { Ok, PeerClosed, Error };

    explicit Connection(Fd fd) : fd_(std::move(fd)) {}

    int fd() const { return fd_.get(); }

    // Does a single read() into the input buffer. Ok also covers "nothing to
    // read right now" (EAGAIN); PeerClosed means the client sent FIN.
    IoStatus readOnce();

    // Writes pending output until it is all sent or the socket would block.
    // Returns false if the connection is broken (e.g. the client reset it).
    bool flush();

    // Bytes received but not yet consumed by the protocol layer.
    std::string& input() { return in_; }

    // Replies are appended here; flush() sends them.
    std::string& output() { return out_; }
    bool hasPendingOutput() const { return sent_ < out_.size(); }

    // "Stop reading, deliver what is queued, then close." Used when the client
    // closes its sending side, and after a protocol error.
    bool closing() const { return closing_; }
    void closeWhenFlushed() { closing_ = true; }

    // The epoll event mask currently registered for this socket, cached so the
    // server only calls epoll_ctl when the mask actually changes.
    std::uint32_t registeredEvents() const { return registeredEvents_; }
    void setRegisteredEvents(std::uint32_t events) { registeredEvents_ = events; }

private:
    Fd fd_;
    std::string in_;
    std::string out_;
    std::size_t sent_ = 0;  // bytes at the front of out_ already written to the socket
    bool closing_ = false;
    std::uint32_t registeredEvents_ = 0;
};
