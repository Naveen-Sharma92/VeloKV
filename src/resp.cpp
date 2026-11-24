// RESP2 parsing and encoding. The parser is stateless: each call re-reads the
// buffer from the start of the current command, which keeps it a pure function
// of its input and therefore easy to test with split and pipelined data.
#include "resp.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace resp {
namespace {

ParseResult incomplete() { return ParseResult{}; }

ParseResult error(std::string message) {
    ParseResult r;
    r.status = ParseStatus::Error;
    r.error = std::move(message);
    return r;
}

ParseResult complete(std::size_t consumed, const std::vector<std::string_view>& args) {
    ParseResult r;
    r.status = ParseStatus::Complete;
    r.consumed = consumed;
    r.args.assign(args.begin(), args.end());
    return r;
}

// Strict decimal: optional '-', digits only, nothing else (no spaces, no '+').
bool parseInteger(std::string_view text, long long& value) {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc() && ptr == last && first != last;
}

bool isInlineSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

// "PING\r\n", "ECHO hello\n": what you type into nc or telnet.
ParseResult parseInline(std::string_view input) {
    const std::size_t newline = input.find('\n');
    if (newline == std::string_view::npos) {
        // Without a cap, a client that never sends '\n' grows the buffer forever.
        if (input.size() > kMaxInlineLength) return error("Protocol error: too big inline request");
        return incomplete();
    }

    std::vector<std::string_view> args;
    const std::string_view line = input.substr(0, newline);
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isInlineSpace(line[i])) ++i;
        const std::size_t start = i;
        while (i < line.size() && !isInlineSpace(line[i])) ++i;
        if (i > start) args.push_back(line.substr(start, i - start));
    }
    return complete(newline + 1, args);
}

// "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n": what every Redis client library sends.
ParseResult parseMultibulk(std::string_view input) {
    std::size_t lineEnd = input.find("\r\n");
    if (lineEnd == std::string_view::npos) {
        if (input.size() > kMaxInlineLength) return error("Protocol error: too big mbulk count string");
        return incomplete();
    }
    long long count = 0;
    if (!parseInteger(input.substr(1, lineEnd - 1), count) || count > kMaxArgs) {
        return error("Protocol error: invalid multibulk length");
    }
    std::size_t pos = lineEnd + 2;
    if (count <= 0) return complete(pos, {});

    std::vector<std::string_view> args;
    // The count comes from the client, so never let it size an allocation directly.
    args.reserve(static_cast<std::size_t>(std::min(count, 1024LL)));

    for (long long i = 0; i < count; ++i) {
        if (pos >= input.size()) return incomplete();
        if (input[pos] != '$') {
            return error(std::string("Protocol error: expected '$', got '") + input[pos] + "'");
        }
        lineEnd = input.find("\r\n", pos);
        if (lineEnd == std::string_view::npos) {
            if (input.size() - pos > kMaxInlineLength) return error("Protocol error: too big bulk count string");
            return incomplete();
        }
        long long length = 0;
        if (!parseInteger(input.substr(pos + 1, lineEnd - pos - 1), length) || length < 0 ||
            static_cast<unsigned long long>(length) > kMaxBulkLength) {
            // Rejected from the header alone, before a single byte of the body is buffered.
            return error("Protocol error: invalid bulk length");
        }
        pos = lineEnd + 2;

        const auto len = static_cast<std::size_t>(length);
        if (input.size() - pos < len + 2) return incomplete();
        // Bulk strings are length-prefixed, so the body may contain anything,
        // including "\r\n". Only the two bytes after it must be the terminator.
        if (input[pos + len] != '\r' || input[pos + len + 1] != '\n') {
            return error("Protocol error: expected '\\r\\n' after bulk string");
        }
        args.push_back(input.substr(pos, len));
        pos += len + 2;
    }
    return complete(pos, args);
}

