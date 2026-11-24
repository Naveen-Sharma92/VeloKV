// Unit tests for the skip list and the sorted set built on it, including a
// randomized stress test that checks every operation against std::map (a
// balanced tree with the same ordering) and verifies all structural
// invariants (order, spans, ownership) as it goes.
#include "skiplist.h"

#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Entry = std::pair<double, std::string>;

std::vector<Entry> contents(const SkipList& list) {
    std::vector<Entry> out;
    for (const SkipList::Node* n = list.at(0); n != nullptr; n = n->next.get()) out.emplace_back(n->score, n->member);
    return out;
}

}  // namespace

// ------------------------------------------------------------------ SkipList

TEST(SkipList, KeepsElementsSortedByScoreThenMember) {
    SkipList list;
    list.insert(2, "b");
    list.insert(1, "z");
    list.insert(2, "a");  // equal scores: ordered by member
    list.insert(-5, "m");
    EXPECT_EQ(contents(list), (std::vector<Entry>{{-5, "m"}, {1, "z"}, {2, "a"}, {2, "b"}}));
    EXPECT_EQ(list.size(), 4u);
    EXPECT_EQ(list.checkInvariants(), "");
}

TEST(SkipList, RankAndAt) {
    SkipList list;
    for (int i = 0; i < 100; ++i) list.insert(i * 10, "m" + std::to_string(i));
    EXPECT_EQ(list.rank(0, "m0"), 0u);
    EXPECT_EQ(list.rank(500, "m50"), 50u);
    EXPECT_EQ(list.rank(990, "m99"), 99u);
    EXPECT_FALSE(list.rank(500, "other"));  // right score, wrong member
    EXPECT_FALSE(list.rank(501, "m50"));    // right member, wrong score
    ASSERT_NE(list.at(42), nullptr);
    EXPECT_EQ(list.at(42)->member, "m42");
    EXPECT_EQ(list.at(100), nullptr);
}

TEST(SkipList, Erase) {
    SkipList list;
    list.insert(1, "a");
    list.insert(2, "b");
    list.insert(3, "c");
    EXPECT_TRUE(list.erase(2, "b"));
    EXPECT_FALSE(list.erase(2, "b"));
    EXPECT_FALSE(list.erase(1, "c"));  // the pair must match
    EXPECT_EQ(contents(list), (std::vector<Entry>{{1, "a"}, {3, "c"}}));
    EXPECT_EQ(list.rank(3, "c"), 1u);
    EXPECT_EQ(list.checkInvariants(), "");
    EXPECT_TRUE(list.erase(1, "a"));
    EXPECT_TRUE(list.erase(3, "c"));
    EXPECT_EQ(list.size(), 0u);
    EXPECT_EQ(list.at(0), nullptr);
    EXPECT_EQ(list.checkInvariants(), "");
}

TEST(SkipList, InfiniteScoresSortAtTheEnds) {
    SkipList list;
    list.insert(std::numeric_limits<double>::infinity(), "top");
    list.insert(-std::numeric_limits<double>::infinity(), "bottom");
    list.insert(0, "middle");
    EXPECT_EQ(list.at(0)->member, "bottom");
    EXPECT_EQ(list.at(2)->member, "top");
}

TEST(SkipList, MoveTransfersTheElements) {
    SkipList a;
    a.insert(1, "x");
    SkipList b(std::move(a));
    EXPECT_EQ(b.size(), 1u);
    SkipList c;
    c.insert(5, "old");
    c = std::move(b);  // c's old node is freed, b's node now belongs to c
    EXPECT_EQ(contents(c), (std::vector<Entry>{{1, "x"}}));
}

TEST(SkipList, DestroyingAMillionElementsDoesNotOverflowTheStack) {
    // Each node owns the next one; a naive recursive destructor would need one
    // stack frame per element.
    SkipList list;
    for (int i = 0; i < 1'000'000; ++i) list.insert(i, "m");
    EXPECT_EQ(list.size(), 1'000'000u);
}

