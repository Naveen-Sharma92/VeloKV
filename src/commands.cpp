// Command dispatch: a table from lowercase name to handler and arity, plus the
// handlers themselves. Error texts match Redis, because client libraries and
// people compare against them. Write handlers report their effect through
// Ctx::propagate, which is what ends up in the AOF.
#include "commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

#include "keyspace.h"
#include "resp.h"

namespace {

// Everything a handler needs: the data, where the reply goes, and where its
// changes go.
struct Ctx {
    Keyspace& db;
    std::string& out;
    const Propagate& propagateFn;

    // Call only after the keyspace really changed, with a command that redoes
    // exactly that change.
    void propagate(const Args& change) const {
        if (propagateFn) propagateFn(change);
    }
};

using Handler = void (*)(Ctx& ctx, const Args& args);

struct Command {
    Handler handler;
    // Redis's convention, counting the command name itself: N means exactly N
    // words, -N means at least N. One number covers every command's validation.
    int arity;
};

constexpr std::string_view kNotAnInteger = "ERR value is not an integer or out of range";
constexpr std::string_view kSyntaxError = "ERR syntax error";

// ------------------------------------------------------------------ helpers

std::string toLower(std::string_view s) {
    std::string lower(s);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// Same rules as Redis's string2ll, so "INCR" accepts exactly what Redis accepts:
// optional '-', digits only, no '+', no spaces, no leading zeros ("0" itself is
// fine, "01" and "-0" are not), and the value must fit in 64 bits.
bool parseInt64(std::string_view s, long long& value) {
    if (s.empty()) return false;
    const std::size_t firstDigit = s[0] == '-' ? 1 : 0;
    if (firstDigit == s.size()) return false;
    if (s[firstDigit] == '0' && s.size() > 1) return false;
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value);
    return ec == std::errc() && ptr == end;
}

// Same rules as Redis's string2d (strtod plus checks): no leading space, no
// trailing junk, no NaN, no overflow to infinity; "inf", "-inf" and "1e3" are
// fine. The C locale is in effect (we never call setlocale), so '.' is the
// decimal point.
bool parseScore(const std::string& s, double& value) {
    if (s.empty() || std::isspace(static_cast<unsigned char>(s[0]))) return false;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;  // also catches an embedded '\0'
    if (errno == ERANGE && (parsed == HUGE_VAL || parsed == -HUGE_VAL || parsed == 0.0)) return false;
    if (std::isnan(parsed)) return false;
    value = parsed;
    return true;
}

void appendArityError(std::string& out, std::string_view name) {
    resp::appendError(out, "ERR wrong number of arguments for '" + std::string(name) + "' command");
}

// Same text as Redis: "unknown command 'foo', with args beginning with: 'a' 'b' ".
void appendUnknownCommandError(std::string& out, const Args& args) {
    constexpr std::size_t kMaxShown = 128;
    std::string shownArgs;
    for (std::size_t i = 1; i < args.size() && shownArgs.size() < kMaxShown; ++i) {
        const std::size_t room = kMaxShown - shownArgs.size();
        shownArgs += "'" + args[i].substr(0, room) + "' ";
    }
    resp::appendError(out, "ERR unknown command '" + args[0].substr(0, kMaxShown) +
                               "', with args beginning with: " + shownArgs);
}

void appendOk(std::string& out) { resp::appendSimpleString(out, "OK"); }

void appendInvalidExpireError(std::string& out, std::string_view command) {
    resp::appendError(out, "ERR invalid expire time in '" + std::string(command) + "' command");
}

// deadline = base + amount * unitMs, or false if any step would overflow 64 bits.
// Checked before computing, because signed overflow is undefined behaviour.
bool deadlineFrom(long long amount, long long unitMs, std::int64_t base, std::int64_t& deadline) {
    if (amount > LLONG_MAX / unitMs || amount < LLONG_MIN / unitMs) return false;
    const long long ms = amount * unitMs;
    if (ms > LLONG_MAX - base) return false;
    deadline = base + ms;
    return true;
}

// Reports a new or changed TTL in absolute form, so replaying the log later
// cannot give the key extra time.
void propagateExpiry(Ctx& ctx, const std::string& key, ExpiryResult result, std::int64_t deadline) {
    if (result == ExpiryResult::Scheduled) {
        ctx.propagate({"PEXPIREAT", key, std::to_string(deadline)});
    } else if (result == ExpiryResult::DeletedNow) {
        ctx.propagate({"DEL", key});
    }
}

// ------------------------------------------------------------------ connection

// PING [message]
void cmdPing(Ctx& ctx, const Args& args) {
    if (args.size() > 2) {
        appendArityError(ctx.out, "ping");
        return;
    }
    if (args.size() == 1) {
        resp::appendSimpleString(ctx.out, "PONG");
    } else {
        resp::appendBulkString(ctx.out, args[1]);
    }
}

// ECHO message
void cmdEcho(Ctx& ctx, const Args& args) { resp::appendBulkString(ctx.out, args[1]); }

// COMMAND [subcommand ...]. redis-cli 7+ sends "COMMAND DOCS" when it starts, to
// show argument hints. An empty array just means "no docs", and it carries on.
void cmdCommand(Ctx& ctx, const Args&) { resp::appendArrayHeader(ctx.out, 0); }

// CONFIG GET parameter. redis-benchmark asks for "save" and "appendonly" at
// startup; an empty array means "no such setting" and it carries on.
void cmdConfig(Ctx& ctx, const Args& args) {
    if (toLower(args[1]) != "get") {
        resp::appendError(ctx.out, "ERR unknown subcommand '" + args[1].substr(0, 128) + "'. Try CONFIG HELP.");
        return;
    }
    if (args.size() < 3) {
        appendArityError(ctx.out, "config|get");
        return;
    }
    resp::appendArrayHeader(ctx.out, 0);
}

// ------------------------------------------------------------------ keys

// DEL key [key ...] -> number of keys removed
void cmdDel(Ctx& ctx, const Args& args) {
    long long removed = 0;
    for (std::size_t i = 1; i < args.size(); ++i) removed += ctx.db.erase(args[i]) ? 1 : 0;
    if (removed > 0) ctx.propagate(args);
    resp::appendInteger(ctx.out, removed);
}

// EXISTS key [key ...] -> how many of the given keys exist (a key named twice counts twice)
void cmdExists(Ctx& ctx, const Args& args) {
    long long found = 0;
    for (std::size_t i = 1; i < args.size(); ++i) found += ctx.db.find(args[i]) != nullptr ? 1 : 0;
    resp::appendInteger(ctx.out, found);
}

// TYPE key -> string | list | hash | zset | none
void cmdType(Ctx& ctx, const Args& args) {
    const Value* value = ctx.db.find(args[1]);
    resp::appendSimpleString(ctx.out, value != nullptr ? typeName(*value) : "none");
}

// DBSIZE -> number of keys
void cmdDbsize(Ctx& ctx, const Args&) { resp::appendInteger(ctx.out, static_cast<long long>(ctx.db.size())); }

// FLUSHALL [ASYNC|SYNC]. The mode is accepted for compatibility; we always flush
// synchronously, since there is only one thread.
void cmdFlushall(Ctx& ctx, const Args& args) {
    if (args.size() > 2) {
        resp::appendError(ctx.out, kSyntaxError);
        return;
    }
    if (args.size() == 2) {
        const std::string mode = toLower(args[1]);
        if (mode != "async" && mode != "sync") {
            resp::appendError(ctx.out, kSyntaxError);
            return;
        }
    }
    ctx.db.clear();
    ctx.propagate({"FLUSHALL"});
    appendOk(ctx.out);
}

// Shared by EXPIRE, PEXPIRE and PEXPIREAT: deadline = base + amount * unitMs.
// A deadline that is not in the future deletes the key, as in Redis.
void expireGeneric(Ctx& ctx, const Args& args, long long unitMs, std::int64_t base, std::string_view name) {
    long long amount = 0;
    if (!parseInt64(args[2], amount)) {
        resp::appendError(ctx.out, kNotAnInteger);
        return;
    }
    std::int64_t deadline = 0;
    if (!deadlineFrom(amount, unitMs, base, deadline)) {
        appendInvalidExpireError(ctx.out, name);
        return;
    }
    const ExpiryResult result = ctx.db.setExpiry(args[1], deadline);
    propagateExpiry(ctx, args[1], result, deadline);
    resp::appendInteger(ctx.out, result == ExpiryResult::NoSuchKey ? 0 : 1);
}

// EXPIRE key seconds -> 1 if the TTL was set, 0 if the key does not exist
void cmdExpire(Ctx& ctx, const Args& args) { expireGeneric(ctx, args, 1000, ctx.db.now(), "expire"); }

// PEXPIRE key milliseconds
void cmdPexpire(Ctx& ctx, const Args& args) { expireGeneric(ctx, args, 1, ctx.db.now(), "pexpire"); }

// PEXPIREAT key unix-time-milliseconds. This is the form TTLs take in the AOF.
void cmdPexpireat(Ctx& ctx, const Args& args) { expireGeneric(ctx, args, 1, 0, "pexpireat"); }

// Shared by TTL and PTTL: -2 if the key does not exist, -1 if it has no TTL.
void ttlGeneric(Ctx& ctx, const std::string& key, bool inMilliseconds) {
    if (ctx.db.find(key) == nullptr) {
        resp::appendInteger(ctx.out, -2);
        return;
    }
    const auto deadline = ctx.db.expiryOf(key);
    if (!deadline) {
        resp::appendInteger(ctx.out, -1);
        return;
    }
    const long long remainingMs = std::max<long long>(0, *deadline - ctx.db.now());
    // TTL rounds to the nearest second, like Redis: 1.6 s left reads as 2.
    resp::appendInteger(ctx.out, inMilliseconds ? remainingMs : (remainingMs + 500) / 1000);
}

// TTL key -> seconds left
void cmdTtl(Ctx& ctx, const Args& args) { ttlGeneric(ctx, args[1], false); }

// PTTL key -> milliseconds left
void cmdPttl(Ctx& ctx, const Args& args) { ttlGeneric(ctx, args[1], true); }

// PERSIST key -> 1 if a TTL was removed, 0 if the key has none or does not exist
void cmdPersist(Ctx& ctx, const Args& args) {
    const bool removed = ctx.db.persist(args[1]);
    if (removed) ctx.propagate(args);
    resp::appendInteger(ctx.out, removed ? 1 : 0);
}

// ------------------------------------------------------------------ strings

// GET key -> the value, or nil
void cmdGet(Ctx& ctx, const Args& args) {
    const std::string* value = ctx.db.findString(args[1]);
    if (value == nullptr) {
        resp::appendNull(ctx.out);
    } else {
        resp::appendBulkString(ctx.out, *value);
    }
}

// SET key value [NX|XX] [EX seconds|PX ms|EXAT unix-seconds|PXAT unix-ms]
// -> OK, or nil if the NX/XX condition was not met. SET replaces a value of any
// type (so it never fails with WRONGTYPE) and, without an expiry option, removes
// any existing TTL.
void cmdSet(Ctx& ctx, const Args& args) {
    enum class Ttl { None, Ex, Px, ExAt, PxAt };
    bool nx = false;  // only set if the key does not exist
    bool xx = false;  // only set if the key already exists
    Ttl ttl = Ttl::None;
    const std::string* ttlArg = nullptr;

    // Same acceptance rules as Redis: NX and XX exclude each other, the four
    // expiry options exclude each other and need a value; anything else is a
    // syntax error.
    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string option = toLower(args[i]);
        const bool hasValue = i + 1 < args.size();
        Ttl kind = Ttl::None;
        if (option == "ex") kind = Ttl::Ex;
        if (option == "px") kind = Ttl::Px;
        if (option == "exat") kind = Ttl::ExAt;
        if (option == "pxat") kind = Ttl::PxAt;

        if (option == "nx" && !xx) {
            nx = true;
        } else if (option == "xx" && !nx) {
            xx = true;
        } else if (kind != Ttl::None && (ttl == Ttl::None || ttl == kind) && hasValue) {
            ttl = kind;
            ttlArg = &args[++i];
        } else {
            resp::appendError(ctx.out, kSyntaxError);
            return;
        }
    }

