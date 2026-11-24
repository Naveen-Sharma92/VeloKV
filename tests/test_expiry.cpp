// Unit tests for TTL tracking: the deadline heap (ordering, stale entries,
// compaction) and the keyspace's lazy and active expiry, driven by a fake clock
// so no test has to sleep.
#include "expiry.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "keyspace.h"

using Keys = std::vector<std::string>;

// ------------------------------------------------------------------ Expiry

TEST(Expiry, TracksDeadlines) {
    Expiry e;
    EXPECT_FALSE(e.deadline("k"));
    e.set("k", 100);
    EXPECT_EQ(e.deadline("k"), 100);
    e.set("k", 200);
    EXPECT_EQ(e.deadline("k"), 200);
    EXPECT_TRUE(e.remove("k"));
    EXPECT_FALSE(e.remove("k"));
    EXPECT_FALSE(e.deadline("k"));
}

TEST(Expiry, TakeDueReturnsExpiredKeysEarliestFirst) {
    Expiry e;
    e.set("c", 300);
    e.set("a", 100);
    e.set("d", 400);
    e.set("b", 200);
    EXPECT_EQ(e.takeDue(100, 10), Keys{});  // a deadline equal to now has not passed yet
    EXPECT_EQ(e.takeDue(301, 10), (Keys{"a", "b", "c"}));
    EXPECT_EQ(e.size(), 1u);
    EXPECT_EQ(e.takeDue(1000, 10), Keys{"d"});
    EXPECT_EQ(e.size(), 0u);
}

TEST(Expiry, TakeDueRespectsTheBudget) {
    Expiry e;
    for (int i = 0; i < 5; ++i) e.set("k" + std::to_string(i), 10 + i);
    EXPECT_EQ(e.takeDue(100, 2).size(), 2u);
    EXPECT_EQ(e.takeDue(100, 2).size(), 2u);
    EXPECT_EQ(e.takeDue(100, 2).size(), 1u);
}

TEST(Expiry, StaleEntriesAreSkipped) {
    Expiry e;
    e.set("moved", 100);
    e.set("moved", 500);  // new TTL: the (100, moved) heap entry is now stale
    e.set("gone", 100);
    e.remove("gone");  // TTL removed: (100, gone) is stale

    EXPECT_EQ(e.takeDue(200, 10), Keys{});
    EXPECT_EQ(e.deadline("moved"), 500);
    EXPECT_EQ(e.takeDue(501, 10), Keys{"moved"});  // exactly once
    EXPECT_EQ(e.heapSize(), 0u);
}

TEST(Expiry, HeapIsCompactedWhenStaleEntriesPileUp) {
    // One key whose TTL is refreshed 100,000 times, like a session timeout.
    Expiry e;
    for (int i = 0; i < 100000; ++i) e.set("session", 1000 + i);
    EXPECT_LE(e.heapSize(), 2 * e.size() + 1024 + 1);

    EXPECT_EQ(e.takeDue(1000 + 99998, 10), Keys{});  // older deadlines are all stale
    EXPECT_EQ(e.takeDue(1000 + 100000, 10), Keys{"session"});
}

// ------------------------------------------------------------------ Keyspace

namespace {

class KeyspaceExpiryTest : public ::testing::Test {
protected:
    std::int64_t now = 1'700'000'000'000;  // a fixed "current" Unix time in ms
    Keyspace db{[this] { return now; }};
};

}  // namespace

TEST_F(KeyspaceExpiryTest, LazyExpiryHidesAndDeletesAKeyOnAccess) {
    db.set("k", std::string("v"));
    ASSERT_EQ(db.setExpiry("k", now + 100), ExpiryResult::Scheduled);

    now += 100;  // exactly at the deadline: still alive
    EXPECT_NE(db.find("k"), nullptr);
    now += 1;
    EXPECT_EQ(db.size(), 1u);  // not yet removed: nobody looked
    EXPECT_EQ(db.find("k"), nullptr);
    EXPECT_EQ(db.size(), 0u);  // the lookup deleted it
}

TEST_F(KeyspaceExpiryTest, ActiveExpiryDeletesKeysNobodyReads) {
    for (int i = 0; i < 10; ++i) {
        const std::string key = "k" + std::to_string(i);
        db.set(key, std::string("v"));
        db.setExpiry(key, now + 10 * (i + 1));  // deadlines at +10, +20, ... +100
    }
    db.set("forever", std::string("v"));

    now += 55;
    EXPECT_EQ(db.expireDue(100), 5u);  // +10 .. +50
    EXPECT_EQ(db.size(), 6u);
    now += 1000;
    EXPECT_EQ(db.expireDue(2), 2u);  // the budget caps each sweep
    EXPECT_EQ(db.expireDue(100), 3u);
    EXPECT_EQ(db.size(), 1u);
    EXPECT_NE(db.find("forever"), nullptr);
}

TEST_F(KeyspaceExpiryTest, SetRemovesTheTtl) {
    db.set("k", std::string("v"));
    db.setExpiry("k", now + 100);
    db.set("k", std::string("v2"));
    EXPECT_FALSE(db.expiryOf("k"));
    now += 1000;
    EXPECT_EQ(db.expireDue(100), 0u);  // the old heap entry is stale
    EXPECT_NE(db.find("k"), nullptr);
}

TEST_F(KeyspaceExpiryTest, DeletedAndRecreatedKeyIsNotExpiredByTheOldEntry) {
    db.set("k", std::string("v"));
    db.setExpiry("k", now + 100);
    db.erase("k");
    db.set("k", std::string("new"));  // no TTL this time
    now += 1000;
    EXPECT_EQ(db.expireDue(100), 0u);
    EXPECT_EQ(*db.findString("k"), "new");
}

TEST_F(KeyspaceExpiryTest, SetExpiryOnMissingKeyOrInThePast) {
    EXPECT_EQ(db.setExpiry("missing", now + 100), ExpiryResult::NoSuchKey);
    db.set("k", std::string("v"));
    EXPECT_EQ(db.setExpiry("k", now), ExpiryResult::DeletedNow);  // deadline not in the future
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(KeyspaceExpiryTest, PersistRemovesTheTtl) {
    db.set("k", std::string("v"));
    EXPECT_FALSE(db.persist("k"));  // no TTL to remove
    db.setExpiry("k", now + 100);
    EXPECT_TRUE(db.persist("k"));
    now += 1000;
    EXPECT_NE(db.find("k"), nullptr);
    EXPECT_FALSE(db.persist("missing"));
}

TEST_F(KeyspaceExpiryTest, EraseOfAnExpiredKeyReportsNothingRemoved) {
    db.set("k", std::string("v"));
    db.setExpiry("k", now + 10);
    now += 11;
    EXPECT_FALSE(db.erase("k"));
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(KeyspaceExpiryTest, ClearForgetsTtls) {
    db.set("k", std::string("v"));
    db.setExpiry("k", now + 10);
    db.clear();
    db.set("k", std::string("v"));
    now += 100;
    EXPECT_EQ(db.expireDue(100), 0u);
    EXPECT_NE(db.find("k"), nullptr);
}
