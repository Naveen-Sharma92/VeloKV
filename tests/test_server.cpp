// In-process tests for the event loop: a Server runs on a kernel-chosen port in a
// background thread, and the test talks RESP to it through blocking sockets.
#include "server.h"

#include "keyspace.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace {

// Starts the server on construction and stops it (within one tick) on destruction.
class RunningServer {
public:
    explicit RunningServer(std::size_t maxQueryBuffer = Server::kDefaultMaxQueryBuffer)
        : server_(db_, nullptr, "127.0.0.1", 0, maxQueryBuffer), thread_([this] { server_.run(stop_); }) {}
    ~RunningServer() {
        stop_.store(true);
        thread_.join();
    }
    std::uint16_t port() const { return server_.port(); }

private:
    std::atomic<bool> stop_{false};
    Keyspace db_;  // declared before server_, so it is built first and destroyed last
    Server server_;
    std::thread thread_;
};

Fd connectTo(std::uint16_t port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) return Fd();
    // A broken server should fail the test, not hang it.
    timeval timeout{5, 0};
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    return fd;
}

bool sendAll(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

// Reads until `n` bytes arrive, the peer closes, or the receive timeout expires.
std::string recvExactly(int fd, std::size_t n) {
    std::string result;
    char buf[64 * 1024];
    while (result.size() < n) {
        const std::size_t want = std::min(sizeof buf, n - result.size());
        const ssize_t got = ::recv(fd, buf, want, 0);
        if (got <= 0) break;
        result.append(buf, static_cast<std::size_t>(got));
    }
    return result;
}

// True if the server closed the connection (EOF or reset), false on timeout.
bool serverClosed(int fd) {
    char c = 0;
    const ssize_t n = ::recv(fd, &c, 1, 0);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

std::string bulkCommand(std::string_view name, std::string_view arg) {
    return "*2\r\n$" + std::to_string(name.size()) + "\r\n" + std::string(name) + "\r\n$" +
           std::to_string(arg.size()) + "\r\n" + std::string(arg) + "\r\n";
}

const std::string kPing = "*1\r\n$4\r\nPING\r\n";
const std::string kPong = "+PONG\r\n";

}  // namespace

TEST(Server, AnswersPing) {
    RunningServer server;
    Fd client = connectTo(server.port());
    ASSERT_TRUE(client.valid());

    ASSERT_TRUE(sendAll(client.get(), kPing));
    EXPECT_EQ(recvExactly(client.get(), kPong.size()), kPong);
}

TEST(Server, AcceptsInlineCommands) {
    RunningServer server;
    Fd client = connectTo(server.port());
    ASSERT_TRUE(client.valid());

    ASSERT_TRUE(sendAll(client.get(), "PING\r\nECHO hi\n"));
    EXPECT_EQ(recvExactly(client.get(), 15), "+PONG\r\n$2\r\nhi\r\n");
}

TEST(Server, ServesTwoClientsConnectedAtTheSameTime) {
    RunningServer server;
    Fd a = connectTo(server.port());
    Fd b = connectTo(server.port());
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());

    // Both connections stay open; B is answered while A is still waiting.
    ASSERT_TRUE(sendAll(a.get(), bulkCommand("ECHO", "from-a")));
    ASSERT_TRUE(sendAll(b.get(), bulkCommand("ECHO", "from-b")));
    EXPECT_EQ(recvExactly(b.get(), 12), "$6\r\nfrom-b\r\n");
    EXPECT_EQ(recvExactly(a.get(), 12), "$6\r\nfrom-a\r\n");
}

TEST(Server, PipelinedCommandsInOneWriteAllGetRepliesInOrder) {
    RunningServer server;
    Fd client = connectTo(server.port());
    ASSERT_TRUE(client.valid());

    std::string batch;
    std::string expected;
    for (int i = 0; i < 1000; ++i) {
        const std::string value = "v" + std::to_string(i);
        batch += bulkCommand("ECHO", value);
        expected += "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
    }
    ASSERT_TRUE(sendAll(client.get(), batch));
    EXPECT_EQ(recvExactly(client.get(), expected.size()), expected);
}