    std::int64_t deadline = 0;
    if (ttlArg != nullptr) {
        long long amount = 0;
        if (!parseInt64(*ttlArg, amount)) {
            resp::appendError(ctx.out, kNotAnInteger);
            return;
        }
        const bool relative = ttl == Ttl::Ex || ttl == Ttl::Px;
        const long long unitMs = (ttl == Ttl::Ex || ttl == Ttl::ExAt) ? 1000 : 1;
        if (amount <= 0 || !deadlineFrom(amount, unitMs, relative ? ctx.db.now() : 0, deadline)) {
            appendInvalidExpireError(ctx.out, "set");
            return;
        }
    }

    const bool exists = ctx.db.find(args[1]) != nullptr;
    if ((nx && exists) || (xx && !exists)) {
        resp::appendNull(ctx.out);
        return;
    }
    ctx.db.set(args[1], args[2]);

    if (ttlArg == nullptr) {
        ctx.propagate({"SET", args[1], args[2]});
    } else if (ctx.db.setExpiry(args[1], deadline) == ExpiryResult::DeletedNow) {
        ctx.propagate({"DEL", args[1]});  // an EXAT/PXAT already in the past
    } else {
        // One self-contained entry, so a crash cannot leave the value without its TTL.
        ctx.propagate({"SET", args[1], args[2], "PXAT", std::to_string(deadline)});
    }
    appendOk(ctx.out);
}

