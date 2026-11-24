// RAII owner of a POSIX file descriptor (socket, epoll instance, file).
// Move-only, so exactly one object owns each descriptor and it is closed exactly
// once, even when an exception unwinds the stack.
#pragma once

#include <unistd.h>

#include <utility>

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { closeIfOpen(); }

    // Copying would mean two owners closing the same number twice; the second
    // close could hit an unrelated descriptor that reused that number.
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            closeIfOpen();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

private:
    void closeIfOpen() noexcept {
        // On Linux the descriptor is released even if close() reports an error,
        // so there is nothing useful to do with a failure here.
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
};
