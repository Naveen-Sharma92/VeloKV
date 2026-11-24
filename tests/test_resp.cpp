// Unit tests for the RESP parser and encoders: complete, split, pipelined,
// binary and inline input, every protocol error, and every reply type.
#include "resp.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

using resp::ParseStatus;
using Args = std::vector<std::string>;

namespace {

std::string bulk(const std::string& s) { return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n"; }

std::string command(const Args& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) out += bulk(a);
    return out;
}

}  // namespace

// ------------------------------------------------------------------ multibulk

TEST(RespParser, ParsesACompleteCommand) {
    const std::string input = "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";
    const auto r = resp::parseCommand(input);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.consumed, input.size());
    EXPECT_EQ(r.args, (Args{"ECHO", "hello"}));
}

TEST(RespParser, EveryPrefixOfACommandIsIncompleteAndConsumesNothing) {
    // Simulates the command arriving split at every possible byte boundary.
    const std::string input = command({"SET", "key", "some value"});
    for (std::size_t len = 0; len < input.size(); ++len) {
        const auto r = resp::parseCommand(std::string_view(input).substr(0, len));
        EXPECT_EQ(r.status, ParseStatus::Incomplete) << "prefix length " << len;
        EXPECT_EQ(r.consumed, 0u) << "prefix length " << len;
    }
    const auto r = resp::parseCommand(input);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.args, (Args{"SET", "key", "some value"}));
}

TEST(RespParser, PipelinedCommandsAreParsedOneAtATime) {
    const Args a{"PING"}, b{"ECHO", "x"}, c{"SET", "k", "v"};
    const std::string input = command(a) + command(b) + command(c) + "*1\r\n$4\r\nPI";  // + a partial 4th

    std::vector<Args> parsed;
    std::size_t pos = 0;
    for (;;) {
        const auto r = resp::parseCommand(std::string_view(input).substr(pos));
        if (r.status != ParseStatus::Complete) {
            EXPECT_EQ(r.status, ParseStatus::Incomplete);
            break;
        }
        parsed.push_back(r.args);
        pos += r.consumed;
    }
    EXPECT_EQ(parsed, (std::vector<Args>{a, b, c}));
    EXPECT_EQ(input.substr(pos), "*1\r\n$4\r\nPI");  // the partial command is left untouched
}

TEST(RespParser, BulkStringsAreBinarySafe) {
    const std::string value("a\r\nb\0c", 6);  // CRLF and NUL inside the value
    const auto r = resp::parseCommand(command({"ECHO", value}));
    ASSERT_EQ(r.status, ParseStatus::Complete);
    ASSERT_EQ(r.args.size(), 2u);
    EXPECT_EQ(r.args[1], value);
}

TEST(RespParser, EmptyBulkString) {
    const auto r = resp::parseCommand("*2\r\n$4\r\nECHO\r\n$0\r\n\r\n");
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.args, (Args{"ECHO", ""}));
}

TEST(RespParser, ZeroOrNegativeCountIsAnEmptyCommand) {
    for (const std::string input : {"*0\r\n", "*-1\r\n"}) {
        const auto r = resp::parseCommand(input);
        ASSERT_EQ(r.status, ParseStatus::Complete) << input;
        EXPECT_EQ(r.consumed, input.size());
        EXPECT_TRUE(r.args.empty());
    }
}

// ------------------------------------------------------------------ inline

TEST(RespParser, InlineCommand) {
    const auto r = resp::parseCommand("PING\r\n");
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.consumed, 6u);
    EXPECT_EQ(r.args, (Args{"PING"}));
}

TEST(RespParser, InlineCommandSplitsOnWhitespaceAndAcceptsBareNewline) {
    const auto r = resp::parseCommand("  SET\tkey   value \n");
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.args, (Args{"SET", "key", "value"}));
}

TEST(RespParser, EmptyInlineLineIsSkipped) {
    const auto r = resp::parseCommand("\r\nPING\r\n");
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.consumed, 2u);
    EXPECT_TRUE(r.args.empty());
}

TEST(RespParser, InlineWithoutNewlineIsIncomplete) {
    EXPECT_EQ(resp::parseCommand("PIN").status, ParseStatus::Incomplete);
    EXPECT_EQ(resp::parseCommand("PING\r").status, ParseStatus::Incomplete);
}