// MGET key [key ...] -> one value or nil per key. A key of another type is nil,
// not an error, as in Redis.
void cmdMget(Ctx& ctx, const Args& args) {
    resp::appendArrayHeader(ctx.out, args.size() - 1);
    for (std::size_t i = 1; i < args.size(); ++i) {
        const Value* value = ctx.db.find(args[i]);
        const std::string* str = value != nullptr ? std::get_if<std::string>(value) : nullptr;
        if (str == nullptr) {
            resp::appendNull(ctx.out);
        } else {
            resp::appendBulkString(ctx.out, *str);
        }
    }
}

// MSET key value [key value ...] -> OK
void cmdMset(Ctx& ctx, const Args& args) {
    if (args.size() % 2 == 0) {  // the name plus pairs is always odd
        appendArityError(ctx.out, "mset");
        return;
    }
    for (std::size_t i = 1; i < args.size(); i += 2) ctx.db.set(args[i], args[i + 1]);
    ctx.propagate(args);
    appendOk(ctx.out);
}

// Shared by INCR, DECR and INCRBY: a missing key counts as 0. `args` is the
// original command, which is deterministic and so is logged as is.
void incrementBy(Ctx& ctx, const Args& args, long long delta) {
    const std::string& key = args[1];
    std::string* current = ctx.db.findString(key);
    long long value = 0;
    if (current != nullptr && !parseInt64(*current, value)) {
        resp::appendError(ctx.out, kNotAnInteger);
        return;
    }
    // Check before adding: signed overflow is undefined behaviour in C++, so
    // "add, then see if it wrapped" is not a valid test.
    if ((delta > 0 && value > LLONG_MAX - delta) || (delta < 0 && value < LLONG_MIN - delta)) {
        resp::appendError(ctx.out, "ERR increment or decrement would overflow");
        return;
    }
    value += delta;
    if (current != nullptr) {
        *current = std::to_string(value);
    } else {
        ctx.db.set(key, std::to_string(value));
    }
    ctx.propagate(args);
    resp::appendInteger(ctx.out, value);
}

