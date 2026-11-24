// Skip list with spans (the same design as Redis's zskiplist) and the sorted
// set built on it. Spans let every search also count how many elements it
// passed, which is what makes rank queries O(log n) instead of O(n).
#include "skiplist.h"

#include <random>

namespace {

// Ordering of the set: by score, then by member bytes for equal scores.
// std::string comparison is memcmp-like, as Redis's sdscmp is.
bool before(double scoreA, const std::string& memberA, double scoreB, const std::string& memberB) {
    return scoreA < scoreB || (scoreA == scoreB && memberA < memberB);
}

bool before(const SkipList::Node* node, double score, const std::string& member) {
    return before(node->score, node->member, score, member);
}

// 1 + the number of successful coin flips with p = 1/4. The levels do not
// depend on the data, so no insertion order can unbalance the list.
int randomLevel() {
    static std::mt19937 rng{std::random_device{}()};  // one thread, so one shared generator is fine
    int level = 1;
    while (level < SkipList::kMaxLevel && rng() % 4 == 0) ++level;
    return level;
}

}  // namespace

SkipList::Node::Node(double score, std::string member, int levels)
    : score(score), member(std::move(member)), links(static_cast<std::size_t>(levels)) {}

SkipList::SkipList() : header_(std::make_unique<Node>(0.0, std::string(), kMaxLevel)) {}

SkipList::~SkipList() { destroyNodes(); }

SkipList::SkipList(SkipList&& other) noexcept
    : header_(std::move(other.header_)), level_(other.level_), size_(other.size_) {}

SkipList& SkipList::operator=(SkipList&& other) noexcept {
    if (this != &other) {
        destroyNodes();
        header_ = std::move(other.header_);
        level_ = other.level_;
        size_ = other.size_;
    }
    return *this;
}

void SkipList::destroyNodes() noexcept {
    // Letting header_ go would destroy node 1, whose destructor destroys node 2,
    // and so on: one stack frame per element, which overflows the stack for a
    // large set. Unlink the nodes one at a time instead.
    if (!header_) return;
    std::unique_ptr<Node> node = std::move(header_->next);
    while (node) node = std::move(node->next);
}

void SkipList::insert(double score, const std::string& member) {
    Node* update[kMaxLevel] = {};        // last node before the new one, per level
    std::size_t rankAt[kMaxLevel] = {};  // 1-based rank of update[i] (header = 0)

    // Walk down from the top level, moving right while the next node sorts
    // before the new one, and add up the spans we cross.
    Node* x = header_.get();
    for (int i = level_ - 1; i >= 0; --i) {
        rankAt[i] = (i == level_ - 1) ? 0 : rankAt[i + 1];
        while (x->links[i].forward != nullptr && before(x->links[i].forward, score, member)) {
            rankAt[i] += x->links[i].span;
            x = x->links[i].forward;
        }
        update[i] = x;
    }

    const int level = randomLevel();
    if (level > level_) {
        // New levels start at the header and, for now, jump over the whole list.
        for (int i = level_; i < level; ++i) {
            rankAt[i] = 0;
            update[i] = header_.get();
            header_->links[i].forward = nullptr;
            header_->links[i].span = size_;
        }
        level_ = level;
    }

    auto owned = std::make_unique<Node>(score, member, level);
    Node* node = owned.get();
    // rankAt[0] is the number of elements before the new node. At each level the
    // predecessor's old span is split in two around the new node.
    for (int i = 0; i < level; ++i) {
        node->links[i].forward = update[i]->links[i].forward;
        update[i]->links[i].forward = node;
        node->links[i].span = update[i]->links[i].span - (rankAt[0] - rankAt[i]);
        update[i]->links[i].span = (rankAt[0] - rankAt[i]) + 1;
    }
    // Links above the new node's height now jump over one more element.
    for (int i = level; i < level_; ++i) update[i]->links[i].span++;

    node->next = std::move(update[0]->next);
    update[0]->next = std::move(owned);
    ++size_;
}