// ------------------------------------------------------------------ errors

struct ErrorCase {
    std::string input;
    std::string message;
};

TEST(RespParser, MalformedInputGivesRedisProtocolErrors) {
    const std::vector<ErrorCase> cases = {
        {"*x\r\n", "Protocol error: invalid multibulk length"},
        {"*1 \r\n", "Protocol error: invalid multibulk length"},
        {"*" + std::to_string(resp::kMaxArgs + 1) + "\r\n", "Protocol error: invalid multibulk length"},
        {"*1\r\n:5\r\n", "Protocol error: expected '$', got ':'"},
        {"*1\r\n$x\r\n", "Protocol error: invalid bulk length"},
        {"*1\r\n$-1\r\n", "Protocol error: invalid bulk length"},
        {"*1\r\n$4\r\nPINGxx", "Protocol error: expected '\\r\\n' after bulk string"},
    };
    for (const auto& c : cases) {
        const auto r = resp::parseCommand(c.input);
        EXPECT_EQ(r.status, ParseStatus::Error) << c.input;
        EXPECT_EQ(r.error, c.message) << c.input;
    }
}

TEST(RespParser, OversizedBulkIsRejectedFromTheHeaderAlone) {
    // Only the header has arrived; the 512 MiB + 1 body never needs to be buffered.
    const auto r = resp::parseCommand("*2\r\n$3\r\nSET\r\n$" + std::to_string(resp::kMaxBulkLength + 1) + "\r\n");
    EXPECT_EQ(r.status, ParseStatus::Error);
    EXPECT_EQ(r.error, "Protocol error: invalid bulk length");
}

TEST(RespParser, BulkAtTheMaximumLengthIsStillAccepted) {
    const auto r = resp::parseCommand("*1\r\n$" + std::to_string(resp::kMaxBulkLength) + "\r\n");
    EXPECT_EQ(r.status, ParseStatus::Incomplete);
}

TEST(RespParser, LinesWithoutTerminatorAreCapped) {
    const std::string longLine(resp::kMaxInlineLength + 1, 'a');
    EXPECT_EQ(resp::parseCommand(longLine).error, "Protocol error: too big inline request");
    EXPECT_EQ(resp::parseCommand("*" + longLine).error, "Protocol error: too big mbulk count string");
    EXPECT_EQ(resp::parseCommand("*1\r\n$" + longLine).error, "Protocol error: too big bulk count string");

    // Just under the cap is still "waiting for more".
    EXPECT_EQ(resp::parseCommand(std::string(resp::kMaxInlineLength, 'a')).status, ParseStatus::Incomplete);
}

// ------------------------------------------------------------------ encoders

TEST(RespEncoder, EncodesEveryReplyType) {
    std::string out;
    resp::appendSimpleString(out, "OK");
    resp::appendError(out, "ERR boom");
    resp::appendInteger(out, -42);
    resp::appendBulkString(out, "hi");
    resp::appendBulkString(out, "");
    resp::appendNull(out);
    resp::appendArrayHeader(out, 0);
    EXPECT_EQ(out, "+OK\r\n-ERR boom\r\n:-42\r\n$2\r\nhi\r\n$0\r\n\r\n$-1\r\n*0\r\n");
}

TEST(RespEncoder, IntegerExtremes) {
    std::string out;
    resp::appendInteger(out, 9223372036854775807LL);
    resp::appendInteger(out, -9223372036854775807LL - 1);
    EXPECT_EQ(out, ":9223372036854775807\r\n:-9223372036854775808\r\n");
}

TEST(RespEncoder, ErrorsCannotInjectExtraReplies) {
    // An argument echoed inside an error must not be able to end the line early.
    std::string out;
    resp::appendError(out, "ERR bad 'x\r\n+OK'");
    EXPECT_EQ(out, "-ERR bad 'x  +OK'\r\n");
}

TEST(RespEncoder, EncodedCommandRoundTripsThroughTheParser) {
    std::string out;
    resp::appendArrayHeader(out, 2);
    resp::appendBulkString(out, "ECHO");
    resp::appendBulkString(out, std::string("bin\0ary\r\n", 9));
    const auto r = resp::parseCommand(out);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.args, (Args{"ECHO", std::string("bin\0ary\r\n", 9)}));
}