// INCR key -> the new value
void cmdIncr(Ctx& ctx, const Args& args) { incrementBy(ctx, args, 1); }

// DECR key -> the new value
void cmdDecr(Ctx& ctx, const Args& args) { incrementBy(ctx, args, -1); }

// INCRBY key increment -> the new value
void cmdIncrby(Ctx& ctx, const Args& args) {
    long long delta = 0;
    if (!parseInt64(args[2], delta)) {
        resp::appendError(ctx.out, kNotAnInteger);
        return;
    }
    incrementBy(ctx, args, delta);
}

// APPEND key value -> the new length
void cmdAppend(Ctx& ctx, const Args& args) {
    std::string* current = ctx.db.findString(args[1]);
    std::size_t length = 0;
    if (current != nullptr) {
        current->append(args[2]);
        length = current->size();
    } else {
        ctx.db.set(args[1], args[2]);
        length = args[2].size();
    }
    ctx.propagate(args);
    resp::appendInteger(ctx.out, static_cast<long long>(length));
}

// ------------------------------------------------------------------ lists

// Turns Redis-style inclusive indices into a valid [from, to] range of a
// container of `size` elements. Negative indices count from the end (-1 is the
// last element); out-of-range ends are clamped. Returns false if nothing is
// left. Same arithmetic as Redis, so LRANGE agrees on every edge case.
bool clampRange(long long start, long long stop, long long size, long long& from, long long& to) {
    if (start < 0) start += size;
    if (stop < 0) stop += size;
    if (start < 0) start = 0;
    if (start > stop || start >= size) return false;
    if (stop >= size) stop = size - 1;
    from = start;
    to = stop;
    return true;
}

