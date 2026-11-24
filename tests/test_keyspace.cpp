// Unit tests for the keyspace: storing and replacing values of any type, and
// typed lookups that refuse a key holding another type.
#include "keyspace.h"

#include <string>

#include <gtest/gtest.h>

TEST(Keyspace, StoresFindsAndErases) {
    Keyspace db;
    EXPECT_EQ(db.find("k"), nullptr);
    db.set("k", std::string("v"));
    ASSERT_NE(db.findString("k"), nullptr);
    EXPECT_EQ(*db.findString("k"), "v");
    EXPECT_EQ(db.size(), 1u);
    EXPECT_TRUE(db.erase("k"));
    EXPECT_FALSE(db.erase("k"));
    EXPECT_EQ(db.size(), 0u);
}

TEST(Keyspace, SetReplacesAValueOfAnyType) {
    Keyspace db;
    db.set("k", List{"a", "b"});
    db.set("k", std::string("now a string"));
    EXPECT_EQ(*db.findString("k"), "now a string");
    EXPECT_EQ(db.size(), 1u);
}

TEST(Keyspace, FindStringOnAnotherTypeThrowsWrongType) {
    Keyspace db;
    db.set("list", List{"a"});
    db.set("hash", Hash{{"f", "v"}});
    EXPECT_THROW(db.findString("list"), WrongTypeError);
    EXPECT_THROW(db.findString("hash"), WrongTypeError);
    EXPECT_EQ(db.findString("missing"), nullptr);  // missing is not a type error
}

TEST(Keyspace, WrongTypeErrorHasRedisText) {
    EXPECT_STREQ(WrongTypeError().what(), "WRONGTYPE Operation against a key holding the wrong kind of value");
}

TEST(Keyspace, TypeNames) {
    EXPECT_STREQ(typeName(Value(std::string("x"))), "string");
    EXPECT_STREQ(typeName(Value(List{})), "list");
    EXPECT_STREQ(typeName(Value(Hash{})), "hash");
    EXPECT_STREQ(typeName(Value(ZSet{})), "zset");
}

TEST(Keyspace, ClearRemovesEverything) {
    Keyspace db;
    db.set("a", std::string("1"));
    db.set("b", List{"x"});
    db.clear();
    EXPECT_EQ(db.size(), 0u);
    EXPECT_EQ(db.find("a"), nullptr);
}

TEST(Keyspace, TypedLookupsForListsAndHashes) {
    Keyspace db;
    db.set("s", std::string("v"));
    db.set("l", List{"a"});
    db.set("h", Hash{{"f", "v"}});
    ASSERT_NE(db.findList("l"), nullptr);
    EXPECT_EQ(db.findList("l")->front(), "a");
    ASSERT_NE(db.findHash("h"), nullptr);
    EXPECT_EQ(db.findHash("h")->at("f"), "v");
    EXPECT_EQ(db.findList("missing"), nullptr);
    EXPECT_THROW(db.findList("s"), WrongTypeError);
    EXPECT_THROW(db.findList("h"), WrongTypeError);
    EXPECT_THROW(db.findHash("l"), WrongTypeError);
}

TEST(Keyspace, SetReturnsTheStoredValueForInPlaceFilling) {
    Keyspace db;
    std::get<List>(db.set("l", List{})).push_back("x");
    EXPECT_EQ(db.findList("l")->size(), 1u);
}
