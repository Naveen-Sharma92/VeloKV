// Deadline map and min-heap with lazy deletion. Changing or removing a TTL is
// O(1) on the map and leaves a stale heap entry behind; stale entries are
// skipped when popped, and the heap is rebuilt if they pile up.
#include "expiry.h"

#include <algorithm>
#include <chrono>

namespace {

// Rebuild once the heap holds this many more entries than twice the number of
// live deadlines. The constant keeps tiny heaps from being rebuilt constantly.
constexpr std::size_t kCompactionSlack = 1024;

}  // namespace

std::int64_t unixTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void Expiry::set(const std::string& key, std::int64_t deadlineMs) {
    deadlines_.insert_or_assign(key, deadlineMs);
    heap_.push_back({deadlineMs, key});
    std::push_heap(heap_.begin(), heap_.end(), Later{});

    // A key whose TTL is refreshed over and over (a session timeout, say) adds an
    // entry each time. Rebuilding from the live deadlines once stale entries
    // outnumber live ones keeps memory bounded; the O(n) rebuild happens at most
    // once per n pushes, so it costs O(1) amortized per push.
    if (heap_.size() > 2 * deadlines_.size() + kCompactionSlack) rebuildHeap();
}

bool Expiry::remove(const std::string& key) { return deadlines_.erase(key) > 0; }

std::optional<std::int64_t> Expiry::deadline(const std::string& key) const {
    // Most keys have no TTL; with no TTLs at all, skip hashing the key.
    if (deadlines_.empty()) return std::nullopt;
    const auto it = deadlines_.find(key);
    if (it == deadlines_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> Expiry::takeDue(std::int64_t nowMs, std::size_t maxKeys) {
    std::vector<std::string> due;
    while (due.size() < maxKeys && !heap_.empty() && heap_.front().deadlineMs < nowMs) {
        std::pop_heap(heap_.begin(), heap_.end(), Later{});
        HeapEntry entry = std::move(heap_.back());
        heap_.pop_back();

        // Stale if the key has since lost its TTL (deleted, overwritten, PERSIST)
        // or been given a different one.
        const auto it = deadlines_.find(entry.key);
        if (it == deadlines_.end() || it->second != entry.deadlineMs) continue;

        deadlines_.erase(it);
        due.push_back(std::move(entry.key));
    }
    return due;
}

void Expiry::clear() {
    deadlines_.clear();
    heap_.clear();
}

void Expiry::rebuildHeap() {
    heap_.clear();
    heap_.reserve(deadlines_.size());
    for (const auto& [key, deadlineMs] : deadlines_) heap_.push_back({deadlineMs, key});
    std::make_heap(heap_.begin(), heap_.end(), Later{});
}
