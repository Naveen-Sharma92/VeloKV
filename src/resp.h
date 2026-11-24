// RESP2, the Redis wire protocol: an incremental request parser (multibulk
// arrays of bulk strings, plus plain-text inline commands) and encoders that
// append replies to an output buffer.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace resp {

// Limits that stop one client from making the server allocate without bound.
// The first two match Redis's defaults (proto-max-bulk-len, PROTO_INLINE_MAX_SIZE).
constexpr std::size_t kMaxBulkLength = 512 * 1024 * 1024;
constexpr std::size_t kMaxInlineLength = 64 * 1024;
constexpr long long kMaxArgs = 1024 * 1024;

enum class ParseStatus { Complete, Incomplete, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;
    std::size_t consumed = 0;       // Complete: bytes this command occupied
    std::vector<std::string> args;  // Complete: command name + arguments (may be empty)
    std::string error;              // Error: e.g. "Protocol error: invalid bulk length"
};

// Parses one command from the front of `input`. Never consumes anything by
// itself: the caller removes `consumed` bytes after a Complete result. On
// Incomplete the caller waits for more bytes and calls again with the longer
// buffer. A Complete result with no args is an empty line or "*0", which is
// skipped without a reply, as Redis does.
ParseResult parseCommand(std::string_view input);

// Reply encoders. Each appends one RESP value to `out`.
void appendSimpleString(std::string& out, std::string_view s);  // +OK
void appendError(std::string& out, std::string_view message);   // -ERR ... (message includes the code)
void appendInteger(std::string& out, long long value);          // :42
void appendBulkString(std::string& out, std::string_view s);    // $5 hello
void appendNull(std::string& out);                              // $-1 (nil)
void appendDouble(std::string& out, double value);              // $3 1.5, formatted like Redis 7.2 (d2string)
void appendArrayHeader(std::string& out, std::size_t count);    // *N, followed by N values
void appendNullArray(std::string& out);                         // *-1 (nil array, e.g. LPOP key 2 on a missing key)

}  // namespace resp