TEST(SkipList, RandomizedStressAgainstStdMap) {
    // std::map is a red-black tree; ordering std::pair<double, string> compares
    // score then member, exactly our order. A small score range forces many ties.
    std::mt19937 rng(12345);
    SkipList list;
    std::map<Entry, bool> reference;

    auto randomEntry = [&] {
        return Entry(static_cast<double>(rng() % 50) / 2, "m" + std::to_string(rng() % 300));
    };

    for (int step = 0; step < 20000; ++step) {
        const Entry e = randomEntry();
        const auto op = rng() % 10;
        if (op < 5) {  // insert, if absent
            if (reference.count(e) == 0) {
                list.insert(e.first, e.second);
                reference[e] = true;
            }
        } else if (op < 8) {  // erase, present or not
            EXPECT_EQ(list.erase(e.first, e.second), reference.erase(e) == 1);
        } else {  // rank of an element that exists, and at() of a random rank
            if (!reference.empty()) {
                const auto it = std::next(reference.begin(), static_cast<long>(rng() % reference.size()));
                const auto expectedRank = static_cast<std::size_t>(std::distance(reference.begin(), it));
                ASSERT_EQ(list.rank(it->first.first, it->first.second), expectedRank);
                const SkipList::Node* node = list.at(expectedRank);
                ASSERT_NE(node, nullptr);
                ASSERT_EQ(Entry(node->score, node->member), it->first);
            }
            EXPECT_FALSE(list.rank(-1, "absent"));
        }
        ASSERT_EQ(list.size(), reference.size());
        if (step % 500 == 0) {
            ASSERT_EQ(list.checkInvariants(), "") << "after step " << step;
        }
    }

    ASSERT_EQ(list.checkInvariants(), "");
    std::vector<Entry> expected;
    for (const auto& kv : reference) expected.push_back(kv.first);
    EXPECT_EQ(contents(list), expected);
    std::size_t rank = 0;
    for (const auto& e : expected) EXPECT_EQ(list.rank(e.first, e.second), rank++);
}

// ------------------------------------------------------------------ ZSet

TEST(ZSet, AddUpdateAndRemoveKeepBothIndexesInStep) {
    ZSet z;
    EXPECT_EQ(z.add("alice", 100), ZSet::AddResult::Added);
    EXPECT_EQ(z.add("bob", 250), ZSet::AddResult::Added);
    EXPECT_EQ(z.add("alice", 100), ZSet::AddResult::Unchanged);
    EXPECT_EQ(z.rank("alice"), 0u);

    EXPECT_EQ(z.add("alice", 300), ZSet::AddResult::Updated);  // moves past bob
    EXPECT_EQ(z.score("alice"), 300);
    EXPECT_EQ(z.rank("alice"), 1u);
    EXPECT_EQ(z.rank("bob"), 0u);
    EXPECT_EQ(z.size(), 2u);
    EXPECT_EQ(z.ordered().size(), 2u);

    EXPECT_TRUE(z.remove("bob"));
    EXPECT_FALSE(z.remove("bob"));
    EXPECT_FALSE(z.score("bob"));
    EXPECT_FALSE(z.rank("bob"));
    EXPECT_EQ(z.rank("alice"), 0u);
    EXPECT_EQ(z.ordered().checkInvariants(), "");
}

TEST(ZSet, RandomizedStressAgainstStdMap) {
    // member -> score, plus the derived order, checked after every operation.
    std::mt19937 rng(777);
    ZSet z;
    std::map<std::string, double> scores;
    for (int step = 0; step < 10000; ++step) {
        const std::string member = "m" + std::to_string(rng() % 200);
        if (rng() % 3 != 0) {
            const double score = static_cast<double>(rng() % 20);
            z.add(member, score);
            scores[member] = score;
        } else {
            EXPECT_EQ(z.remove(member), scores.erase(member) == 1);
        }
        ASSERT_EQ(z.size(), scores.size());
    }
    std::map<Entry, bool> order;
    for (const auto& [member, score] : scores) order[{score, member}] = true;
    std::size_t rank = 0;
    for (const auto& kv : order) {
        EXPECT_EQ(z.score(kv.first.second), kv.first.first);
        EXPECT_EQ(z.rank(kv.first.second), rank++);
    }
    EXPECT_EQ(z.ordered().checkInvariants(), "");
}
