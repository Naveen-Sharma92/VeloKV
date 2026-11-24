// Keyspace storage, typed access and expiry. Commands never touch the maps
// directly, so the expiry check in find() cannot be bypassed.
#include "keyspace.h"

#include <utility>

const char* typeName(const Value& value) {
    if (std::holds_alternative<std::string>(value)) return "string";
    if (std::holds_alternative<List>(value)) return "list";
    if (std::holds_alternative<Hash>(value)) return "hash";
    return "zset";
}

Keyspace::Keyspace(Clock clock) : clock_(std::move(clock)) {}

bool Keyspace::expireIfNeeded(const std::string& key) {
    if (loading_) return false;
    // Only keys with a TTL need the clock, so the common case is one map lookup.
    // Like Redis, a key is expired once the time is strictly past its deadline.
    const auto deadline = expiry_.deadline(key);
    if (!deadline || *deadline >= clock_()) return false;
    map_.erase(key);
    expiry_.remove(key);
    if (onExpired_) onExpired_(key);
    return true;
}

Value* Keyspace::find(const std::string& key) {
    if (expireIfNeeded(key)) return nullptr;
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

template <typename T>
T* Keyspace::findAs(const std::string& key) {
    Value* value = find(key);
    if (value == nullptr) return nullptr;
    T* typed = std::get_if<T>(value);  // nullptr if the variant holds another type
    if (typed == nullptr) throw WrongTypeError();
    return typed;
}

std::string* Keyspace::findString(const std::string& key) { return findAs<std::string>(key); }
List* Keyspace::findList(const std::string& key) { return findAs<List>(key); }
Hash* Keyspace::findHash(const std::string& key) { return findAs<Hash>(key); }
ZSet* Keyspace::findZSet(const std::string& key) { return findAs<ZSet>(key); }

Value& Keyspace::set(const std::string& key, Value value) {
    expiry_.remove(key);
    return map_.insert_or_assign(key, std::move(value)).first->second;
}

bool Keyspace::erase(const std::string& key) {
    if (expireIfNeeded(key)) return false;  // it was already logically gone
    expiry_.remove(key);
    return map_.erase(key) > 0;
}

ExpiryResult Keyspace::setExpiry(const std::string& key, std::int64_t deadlineMs) {
    if (find(key) == nullptr) return ExpiryResult::NoSuchKey;
    if (deadlineMs <= clock_() && !loading_) {
        map_.erase(key);
        expiry_.remove(key);
        return ExpiryResult::DeletedNow;
    }
    expiry_.set(key, deadlineMs);
    return ExpiryResult::Scheduled;
}

std::optional<std::int64_t> Keyspace::expiryOf(const std::string& key) {
    if (find(key) == nullptr) return std::nullopt;
    return expiry_.deadline(key);
}

bool Keyspace::persist(const std::string& key) {
    if (find(key) == nullptr) return false;
    return expiry_.remove(key);
}

std::size_t Keyspace::expireDue(std::size_t maxKeys) {
    if (loading_) return 0;
    const auto due = expiry_.takeDue(clock_(), maxKeys);
    for (const auto& key : due) {
        map_.erase(key);
        if (onExpired_) onExpired_(key);
    }
    return due.size();
}

void Keyspace::clear() {
    map_.clear();
    expiry_.clear();
}