// Shared by LPUSH and RPUSH: creates the list if needed, returns the new length.
void pushGeneric(Ctx& ctx, const Args& args, bool atFront) {
    List* list = ctx.db.findList(args[1]);  // throws on a non-list before anything changes
    if (list == nullptr) list = &std::get<List>(ctx.db.set(args[1], List{}));
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (atFront) {
            list->push_front(args[i]);
        } else {
            list->push_back(args[i]);
        }
    }
    ctx.propagate(args);
    resp::appendInteger(ctx.out, static_cast<long long>(list->size()));
}

// LPUSH key element [element ...] -> new length. Each element goes to the
// front in turn, so LPUSH k a b c leaves c b a.
void cmdLpush(Ctx& ctx, const Args& args) { pushGeneric(ctx, args, true); }

// RPUSH key element [element ...] -> new length
void cmdRpush(Ctx& ctx, const Args& args) { pushGeneric(ctx, args, false); }

// Shared by LPOP and RPOP. Without a count: one element, or nil. With a count
// (Redis 6.2+): an array of up to `count` elements, or a nil array if the key
// does not exist. Removing the last element deletes the key, as in Redis.
void popGeneric(Ctx& ctx, const Args& args, bool fromFront, std::string_view name) {
    if (args.size() > 3) {
        appendArityError(ctx.out, name);
        return;
    }
    const bool hasCount = args.size() == 3;
    long long count = 0;
    if (hasCount && (!parseInt64(args[2], count) || count < 0)) {
        resp::appendError(ctx.out, "ERR value is out of range, must be positive");
        return;
    }

    List* list = ctx.db.findList(args[1]);
    if (list == nullptr) {
        if (hasCount) {
            resp::appendNullArray(ctx.out);
        } else {
            resp::appendNull(ctx.out);
        }
        return;
    }
    if (hasCount && count == 0) {
        resp::appendArrayHeader(ctx.out, 0);
        return;
    }

    const std::size_t popCount = hasCount ? std::min<std::size_t>(static_cast<std::size_t>(count), list->size()) : 1;
    if (hasCount) resp::appendArrayHeader(ctx.out, popCount);
    for (std::size_t i = 0; i < popCount; ++i) {
        if (fromFront) {
            resp::appendBulkString(ctx.out, list->front());
            list->pop_front();
        } else {
            resp::appendBulkString(ctx.out, list->back());
            list->pop_back();
        }
    }
    if (list->empty()) ctx.db.erase(args[1]);  // `list` dangles after this
    ctx.propagate(args);
}

// LPOP key [count]
void cmdLpop(Ctx& ctx, const Args& args) { popGeneric(ctx, args, true, "lpop"); }

// RPOP key [count]
void cmdRpop(Ctx& ctx, const Args& args) { popGeneric(ctx, args, false, "rpop"); }

