// TTL bookkeeping: the deadline of every key that has one (absolute Unix time in
// milliseconds), plus a min-heap of (deadline, key) that lets the active sweep
// find the keys that are due next without scanning all of them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Wall-clock time as Unix milliseconds. Deadlines use the wall clock, not a
// monotonic one, because they are written to the AOF and must still mean the
// same moment after a restart.
std::int64_t unixTimeMs();

class Expiry {
public:
    // Sets or replaces the deadline of `key`.
    void set(const std::string& key, std::int64_t deadlineMs);

    // Forgets the deadline of `key`. Returns true if it had one.
    bool remove(const std::string& key);

    std::optional<std::int64_t> deadline(const std::string& key) const;

    // Pops due heap entries in deadline order and returns up to `maxKeys` keys
    // that have expired (deadline < nowMs), forgetting their deadlines. The
    // caller then deletes their values.
    std::vector<std::string> takeDue(std::int64_t nowMs, std::size_t maxKeys);

    void clear();

    std::size_t size() const { return deadlines_.size(); }  // keys with a deadline
    std::size_t heapSize() const { return heap_.size(); }   // includes stale entries

private:
    struct HeapEntry {
        std::int64_t deadlineMs;
        std::string key;
    };

    // The std::*_heap algorithms build a max-heap; ordering by "comes later"
    // puts the earliest deadline on top.
    struct Later {
        bool operator()(const HeapEntry& a, const HeapEntry& b) const { return a.deadlineMs > b.deadlineMs; }
    };

    void rebuildHeap();

    // The source of truth: one entry per key that has a TTL.
    std::unordered_map<std::string, std::int64_t> deadlines_;
    // Min-heap ordered by deadline (std::push_heap / std::pop_heap). Entries are
    // never removed early: when a key is deleted or its TTL changes, its old
    // entry becomes stale and is skipped when it reaches the top.
    std::vector<HeapEntry> heap_;
};