// Simple strings and errors are terminated by "\r\n", so a CR or LF inside one
// (for example an argument echoed back in an error) would let a client forge
// extra replies. Redis replaces them with spaces too.
void appendLine(std::string& out, char prefix, std::string_view text) {
    out += prefix;
    for (const char c : text) out += (c == '\r' || c == '\n') ? ' ' : c;
    out += "\r\n";
}

void appendLength(std::string& out, char prefix, long long value) {
    char buf[24];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, value);
    (void)ec;  // 24 bytes always fit a 64-bit integer
    out += prefix;
    out.append(buf, ptr);
    out += "\r\n";
}

// Redis 7.2's d2string(): the text a score is sent as. Integral values print as
// integers ("3", not "3.0"); everything else prints the shortest digits that
// read back as the same double, laid out like Redis's fpconv_dtoa (plain
// decimal for moderate exponents, otherwise "1.5e+30").
std::string formatDouble(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    if (value == 0) return std::signbit(value) ? "-0" : "0";
    if (value >= static_cast<double>(-LLONG_MAX / 2) && value <= static_cast<double>(LLONG_MAX / 2)) {
        const auto asInteger = static_cast<long long>(value);
        if (static_cast<double>(asInteger) == value) return std::to_string(asInteger);
    }

    // Shortest round-trip digits, e.g. "1.2345e+20" -> digits "12345", exponent 20.
    char sci[64];
    const auto result = std::to_chars(sci, sci + sizeof sci, std::fabs(value), std::chars_format::scientific);
    const std::string_view text(sci, static_cast<std::size_t>(result.ptr - sci));
    const std::size_t e = text.find('e');
    std::string digits;
    for (const char c : text.substr(0, e)) {
        if (c != '.') digits += c;
    }
    const int exponent = std::atoi(std::string(text.substr(e + 1)).c_str());

    const int ndigits = static_cast<int>(digits.size());
    const int k = exponent - (ndigits - 1);  // value = digits * 10^k
    const int absExp = std::abs(exponent);
    std::string out = value < 0 ? "-" : "";

    if (k >= 0 && absExp < ndigits + 7) {  // integer with trailing zeros
        out += digits + std::string(static_cast<std::size_t>(k), '0');
    } else if (k < 0 && (k > -7 || absExp < 4)) {  // plain decimal
        const int intDigits = ndigits + k;
        if (intDigits <= 0) {
            out += "0." + std::string(static_cast<std::size_t>(-intDigits), '0') + digits;
        } else {
            out += digits.substr(0, static_cast<std::size_t>(intDigits)) + "." +
                   digits.substr(static_cast<std::size_t>(intDigits));
        }
    } else {  // scientific, exponent without leading zeros
        out += digits.substr(0, 1);
        if (ndigits > 1) out += "." + digits.substr(1);
        out += exponent < 0 ? "e-" : "e+";
        out += std::to_string(absExp);
    }
    return out;
}

}  // namespace

ParseResult parseCommand(std::string_view input) {
    if (input.empty()) return incomplete();
    if (input[0] == '*') return parseMultibulk(input);
    return parseInline(input);
}

void appendSimpleString(std::string& out, std::string_view s) { appendLine(out, '+', s); }

void appendError(std::string& out, std::string_view message) { appendLine(out, '-', message); }

void appendInteger(std::string& out, long long value) { appendLength(out, ':', value); }

void appendBulkString(std::string& out, std::string_view s) {
    appendLength(out, '$', static_cast<long long>(s.size()));
    out.append(s);
    out += "\r\n";
}

void appendNull(std::string& out) { out += "$-1\r\n"; }

void appendDouble(std::string& out, double value) { appendBulkString(out, formatDouble(value)); }

void appendArrayHeader(std::string& out, std::size_t count) {
    appendLength(out, '*', static_cast<long long>(count));
}

void appendNullArray(std::string& out) { out += "*-1\r\n"; }

}  // namespace resp