// LRANGE key start stop -> the elements from start to stop, inclusive
void cmdLrange(Ctx& ctx, const Args& args) {
    long long start = 0;
    long long stop = 0;
    if (!parseInt64(args[2], start) || !parseInt64(args[3], stop)) {
        resp::appendError(ctx.out, kNotAnInteger);
        return;
    }
    const List* list = ctx.db.findList(args[1]);
    long long from = 0;
    long long to = 0;
    if (list == nullptr || !clampRange(start, stop, static_cast<long long>(list->size()), from, to)) {
        resp::appendArrayHeader(ctx.out, 0);
        return;
    }
    resp::appendArrayHeader(ctx.out, static_cast<std::size_t>(to - from + 1));
    // std::deque has random access, so jumping to `from` is O(1).
    for (long long i = from; i <= to; ++i) resp::appendBulkString(ctx.out, (*list)[static_cast<std::size_t>(i)]);
}

// LLEN key -> length (0 if the key does not exist)
void cmdLlen(Ctx& ctx, const Args& args) {
    const List* list = ctx.db.findList(args[1]);
    resp::appendInteger(ctx.out, list != nullptr ? static_cast<long long>(list->size()) : 0);
}

// ------------------------------------------------------------------ hashes

// HSET key field value [field value ...] -> number of fields that were new
void cmdHset(Ctx& ctx, const Args& args) {
    if (args.size() % 2 == 1) {  // name + key + pairs is always even
        appendArityError(ctx.out, "hset");
        return;
    }
    Hash* hash = ctx.db.findHash(args[1]);
    if (hash == nullptr) hash = &std::get<Hash>(ctx.db.set(args[1], Hash{}));
    long long added = 0;
    for (std::size_t i = 2; i < args.size(); i += 2) {
        const bool inserted = hash->insert_or_assign(args[i], args[i + 1]).second;
        added += inserted ? 1 : 0;
    }
    ctx.propagate(args);
    resp::appendInteger(ctx.out, added);
}

// HGET key field -> the value, or nil
void cmdHget(Ctx& ctx, const Args& args) {
    const Hash* hash = ctx.db.findHash(args[1]);
    if (hash != nullptr) {
        const auto it = hash->find(args[2]);
        if (it != hash->end()) {
            resp::appendBulkString(ctx.out, it->second);
            return;
        }
    }
    resp::appendNull(ctx.out);
}

// HDEL key field [field ...] -> number of fields removed. Removing the last
// field deletes the key, as in Redis.
void cmdHdel(Ctx& ctx, const Args& args) {
    Hash* hash = ctx.db.findHash(args[1]);
    long long removed = 0;
    if (hash != nullptr) {
        for (std::size_t i = 2; i < args.size(); ++i) removed += static_cast<long long>(hash->erase(args[i]));
        if (hash->empty()) ctx.db.erase(args[1]);  // `hash` dangles after this
    }
    if (removed > 0) ctx.propagate(args);
    resp::appendInteger(ctx.out, removed);
}

// HGETALL key -> field1, value1, field2, value2, ... The order is unspecified,
// as in Redis.
void cmdHgetall(Ctx& ctx, const Args& args) {
    const Hash* hash = ctx.db.findHash(args[1]);
    if (hash == nullptr) {
        resp::appendArrayHeader(ctx.out, 0);
        return;
    }
    resp::appendArrayHeader(ctx.out, 2 * hash->size());
    for (const auto& [field, value] : *hash) {
        resp::appendBulkString(ctx.out, field);
        resp::appendBulkString(ctx.out, value);
    }
}

// HLEN key -> number of fields (0 if the key does not exist)
void cmdHlen(Ctx& ctx, const Args& args) {
    const Hash* hash = ctx.db.findHash(args[1]);
    resp::appendInteger(ctx.out, hash != nullptr ? static_cast<long long>(hash->size()) : 0);
}

// HEXISTS key field -> 1 or 0
void cmdHexists(Ctx& ctx, const Args& args) {
    const Hash* hash = ctx.db.findHash(args[1]);
    resp::appendInteger(ctx.out, hash != nullptr && hash->count(args[2]) > 0 ? 1 : 0);
}

// ------------------------------------------------------------------ sorted sets