bool SkipList::erase(double score, const std::string& member) {
    Node* update[kMaxLevel] = {};
    Node* x = header_.get();
    for (int i = level_ - 1; i >= 0; --i) {
        while (x->links[i].forward != nullptr && before(x->links[i].forward, score, member)) {
            x = x->links[i].forward;
        }
        update[i] = x;
    }

    Node* target = x->links[0].forward;
    if (target == nullptr || target->score != score || target->member != member) return false;

    for (int i = 0; i < level_; ++i) {
        if (update[i]->links[i].forward == target) {
            // Bypass the target: the predecessor now jumps over the target's span too.
            update[i]->links[i].span += target->links[i].span - 1;
            update[i]->links[i].forward = target->links[i].forward;
        } else {
            // A taller link passes over the target: one element fewer under it.
            update[i]->links[i].span -= 1;
        }
    }
    while (level_ > 1 && header_->links[level_ - 1].forward == nullptr) --level_;
    --size_;

    // Last, hand the target's successor to its predecessor; this frees the target.
    update[0]->next = std::move(target->next);
    return true;
}

std::optional<std::size_t> SkipList::rank(double score, const std::string& member) const {
    std::size_t traversed = 0;
    const Node* x = header_.get();
    for (int i = level_ - 1; i >= 0; --i) {
        // Move right while the next node is <= (score, member).
        while (x->links[i].forward != nullptr && !before(score, member, x->links[i].forward->score,
                                                         x->links[i].forward->member)) {
            traversed += x->links[i].span;
            x = x->links[i].forward;
        }
        if (x != header_.get() && x->score == score && x->member == member) return traversed - 1;
    }
    return std::nullopt;
}

const SkipList::Node* SkipList::at(std::size_t rank) const {
    const std::size_t target = rank + 1;  // spans count 1-based positions
    std::size_t traversed = 0;
    const Node* x = header_.get();
    for (int i = level_ - 1; i >= 0; --i) {
        while (x->links[i].forward != nullptr && traversed + x->links[i].span <= target) {
            traversed += x->links[i].span;
            x = x->links[i].forward;
        }
        if (traversed == target) return x;
    }
    return nullptr;
}

std::string SkipList::checkInvariants() const {
    // Position of every node along level 0 (header = 0, first element = 1).
    std::unordered_map<const Node*, std::size_t> position;
    std::size_t count = 0;
    const Node* prev = nullptr;
    for (const Node* x = header_.get(); x != nullptr; x = x->next.get()) {
        if (x->links[0].forward != x->next.get()) return "links[0].forward differs from next";
        if (x != header_.get()) {
            if (prev != header_.get() && !before(prev, x->score, x->member)) return "level 0 is not strictly sorted";
            ++count;
        }
        position[x] = count;
        prev = x;
    }
    if (count != size_) return "size does not match the number of nodes";
    if (level_ < 1 || level_ > kMaxLevel) return "level out of range";
    if (level_ > 1 && header_->links[level_ - 1].forward == nullptr) return "top level is empty";

    for (const auto& [node, pos] : position) {
        const int levels = node == header_.get() ? level_ : static_cast<int>(node->links.size());
        if (levels > level_) return "node is taller than the list";
        for (int i = 0; i < levels; ++i) {
            const Node* fwd = node->links[i].forward;
            const std::size_t expected = fwd != nullptr ? position.at(fwd) - pos : size_ - pos;
            if (node->links[i].span != expected) return "wrong span at level " + std::to_string(i);
            if (fwd != nullptr && static_cast<int>(fwd->links.size()) <= i) return "link points to a shorter node";
        }
    }
    return "";
}

// ------------------------------------------------------------------ ZSet

ZSet::AddResult ZSet::add(const std::string& member, double score) {
    const auto it = scores_.find(member);
    if (it == scores_.end()) {
        scores_.emplace(member, score);
        list_.insert(score, member);
        return AddResult::Added;
    }
    if (it->second == score) return AddResult::Unchanged;
    // The node's position depends on its score, so a new score means remove and
    // re-insert. (Redis updates in place when the order would not change; that
    // is an optimization, not a different result.)
    list_.erase(it->second, member);
    list_.insert(score, member);
    it->second = score;
    return AddResult::Updated;
}

bool ZSet::remove(const std::string& member) {
    const auto it = scores_.find(member);
    if (it == scores_.end()) return false;
    list_.erase(it->second, member);
    scores_.erase(it);
    return true;
}

std::optional<double> ZSet::score(const std::string& member) const {
    const auto it = scores_.find(member);
    if (it == scores_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::size_t> ZSet::rank(const std::string& member) const {
    const auto it = scores_.find(member);
    if (it == scores_.end()) return std::nullopt;
    return list_.rank(it->second, member);
}