TEST(Server, CommandSplitAcrossWritesIsReassembled) {
    RunningServer server;
    Fd client = connectTo(server.port());
    ASSERT_TRUE(client.valid());

    const std::string cmd = bulkCommand("ECHO", "split-me");
    const std::size_t cut = cmd.size() / 2;  // in the middle of the argument header
    ASSERT_TRUE(sendAll(client.get(), cmd.substr(0, cut)));
    // Long enough for the server to read and parse the first half on its own.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(sendAll(client.get(), cmd.substr(cut)));
    EXPECT_EQ(recvExactly(client.get(), 14), "$8\r\nsplit-me\r\n");
}

TEST(Server, LargeReplySurvivesPartialWrites) {
    // 16 MiB is far more than the loopback socket buffers hold, so the server's
    // send() must hit EAGAIN, keep the rest buffered and finish via EPOLLOUT.
    RunningServer server;
    Fd client = connectTo(server.port());
    ASSERT_TRUE(client.valid());

    std::string payload(16 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>('a' + i % 26);
    const std::string expected = "$" + std::to_string(payload.size()) + "\r\n" + payload + "\r\n";

    ASSERT_TRUE(sendAll(client.get(), bulkCommand("ECHO", payload)));
    const std::string reply = recvExactly(client.get(), expected.size());
    ASSERT_EQ(reply.size(), expected.size());
    EXPECT_TRUE(reply == expected);  // EXPECT_EQ would print 16 MiB on failure
}

TEST(Server, AbruptDisconnectDoesNotAffectOtherClients) {
    RunningServer server;
    Fd survivor = connectTo(server.port());
    ASSERT_TRUE(survivor.valid());
    {
        // Ask for a big reply and close without reading it: the server's send()
        // now targets a reset connection and fails with EPIPE/ECONNRESET.
        Fd rude = connectTo(server.port());
        ASSERT_TRUE(rude.valid());
        ASSERT_TRUE(sendAll(rude.get(), bulkCommand("ECHO", std::string(4 * 1024 * 1024, 'x'))));
    }
    ASSERT_TRUE(sendAll(survivor.get(), kPing));
    EXPECT_EQ(recvExactly(survivor.get(), kPong.size()), kPong);
}

TEST(Server, DeliversPendingRepliesAfterClientHalfCloses) {
    // Like `printf ... | nc -N`: the client sends, shuts down its write side and
    // keeps reading. Queued replies must still be delivered before we close.
    RunningServer server;
    Fd client = connectTo(server.port());
    ASSERT_TRUE(client.valid());

    const std::string payload(16 * 1024 * 1024, 'y');
    const std::size_t expectedSize = payload.size() + std::to_string(payload.size()).size() + 5;
    ASSERT_TRUE(sendAll(client.get(), bulkCommand("ECHO", payload)));
    ASSERT_EQ(::shutdown(client.get(), SHUT_WR), 0);
    // Give the server time to read all input and the EOF while most of the reply
    // is still queued, so the "closing, output pending" path really runs.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(recvExactly(client.get(), expectedSize).size(), expectedSize);
    EXPECT_TRUE(serverClosed(client.get()));
}

TEST(Server, ProtocolErrorRepliesThenCloses) {
    RunningServer server;
    Fd client = connectTo(server.port());
    ASSERT_TRUE(client.valid());

    // A valid command, a malformed one, then one that must never run.
    ASSERT_TRUE(sendAll(client.get(), kPing + "*1\r\n:5\r\n" + kPing));
    const std::string expected = kPong + "-ERR Protocol error: expected '$', got ':'\r\n";
    EXPECT_EQ(recvExactly(client.get(), expected.size()), expected);
    EXPECT_TRUE(serverClosed(client.get()));
}

TEST(Server, ClientOverTheQueryBufferLimitIsDisconnected) {
    RunningServer server(/*maxQueryBuffer=*/1024 * 1024);
    Fd hog = connectTo(server.port());
    Fd other = connectTo(server.port());
    ASSERT_TRUE(hog.valid());
    ASSERT_TRUE(other.valid());

    // Declares a 4 MiB argument (legal on its own) and starts sending it; the
    // partial command passes the 1 MiB cap and the server hangs up.
    std::string partial = "*2\r\n$4\r\nECHO\r\n$4194304\r\n" + std::string(2 * 1024 * 1024, 'z');
    sendAll(hog.get(), partial);  // may fail midway once the server closes
    EXPECT_TRUE(serverClosed(hog.get()));

    ASSERT_TRUE(sendAll(other.get(), kPing));
    EXPECT_EQ(recvExactly(other.get(), kPong.size()), kPong);
}