// ZADD key score member [score member ...] -> number of members added (score
// updates of existing members do not count, as in Redis). Every score is
// checked before anything changes, so a bad score leaves the set untouched.
void cmdZadd(Ctx& ctx, const Args& args) {
    if ((args.size() - 2) % 2 != 0) {
        resp::appendError(ctx.out, kSyntaxError);
        return;
    }
    std::vector<double> scores;
    scores.reserve((args.size() - 2) / 2);
    for (std::size_t i = 2; i < args.size(); i += 2) {
        double score = 0;
        if (!parseScore(args[i], score)) {
            resp::appendError(ctx.out, "ERR value is not a valid float");
            return;
        }
        scores.push_back(score);
    }

    ZSet* zset = ctx.db.findZSet(args[1]);
    if (zset == nullptr) zset = &std::get<ZSet>(ctx.db.set(args[1], ZSet{}));
    long long added = 0;
    bool changed = false;
    for (std::size_t i = 0; i < scores.size(); ++i) {
        const ZSet::AddResult result = zset->add(args[3 + 2 * i], scores[i]);
        added += result == ZSet::AddResult::Added ? 1 : 0;
        changed = changed || result != ZSet::AddResult::Unchanged;
    }
    if (changed) ctx.propagate(args);
    resp::appendInteger(ctx.out, added);
}

// ZREM key member [member ...] -> number removed. Removing the last member
// deletes the key.
void cmdZrem(Ctx& ctx, const Args& args) {
    ZSet* zset = ctx.db.findZSet(args[1]);
    long long removed = 0;
    if (zset != nullptr) {
        for (std::size_t i = 2; i < args.size(); ++i) removed += zset->remove(args[i]) ? 1 : 0;
        if (zset->size() == 0) ctx.db.erase(args[1]);  // `zset` dangles after this
    }
    if (removed > 0) ctx.propagate(args);
    resp::appendInteger(ctx.out, removed);
}

// ZSCORE key member -> the score, or nil. O(1) through the hash map.
void cmdZscore(Ctx& ctx, const Args& args) {
    const ZSet* zset = ctx.db.findZSet(args[1]);
    const auto score = zset != nullptr ? zset->score(args[2]) : std::nullopt;
    if (score) {
        resp::appendDouble(ctx.out, *score);
    } else {
        resp::appendNull(ctx.out);
    }
}

// ZRANK key member [WITHSCORE] -> 0-based position by (score, member), or nil.
// O(log n) through the skip list's spans.
void cmdZrank(Ctx& ctx, const Args& args) {
    if (args.size() > 4) {
        appendArityError(ctx.out, "zrank");
        return;
    }
    const bool withScore = args.size() == 4;
    if (withScore && toLower(args[3]) != "withscore") {
        resp::appendError(ctx.out, kSyntaxError);
        return;
    }
    const ZSet* zset = ctx.db.findZSet(args[1]);
    const auto rank = zset != nullptr ? zset->rank(args[2]) : std::nullopt;
    if (!rank) {
        if (withScore) {
            resp::appendNullArray(ctx.out);
        } else {
            resp::appendNull(ctx.out);
        }
        return;
    }
    if (withScore) resp::appendArrayHeader(ctx.out, 2);
    resp::appendInteger(ctx.out, static_cast<long long>(*rank));
    if (withScore) resp::appendDouble(ctx.out, *zset->score(args[2]));
}

// ZRANGE key start stop [WITHSCORES] -> members from rank start to stop,
// inclusive, lowest score first. Same index rules as LRANGE. O(log n + m): one
// descent to find `start`, then a walk along level 0.
void cmdZrange(Ctx& ctx, const Args& args) {
    bool withScores = false;
    for (std::size_t i = 4; i < args.size(); ++i) {
        if (toLower(args[i]) != "withscores") {  // BYSCORE, BYLEX, REV and LIMIT are not supported
            resp::appendError(ctx.out, kSyntaxError);
            return;
        }
        withScores = true;
    }
    long long start = 0;
    long long stop = 0;
    if (!parseInt64(args[2], start) || !parseInt64(args[3], stop)) {
        resp::appendError(ctx.out, kNotAnInteger);
        return;
    }
    const ZSet* zset = ctx.db.findZSet(args[1]);
    long long from = 0;
    long long to = 0;
    if (zset == nullptr || !clampRange(start, stop, static_cast<long long>(zset->size()), from, to)) {
        resp::appendArrayHeader(ctx.out, 0);
        return;
    }
    const auto count = static_cast<std::size_t>(to - from + 1);
    resp::appendArrayHeader(ctx.out, withScores ? 2 * count : count);
    const SkipList::Node* node = zset->ordered().at(static_cast<std::size_t>(from));
    for (std::size_t i = 0; i < count; ++i, node = node->next.get()) {
        resp::appendBulkString(ctx.out, node->member);
        if (withScores) resp::appendDouble(ctx.out, node->score);
    }
}

