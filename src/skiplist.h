// Sorted-set storage. SkipList keeps (score, member) pairs in order, with a
// span on every forward pointer so that rank queries are O(log n). ZSet pairs
// it with a hash map from member to score for O(1) score lookups.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class SkipList {
public:
    // Same parameters as Redis: each node is promoted one level up with
    // probability 1/4, up to 32 levels. 32 levels at p = 1/4 cover 4^32 = 2^64
    // elements before the top level gets crowded.
    static constexpr int kMaxLevel = 32;

    struct Node {
        struct Link {
            Node* forward = nullptr;  // next node at this level (non-owning)
            std::size_t span = 0;     // how many level-0 steps this link jumps over
        };

        Node(double score, std::string member, int levels);

        double score;
        std::string member;
        // Level 0 contains every node exactly once, so ownership follows it: each
        // node owns its level-0 successor. links[0].forward is always next.get();
        // the higher levels are non-owning "express lanes".
        std::unique_ptr<Node> next;
        std::vector<Link> links;
    };

    SkipList();
    ~SkipList();

    SkipList(SkipList&& other) noexcept;
    SkipList& operator=(SkipList&& other) noexcept;  // a moved-from list may only be destroyed or assigned to
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Inserts (score, member). The pair must not already be present. O(log n) expected.
    void insert(double score, const std::string& member);

    // Removes (score, member). Returns false if it was not present.
    bool erase(double score, const std::string& member);

    // 0-based position of (score, member) in sorted order, or nullopt. O(log n).
    std::optional<std::size_t> rank(double score, const std::string& member) const;

    // The node at 0-based position `rank`, or nullptr if out of range. O(log n).
    // Follow Node::next to walk forward from there.
    const Node* at(std::size_t rank) const;

    std::size_t size() const { return size_; }

    // Checks every structural invariant (order, spans, ownership, size) and
    // returns a description of the first violation, or "" if there is none.
    // O(n log n); used by the tests.
    std::string checkInvariants() const;

private:
    void destroyNodes() noexcept;

    std::unique_ptr<Node> header_;  // sentinel before the first element, with kMaxLevel links
    int level_ = 1;                 // levels currently in use
    std::size_t size_ = 0;
};

// A Redis sorted set: unique members, each with a score, ordered by
// (score, member). The skip list answers order and rank questions; the hash map
// answers "what is this member's score?" in O(1). Every operation keeps the two
// in step.
class ZSet {
public:
    enum class AddResult { Added, Updated, Unchanged };

    // Adds `member`, or moves it if its score changed.
    AddResult add(const std::string& member, double score);

    // Returns true if `member` was present.
    bool remove(const std::string& member);

    std::optional<double> score(const std::string& member) const;
    std::optional<std::size_t> rank(const std::string& member) const;

    std::size_t size() const { return scores_.size(); }
    const SkipList& ordered() const { return list_; }

private:
    SkipList list_;
    std::unordered_map<std::string, double> scores_;
};
