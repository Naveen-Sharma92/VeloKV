// The database: every key and its typed value, in one hash table, plus the TTLs
// of keys that have one. Typed lookups are the single place where "this key
// holds the wrong kind of value" is detected (WRONGTYPE), and every lookup
// checks expiry, so an expired key is never visible to a command.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

#include "expiry.h"
#include "skiplist.h"

using List = std::deque<std::string>;
using Hash = std::unordered_map<std::string, std::string>;

// One value per key. The variant records which type it is, so a type check is a
// tag comparison.
using Value = std::variant<std::string, List, Hash, ZSet>;

// "string", "list", "hash", "zset": the names TYPE replies with.
const char* typeName(const Value& value);

// Thrown by a typed lookup when the key exists but holds another type. The
// command dispatcher turns it into the WRONGTYPE error reply.
class WrongTypeError : public std::runtime_error {
public:
    WrongTypeError() : std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value") {}
};

// What setExpiry() did.
enum class ExpiryResult {
    NoSuchKey,  // nothing to expire
    Scheduled,  // the key now has this deadline
    DeletedNow  // the deadline had already passed, so the key was deleted
};

class Keyspace {
public:
    // Returns the current time as Unix milliseconds. Tests pass a fake clock to
    // control time without sleeping.
    using Clock = std::function<std::int64_t()>;

    // Called with the name of each key that expires (lazily or in the sweep), so
    // the AOF can record the deletion as a DEL.
    using ExpiredHook = std::function<void(const std::string& key)>;

    explicit Keyspace(Clock clock = unixTimeMs);

    std::int64_t now() const { return clock_(); }

    // The value at `key`, or nullptr if the key does not exist. An expired key is
    // deleted here, on first access, and reported as missing (lazy expiry).
    Value* find(const std::string& key);

    // Typed lookups: the value at `key` if it has that type, nullptr if the key
    // does not exist, and WrongTypeError if it holds another type.
    std::string* findString(const std::string& key);
    List* findList(const std::string& key);
    Hash* findHash(const std::string& key);
    ZSet* findZSet(const std::string& key);

    // Stores `value` at `key`, replacing whatever was there, of any type, and
    // removing any TTL (as SET without EX/PX does in Redis). Returns the stored
    // value, so a command can create a container and fill it in place.
    Value& set(const std::string& key, Value value);

    // Returns true if a live key was removed.
    bool erase(const std::string& key);

    // Gives an existing key a deadline in Unix ms. A deadline that has already
    // passed deletes the key immediately, as Redis does (except while loading).
    ExpiryResult setExpiry(const std::string& key, std::int64_t deadlineMs);

    // The deadline of a key, or nullopt if it has none (or does not exist).
    std::optional<std::int64_t> expiryOf(const std::string& key);

    // Removes the TTL of `key`. Returns true if it had one.
    bool persist(const std::string& key);

    // Active expiry: deletes up to `maxKeys` keys whose deadline has passed,
    // earliest first. Returns how many were deleted.
    std::size_t expireDue(std::size_t maxKeys);

    // Counts keys that have expired but not been deleted yet, as Redis's DBSIZE does.
    std::size_t size() const { return map_.size(); }
    void clear();

    // While loading the AOF nothing expires: the log already contains a DEL for
    // every key that expired in the original run, at the right point, and
    // expiring by the current time instead would change the outcome of the
    // commands that follow. Keys whose deadline passed meanwhile are expired
    // normally once loading ends.
    void setLoading(bool loading) { loading_ = loading; }

    void setExpiredHook(ExpiredHook hook) { onExpired_ = std::move(hook); }

private:
    // Deletes `key` if its deadline has passed. Returns true if it did.
    bool expireIfNeeded(const std::string& key);

    // Shared body of the typed lookups.
    template <typename T>
    T* findAs(const std::string& key);

    Clock clock_;
    std::unordered_map<std::string, Value> map_;
    Expiry expiry_;
    bool loading_ = false;
    ExpiredHook onExpired_;
};