// ZCARD key -> number of members (0 if the key does not exist)
void cmdZcard(Ctx& ctx, const Args& args) {
    const ZSet* zset = ctx.db.findZSet(args[1]);
    resp::appendInteger(ctx.out, zset != nullptr ? static_cast<long long>(zset->size()) : 0);
}

// ------------------------------------------------------------------ table

const std::unordered_map<std::string, Command>& commandTable() {
    static const std::unordered_map<std::string, Command> table = {
        // connection
        {"ping", {cmdPing, -1}},
        {"echo", {cmdEcho, 2}},
        {"command", {cmdCommand, -1}},
        {"config", {cmdConfig, -2}},
        // keys
        {"del", {cmdDel, -2}},
        {"exists", {cmdExists, -2}},
        {"type", {cmdType, 2}},
        {"dbsize", {cmdDbsize, 1}},
        {"flushall", {cmdFlushall, -1}},
        {"expire", {cmdExpire, 3}},
        {"pexpire", {cmdPexpire, 3}},
        {"pexpireat", {cmdPexpireat, 3}},
        {"ttl", {cmdTtl, 2}},
        {"pttl", {cmdPttl, 2}},
        {"persist", {cmdPersist, 2}},
        // strings
        {"get", {cmdGet, 2}},
        {"set", {cmdSet, -3}},
        {"mget", {cmdMget, -2}},
        {"mset", {cmdMset, -3}},
        {"incr", {cmdIncr, 2}},
        {"decr", {cmdDecr, 2}},
        {"incrby", {cmdIncrby, 3}},
        {"append", {cmdAppend, 3}},
        // lists
        {"lpush", {cmdLpush, -3}},
        {"rpush", {cmdRpush, -3}},
        {"lpop", {cmdLpop, -2}},
        {"rpop", {cmdRpop, -2}},
        {"lrange", {cmdLrange, 4}},
        {"llen", {cmdLlen, 2}},
        // hashes
        {"hset", {cmdHset, -4}},
        {"hget", {cmdHget, 3}},
        {"hdel", {cmdHdel, -3}},
        {"hgetall", {cmdHgetall, 2}},
        {"hlen", {cmdHlen, 2}},
        {"hexists", {cmdHexists, 3}},
        // sorted sets
        {"zadd", {cmdZadd, -4}},
        {"zrem", {cmdZrem, -3}},
        {"zscore", {cmdZscore, 3}},
        {"zrank", {cmdZrank, -3}},
        {"zrange", {cmdZrange, -4}},
        {"zcard", {cmdZcard, 2}},
    };
    return table;
}

}  // namespace

void executeCommand(Keyspace& db, const Args& args, std::string& out, const Propagate& propagate) {
    const std::string name = toLower(args[0]);
    const auto& table = commandTable();
    const auto it = table.find(name);
    if (it == table.end()) {
        appendUnknownCommandError(out, args);
        return;
    }

    const Command& cmd = it->second;
    const auto argc = static_cast<long long>(args.size());
    const bool arityOk = cmd.arity >= 0 ? argc == cmd.arity : argc >= -cmd.arity;
    if (!arityOk) {
        appendArityError(out, name);
        return;
    }

    // Type checks live in the keyspace and the error reply lives here, so no
    // handler can forget either. Handlers check types before changing anything
    // (and so before propagating); if a reply was half-written when the error
    // surfaced, it is discarded.
    const std::size_t replyStart = out.size();
    Ctx ctx{db, out, propagate};
    try {
        cmd.handler(ctx, args);
    } catch (const WrongTypeError& e) {
        out.resize(replyStart);
        resp::appendError(out, e.what());
    }
}
