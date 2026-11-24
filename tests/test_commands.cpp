// Unit tests for command dispatch and the handlers. Expected replies were checked
// byte for byte against redis-server 7.2.
#include "commands.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "keyspace.h"

namespace {

class CommandsTest : public ::testing::Test {
protected:
    std::string run(const std::vector<std::string>& args) {
        std::string out;
        executeCommand(db, args, out);
        return out;
    }

    std::int64_t now = 1'700'000'000'000;  // fake clock, Unix ms; tests advance it
    Keyspace db{[this] { return now; }};
};

const std::string kOk = "+OK\r\n";
const std::string kNil = "$-1\r\n";
const std::string kWrongType = "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
const std::string kNotInteger = "-ERR value is not an integer or out of range\r\n";
const std::string kSyntax = "-ERR syntax error\r\n";

}  // namespace

// ------------------------------------------------------------------ connection

TEST_F(CommandsTest, Ping) {
    EXPECT_EQ(run({"PING"}), "+PONG\r\n");
    EXPECT_EQ(run({"PING", "hello"}), "$5\r\nhello\r\n");
    EXPECT_EQ(run({"PING", "a", "b"}), "-ERR wrong number of arguments for 'ping' command\r\n");
}

TEST_F(CommandsTest, NamesAreCaseInsensitive) {
    EXPECT_EQ(run({"ping"}), "+PONG\r\n");
    EXPECT_EQ(run({"pInG"}), "+PONG\r\n");
}

TEST_F(CommandsTest, Echo) {
    EXPECT_EQ(run({"ECHO", "hi"}), "$2\r\nhi\r\n");
    EXPECT_EQ(run({"ECHO", ""}), "$0\r\n\r\n");
    EXPECT_EQ(run({"ECHO"}), "-ERR wrong number of arguments for 'echo' command\r\n");
    EXPECT_EQ(run({"ECHO", "a", "b"}), "-ERR wrong number of arguments for 'echo' command\r\n");
}

TEST_F(CommandsTest, CommandStubRepliesWithEmptyArray) {
    EXPECT_EQ(run({"COMMAND"}), "*0\r\n");
    EXPECT_EQ(run({"COMMAND", "DOCS"}), "*0\r\n");
}

TEST_F(CommandsTest, ConfigGetStubRepliesWithEmptyArray) {
    EXPECT_EQ(run({"CONFIG", "GET", "save"}), "*0\r\n");
    EXPECT_EQ(run({"config", "get", "appendonly"}), "*0\r\n");
    EXPECT_EQ(run({"CONFIG", "GET"}), "-ERR wrong number of arguments for 'config|get' command\r\n");
    EXPECT_EQ(run({"CONFIG", "SET", "x", "y"}), "-ERR unknown subcommand 'SET'. Try CONFIG HELP.\r\n");
    EXPECT_EQ(run({"CONFIG"}), "-ERR wrong number of arguments for 'config' command\r\n");
}

TEST_F(CommandsTest, UnknownCommandMatchesRedisWording) {
    EXPECT_EQ(run({"FOO"}), "-ERR unknown command 'FOO', with args beginning with: \r\n");
    EXPECT_EQ(run({"foo", "a", "b"}), "-ERR unknown command 'foo', with args beginning with: 'a' 'b' \r\n");
}

TEST_F(CommandsTest, UnknownCommandErrorIsTruncated) {
    const std::string longArg(300, 'x');
    // Like Redis: at most 128 characters of arguments are shown.
    EXPECT_EQ(run({"FOO", longArg, "next"}),
              "-ERR unknown command 'FOO', with args beginning with: '" + std::string(128, 'x') + "' \r\n");
}

// ------------------------------------------------------------------ strings

TEST_F(CommandsTest, SetAndGet) {
    EXPECT_EQ(run({"GET", "k"}), kNil);
    EXPECT_EQ(run({"SET", "k", "hello"}), kOk);
    EXPECT_EQ(run({"GET", "k"}), "$5\r\nhello\r\n");
    EXPECT_EQ(run({"SET", "k", "world"}), kOk);
    EXPECT_EQ(run({"GET", "k"}), "$5\r\nworld\r\n");
}

TEST_F(CommandsTest, ValuesAreBinarySafe) {
    const std::string value("a\0b\r\nc", 6);
    EXPECT_EQ(run({"SET", "k", value}), kOk);
    EXPECT_EQ(run({"GET", "k"}), "$6\r\n" + value + "\r\n");
}

TEST_F(CommandsTest, SetNxAndXx) {
    EXPECT_EQ(run({"SET", "k", "1", "XX"}), kNil);  // XX: key must exist
    EXPECT_EQ(run({"GET", "k"}), kNil);
    EXPECT_EQ(run({"SET", "k", "1", "nx"}), kOk);  // NX: key must not exist
    EXPECT_EQ(run({"SET", "k", "2", "NX"}), kNil);
    EXPECT_EQ(run({"SET", "k", "3", "XX"}), kOk);
    EXPECT_EQ(run({"GET", "k"}), "$1\r\n3\r\n");
}

TEST_F(CommandsTest, SetRejectsBadOptions) {
    EXPECT_EQ(run({"SET", "k", "v", "NX", "XX"}), kSyntax);
    EXPECT_EQ(run({"SET", "k", "v", "FOO"}), kSyntax);
    EXPECT_EQ(run({"SET", "k"}), "-ERR wrong number of arguments for 'set' command\r\n");
    EXPECT_EQ(run({"GET", "k"}), kNil);  // nothing was stored
}

TEST_F(CommandsTest, SetOverwritesAnyType) {
    db.set("k", List{"a"});
    EXPECT_EQ(run({"SET", "k", "v"}), kOk);
    EXPECT_EQ(run({"TYPE", "k"}), "+string\r\n");
}

TEST_F(CommandsTest, MsetAndMget) {
    EXPECT_EQ(run({"MSET", "a", "1", "b", "2"}), kOk);
    db.set("list", List{"x"});
    // Missing keys and keys of another type are both nil, not errors.
    EXPECT_EQ(run({"MGET", "a", "missing", "b", "list"}), "*4\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n$-1\r\n");
}

TEST_F(CommandsTest, MsetNeedsPairs) {
    EXPECT_EQ(run({"MSET", "a"}), "-ERR wrong number of arguments for 'mset' command\r\n");
    EXPECT_EQ(run({"MSET", "a", "1", "b"}), "-ERR wrong number of arguments for 'mset' command\r\n");
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(CommandsTest, Incr) {
    EXPECT_EQ(run({"INCR", "n"}), ":1\r\n");  // missing counts as 0
    EXPECT_EQ(run({"INCR", "n"}), ":2\r\n");
    EXPECT_EQ(run({"DECR", "n"}), ":1\r\n");
    EXPECT_EQ(run({"INCRBY", "n", "-10"}), ":-9\r\n");
    EXPECT_EQ(run({"GET", "n"}), "$2\r\n-9\r\n");  // stored as a decimal string
}

TEST_F(CommandsTest, IncrRejectsValuesRedisDoesNotTreatAsIntegers) {
    for (const std::string bad : {"abc", "", "1.5", "01", "-0", "+1", " 1", "1 ", "9223372036854775808"}) {
        db.set("n", bad);
        EXPECT_EQ(run({"INCR", "n"}), kNotInteger) << "value [" << bad << "]";
        EXPECT_EQ(*db.findString("n"), bad);  // untouched
    }
    db.set("n", std::string("0"));
    EXPECT_EQ(run({"INCR", "n"}), ":1\r\n");
}

TEST_F(CommandsTest, IncrbyRejectsANonIntegerIncrement) {
    EXPECT_EQ(run({"INCRBY", "n", "x"}), kNotInteger);
    EXPECT_EQ(run({"INCRBY", "n", "+5"}), kNotInteger);
    EXPECT_EQ(db.size(), 0u);  // no key was created
}

TEST_F(CommandsTest, IncrDetectsOverflowAndLeavesTheValueAlone) {
    EXPECT_EQ(run({"SET", "n", "9223372036854775807"}), kOk);
    EXPECT_EQ(run({"INCR", "n"}), "-ERR increment or decrement would overflow\r\n");
    EXPECT_EQ(run({"GET", "n"}), "$19\r\n9223372036854775807\r\n");

    EXPECT_EQ(run({"SET", "m", "-9223372036854775808"}), kOk);
    EXPECT_EQ(run({"DECR", "m"}), "-ERR increment or decrement would overflow\r\n");

    EXPECT_EQ(run({"SET", "p", "-2"}), kOk);
    EXPECT_EQ(run({"INCRBY", "p", "-9223372036854775807"}), "-ERR increment or decrement would overflow\r\n");
    EXPECT_EQ(run({"INCRBY", "p", "-9223372036854775806"}), ":-9223372036854775808\r\n");  // exactly LLONG_MIN
}

TEST_F(CommandsTest, Append) {
    EXPECT_EQ(run({"APPEND", "k", "Hello"}), ":5\r\n");  // creates the key
    EXPECT_EQ(run({"APPEND", "k", " World"}), ":11\r\n");
    EXPECT_EQ(run({"GET", "k"}), "$11\r\nHello World\r\n");
}

TEST_F(CommandsTest, StringCommandsOnAListGiveWrongType) {
    db.set("l", List{"a"});
    EXPECT_EQ(run({"GET", "l"}), kWrongType);
    EXPECT_EQ(run({"INCR", "l"}), kWrongType);
    EXPECT_EQ(run({"INCRBY", "l", "5"}), kWrongType);
    EXPECT_EQ(run({"APPEND", "l", "x"}), kWrongType);
    EXPECT_EQ(run({"TYPE", "l"}), "+list\r\n");  // still a list, unchanged
}

// ------------------------------------------------------------------ keys

TEST_F(CommandsTest, DelCountsKeysActuallyRemoved) {
    run({"MSET", "a", "1", "b", "2"});
    EXPECT_EQ(run({"DEL", "a", "missing", "a", "b"}), ":2\r\n");
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(CommandsTest, ExistsCountsRepeatedKeys) {
    run({"SET", "a", "1"});
    EXPECT_EQ(run({"EXISTS", "a", "a", "missing"}), ":2\r\n");
}

TEST_F(CommandsTest, Type) {
    db.set("s", std::string("v"));
    db.set("l", List{"a"});
    db.set("h", Hash{{"f", "v"}});
    EXPECT_EQ(run({"TYPE", "s"}), "+string\r\n");
    EXPECT_EQ(run({"TYPE", "l"}), "+list\r\n");
    EXPECT_EQ(run({"TYPE", "h"}), "+hash\r\n");
    EXPECT_EQ(run({"TYPE", "missing"}), "+none\r\n");
}

TEST_F(CommandsTest, DbsizeAndFlushall) {
    run({"MSET", "a", "1", "b", "2", "c", "3"});
    EXPECT_EQ(run({"DBSIZE"}), ":3\r\n");
    EXPECT_EQ(run({"FLUSHALL", "x"}), kSyntax);
    EXPECT_EQ(run({"DBSIZE"}), ":3\r\n");
    EXPECT_EQ(run({"FLUSHALL"}), kOk);
    EXPECT_EQ(run({"DBSIZE"}), ":0\r\n");
    run({"SET", "a", "1"});
    EXPECT_EQ(run({"flushall", "ASYNC"}), kOk);
    EXPECT_EQ(run({"DBSIZE"}), ":0\r\n");
}

// ------------------------------------------------------------------ expiry

TEST_F(CommandsTest, SetWithExAndPx) {
    EXPECT_EQ(run({"SET", "k", "v", "EX", "10"}), kOk);
    EXPECT_EQ(run({"TTL", "k"}), ":10\r\n");
    EXPECT_EQ(run({"PTTL", "k"}), ":10000\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "px", "2500"}), kOk);
    EXPECT_EQ(run({"PTTL", "k"}), ":2500\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":3\r\n");  // 2.5 s rounds to 3, like Redis
}

TEST_F(CommandsTest, KeyExpiresAfterItsDeadline) {
    EXPECT_EQ(run({"SET", "k", "v", "PX", "200"}), kOk);
    now += 200;
    EXPECT_EQ(run({"GET", "k"}), "$1\r\nv\r\n");  // at the deadline: still there
    now += 100;
    EXPECT_EQ(run({"GET", "k"}), kNil);
    EXPECT_EQ(run({"TTL", "k"}), ":-2\r\n");
    EXPECT_EQ(run({"EXISTS", "k"}), ":0\r\n");
}

TEST_F(CommandsTest, SetWithoutExpiryClearsTheTtl) {
    run({"SET", "k", "v", "EX", "10"});
    EXPECT_EQ(run({"SET", "k", "v2"}), kOk);
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");
}

TEST_F(CommandsTest, SetExpiryOptionErrors) {
    EXPECT_EQ(run({"SET", "k", "v", "EX", "0"}), "-ERR invalid expire time in 'set' command\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "PX", "-5"}), "-ERR invalid expire time in 'set' command\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "EX", "9223372036854776"}), "-ERR invalid expire time in 'set' command\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "EX", "ten"}), kNotInteger);
    EXPECT_EQ(run({"SET", "k", "v", "EX"}), kSyntax);
    EXPECT_EQ(run({"SET", "k", "v", "EX", "10", "PX", "100"}), kSyntax);
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(CommandsTest, FailedSetNxLeavesTheTtlAlone) {
    run({"SET", "k", "v", "EX", "100"});
    EXPECT_EQ(run({"SET", "k", "other", "NX", "EX", "5"}), kNil);
    EXPECT_EQ(run({"TTL", "k"}), ":100\r\n");
}

TEST_F(CommandsTest, ExpireAndPexpire) {
    EXPECT_EQ(run({"EXPIRE", "missing", "10"}), ":0\r\n");
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");
    EXPECT_EQ(run({"EXPIRE", "k", "100"}), ":1\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":100\r\n");
    EXPECT_EQ(run({"PEXPIRE", "k", "1499"}), ":1\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":1\r\n");  // 1.499 s rounds down
    now += 1500;
    EXPECT_EQ(run({"GET", "k"}), kNil);
}

TEST_F(CommandsTest, ExpireWithANonPositiveTtlDeletesTheKey) {
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"EXPIRE", "k", "-1"}), ":1\r\n");
    EXPECT_EQ(db.size(), 0u);
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"PEXPIRE", "k", "0"}), ":1\r\n");
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(CommandsTest, ExpireErrors) {
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"EXPIRE", "k", "soon"}), kNotInteger);
    EXPECT_EQ(run({"EXPIRE", "k", "9223372036854775807"}), "-ERR invalid expire time in 'expire' command\r\n");
    EXPECT_EQ(run({"PEXPIRE", "k", "9223372036854775807"}), "-ERR invalid expire time in 'pexpire' command\r\n");
    EXPECT_EQ(run({"EXPIRE", "k"}), "-ERR wrong number of arguments for 'expire' command\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");  // unchanged
}

TEST_F(CommandsTest, Persist) {
    EXPECT_EQ(run({"PERSIST", "missing"}), ":0\r\n");
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"PERSIST", "k"}), ":0\r\n");  // had no TTL
    run({"EXPIRE", "k", "10"});
    EXPECT_EQ(run({"PERSIST", "k"}), ":1\r\n");
    EXPECT_EQ(run({"TTL", "k"}), ":-1\r\n");
}

TEST_F(CommandsTest, InPlaceUpdatesKeepTheTtlButMsetClearsIt) {
    run({"SET", "n", "1", "EX", "100"});
    run({"INCR", "n"});
    EXPECT_EQ(run({"TTL", "n"}), ":100\r\n");
    run({"SET", "s", "a", "EX", "100"});
    run({"APPEND", "s", "b"});
    EXPECT_EQ(run({"TTL", "s"}), ":100\r\n");
    run({"MSET", "s", "x"});
    EXPECT_EQ(run({"TTL", "s"}), ":-1\r\n");
}

TEST_F(CommandsTest, DelOfAnExpiredKeyCountsZero) {
    run({"SET", "k", "v", "PX", "10"});
    now += 11;
    EXPECT_EQ(run({"DEL", "k"}), ":0\r\n");
}

TEST_F(CommandsTest, Pexpireat) {
    EXPECT_EQ(run({"PEXPIREAT", "missing", std::to_string(now + 5000)}), ":0\r\n");
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"PEXPIREAT", "k", std::to_string(now + 5000)}), ":1\r\n");
    EXPECT_EQ(run({"PTTL", "k"}), ":5000\r\n");
    EXPECT_EQ(run({"PEXPIREAT", "k", std::to_string(now - 1)}), ":1\r\n");  // in the past: deleted
    EXPECT_EQ(run({"EXISTS", "k"}), ":0\r\n");
    EXPECT_EQ(run({"PEXPIREAT", "k", "soon"}), kNotInteger);
}

TEST_F(CommandsTest, SetWithAbsoluteExpiry) {
    EXPECT_EQ(run({"SET", "k", "v", "PXAT", std::to_string(now + 2500)}), kOk);
    EXPECT_EQ(run({"PTTL", "k"}), ":2500\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "EXAT", std::to_string(now / 1000 + 10)}), kOk);
    EXPECT_EQ(run({"TTL", "k"}), ":10\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "PXAT", std::to_string(now - 1)}), kOk);  // already past
    EXPECT_EQ(run({"GET", "k"}), kNil);
    EXPECT_EQ(run({"SET", "k", "v", "PXAT", "0"}), "-ERR invalid expire time in 'set' command\r\n");
    EXPECT_EQ(run({"SET", "k", "v", "EX", "5", "PXAT", "100"}), kSyntax);
}

// ------------------------------------------------------------------ lists

TEST_F(CommandsTest, PushAndRange) {
    EXPECT_EQ(run({"LPUSH", "l", "a", "b", "c"}), ":3\r\n");  // each goes to the front: c b a
    EXPECT_EQ(run({"RPUSH", "l", "x"}), ":4\r\n");
    EXPECT_EQ(run({"LRANGE", "l", "0", "-1"}), "*4\r\n$1\r\nc\r\n$1\r\nb\r\n$1\r\na\r\n$1\r\nx\r\n");
    EXPECT_EQ(run({"LLEN", "l"}), ":4\r\n");
    EXPECT_EQ(run({"LLEN", "missing"}), ":0\r\n");
}

TEST_F(CommandsTest, LrangeIndicesFollowRedis) {
    run({"RPUSH", "l", "a", "b", "c", "d", "e"});
    auto range = [&](const std::string& start, const std::string& stop) { return run({"LRANGE", "l", start, stop}); };
    EXPECT_EQ(range("1", "2"), "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    EXPECT_EQ(range("-2", "-1"), "*2\r\n$1\r\nd\r\n$1\r\ne\r\n");   // negative = from the end
    EXPECT_EQ(range("-100", "1"), "*2\r\n$1\r\na\r\n$1\r\nb\r\n");  // start clamped to 0
    EXPECT_EQ(range("3", "100"), "*2\r\n$1\r\nd\r\n$1\r\ne\r\n");   // stop clamped to the end
    EXPECT_EQ(range("4", "4"), "*1\r\n$1\r\ne\r\n");
    EXPECT_EQ(range("3", "1"), "*0\r\n");   // start after stop
    EXPECT_EQ(range("5", "10"), "*0\r\n");  // start past the end
    EXPECT_EQ(range("-1", "0"), "*0\r\n");
    EXPECT_EQ(range("-9223372036854775808", "9223372036854775807").substr(0, 4), "*5\r\n");
    EXPECT_EQ(range("x", "1"), kNotInteger);
    EXPECT_EQ(run({"LRANGE", "missing", "0", "-1"}), "*0\r\n");
}

TEST_F(CommandsTest, PopFromBothEnds) {
    run({"RPUSH", "l", "a", "b", "c", "d"});
    EXPECT_EQ(run({"LPOP", "l"}), "$1\r\na\r\n");
    EXPECT_EQ(run({"RPOP", "l"}), "$1\r\nd\r\n");
    EXPECT_EQ(run({"LRANGE", "l", "0", "-1"}), "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    EXPECT_EQ(run({"LPOP", "missing"}), kNil);
}

TEST_F(CommandsTest, PopWithCount) {
    run({"RPUSH", "l", "a", "b", "c", "d", "e"});
    EXPECT_EQ(run({"LPOP", "l", "2"}), "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    EXPECT_EQ(run({"RPOP", "l", "2"}), "*2\r\n$1\r\ne\r\n$1\r\nd\r\n");  // in pop order
    EXPECT_EQ(run({"RPOP", "l", "0"}), "*0\r\n");
    EXPECT_EQ(run({"LPOP", "l", "10"}), "*1\r\n$1\r\nc\r\n");  // count larger than the list
    EXPECT_EQ(run({"LPOP", "missing", "2"}), "*-1\r\n");        // nil array, not nil
    EXPECT_EQ(run({"LPOP", "l", "-1"}), "-ERR value is out of range, must be positive\r\n");
    EXPECT_EQ(run({"LPOP", "l", "x"}), "-ERR value is out of range, must be positive\r\n");
    EXPECT_EQ(run({"LPOP", "l", "1", "2"}), "-ERR wrong number of arguments for 'lpop' command\r\n");
}

TEST_F(CommandsTest, PoppingTheLastElementDeletesTheKey) {
    run({"RPUSH", "l", "a", "b"});
    run({"EXPIRE", "l", "100"});
    run({"LPOP", "l", "2"});
    EXPECT_EQ(run({"EXISTS", "l"}), ":0\r\n");
    EXPECT_EQ(run({"TYPE", "l"}), "+none\r\n");
    run({"RPUSH", "l", "new"});
    EXPECT_EQ(run({"TTL", "l"}), ":-1\r\n");  // a fresh key, without the old TTL
}

TEST_F(CommandsTest, ListsKeepTheirTtlWhenModified) {
    run({"RPUSH", "l", "a"});
    run({"EXPIRE", "l", "100"});
    run({"RPUSH", "l", "b"});
    run({"LPOP", "l"});
    EXPECT_EQ(run({"TTL", "l"}), ":100\r\n");
}

TEST_F(CommandsTest, ListCommandsCheckTheType) {
    run({"SET", "s", "v"});
    EXPECT_EQ(run({"LPUSH", "s", "a"}), kWrongType);
    EXPECT_EQ(run({"RPUSH", "s", "a"}), kWrongType);
    EXPECT_EQ(run({"LPOP", "s"}), kWrongType);
    EXPECT_EQ(run({"RPOP", "s", "2"}), kWrongType);
    EXPECT_EQ(run({"LRANGE", "s", "0", "-1"}), kWrongType);
    EXPECT_EQ(run({"LLEN", "s"}), kWrongType);
    EXPECT_EQ(run({"GET", "s"}), "$1\r\nv\r\n");  // untouched

    run({"RPUSH", "l", "a"});
    EXPECT_EQ(run({"GET", "l"}), kWrongType);
    EXPECT_EQ(run({"HSET", "l", "f", "v"}), kWrongType);
}

// ------------------------------------------------------------------ hashes

TEST_F(CommandsTest, HsetCountsNewFieldsOnly) {
    EXPECT_EQ(run({"HSET", "h", "f1", "v1"}), ":1\r\n");
    EXPECT_EQ(run({"HSET", "h", "f1", "changed", "f2", "v2"}), ":1\r\n");
    EXPECT_EQ(run({"HGET", "h", "f1"}), "$7\r\nchanged\r\n");
    EXPECT_EQ(run({"HLEN", "h"}), ":2\r\n");
    EXPECT_EQ(run({"HSET", "h", "f"}), "-ERR wrong number of arguments for 'hset' command\r\n");
    EXPECT_EQ(run({"HSET", "h", "f", "v", "g"}), "-ERR wrong number of arguments for 'hset' command\r\n");
}

TEST_F(CommandsTest, HashReads) {
    run({"HSET", "h", "f", "v"});
    EXPECT_EQ(run({"HGET", "h", "nope"}), kNil);
    EXPECT_EQ(run({"HGET", "missing", "f"}), kNil);
    EXPECT_EQ(run({"HEXISTS", "h", "f"}), ":1\r\n");
    EXPECT_EQ(run({"HEXISTS", "h", "nope"}), ":0\r\n");
    EXPECT_EQ(run({"HEXISTS", "missing", "f"}), ":0\r\n");
    EXPECT_EQ(run({"HLEN", "missing"}), ":0\r\n");
    EXPECT_EQ(run({"HGETALL", "h"}), "*2\r\n$1\r\nf\r\n$1\r\nv\r\n");
    EXPECT_EQ(run({"HGETALL", "missing"}), "*0\r\n");
}

TEST_F(CommandsTest, HgetallReturnsEveryPair) {
    run({"HSET", "h", "a", "1", "b", "2", "c", "3"});
    const std::string reply = run({"HGETALL", "h"});
    EXPECT_EQ(reply.substr(0, 4), "*6\r\n");
    // Field order is unspecified (as in Redis), so check each pair is present.
    for (const std::string pair : {"$1\r\na\r\n$1\r\n1\r\n", "$1\r\nb\r\n$1\r\n2\r\n", "$1\r\nc\r\n$1\r\n3\r\n"}) {
        EXPECT_NE(reply.find(pair), std::string::npos) << pair;
    }
}

TEST_F(CommandsTest, HdelRemovesFieldsAndFinallyTheKey) {
    run({"HSET", "h", "a", "1", "b", "2"});
    EXPECT_EQ(run({"HDEL", "h", "a", "nope", "a"}), ":1\r\n");
    EXPECT_EQ(run({"HDEL", "missing", "a"}), ":0\r\n");
    EXPECT_EQ(run({"HDEL", "h", "b"}), ":1\r\n");
    EXPECT_EQ(run({"EXISTS", "h"}), ":0\r\n");
}

TEST_F(CommandsTest, HashCommandsCheckTheType) {
    run({"SET", "s", "v"});
    EXPECT_EQ(run({"HSET", "s", "f", "v"}), kWrongType);
    EXPECT_EQ(run({"HGET", "s", "f"}), kWrongType);
    EXPECT_EQ(run({"HDEL", "s", "f"}), kWrongType);
    EXPECT_EQ(run({"HGETALL", "s"}), kWrongType);
    EXPECT_EQ(run({"HLEN", "s"}), kWrongType);
    EXPECT_EQ(run({"HEXISTS", "s", "f"}), kWrongType);
    run({"HSET", "h", "f", "v"});
    EXPECT_EQ(run({"LPUSH", "h", "x"}), kWrongType);
    EXPECT_EQ(run({"INCR", "h"}), kWrongType);
    EXPECT_EQ(run({"TYPE", "h"}), "+hash\r\n");
}

// ------------------------------------------------------------------ sorted sets

TEST_F(CommandsTest, ZaddAndZrange) {
    EXPECT_EQ(run({"ZADD", "board", "100", "alice", "250", "bob", "50", "carol"}), ":3\r\n");
    EXPECT_EQ(run({"ZRANGE", "board", "0", "-1"}), "*3\r\n$5\r\ncarol\r\n$5\r\nalice\r\n$3\r\nbob\r\n");
    EXPECT_EQ(run({"ZRANGE", "board", "0", "-1", "WITHSCORES"}),
              "*6\r\n$5\r\ncarol\r\n$2\r\n50\r\n$5\r\nalice\r\n$3\r\n100\r\n$3\r\nbob\r\n$3\r\n250\r\n");
    EXPECT_EQ(run({"ZRANGE", "board", "-2", "-1"}), "*2\r\n$5\r\nalice\r\n$3\r\nbob\r\n");
    EXPECT_EQ(run({"ZRANGE", "board", "5", "10"}), "*0\r\n");
    EXPECT_EQ(run({"ZRANGE", "missing", "0", "-1"}), "*0\r\n");
    EXPECT_EQ(run({"ZCARD", "board"}), ":3\r\n");
    EXPECT_EQ(run({"ZCARD", "missing"}), ":0\r\n");
    EXPECT_EQ(run({"TYPE", "board"}), "+zset\r\n");
}

TEST_F(CommandsTest, EqualScoresAreOrderedByMember) {
    run({"ZADD", "z", "1", "charlie", "1", "alpha", "1", "bravo"});
    EXPECT_EQ(run({"ZRANGE", "z", "0", "-1"}), "*3\r\n$5\r\nalpha\r\n$5\r\nbravo\r\n$7\r\ncharlie\r\n");
}

TEST_F(CommandsTest, ZaddUpdatesScoresAndCountsOnlyNewMembers) {
    run({"ZADD", "z", "1", "a", "2", "b"});
    EXPECT_EQ(run({"ZADD", "z", "3", "a", "4", "c"}), ":1\r\n");  // a moved, c added
    EXPECT_EQ(run({"ZRANGE", "z", "0", "-1"}), "*3\r\n$1\r\nb\r\n$1\r\na\r\n$1\r\nc\r\n");
    EXPECT_EQ(run({"ZADD", "z", "5", "d", "6", "d"}), ":1\r\n");  // repeated member: the last score wins
    EXPECT_EQ(run({"ZSCORE", "z", "d"}), "$1\r\n6\r\n");
}

TEST_F(CommandsTest, ZaddValidatesEverythingBeforeChangingAnything) {
    EXPECT_EQ(run({"ZADD", "z", "1", "a", "x", "b"}), "-ERR value is not a valid float\r\n");
    EXPECT_EQ(run({"ZADD", "z", "nan", "a"}), "-ERR value is not a valid float\r\n");
    EXPECT_EQ(run({"ZADD", "z", " 1", "a"}), "-ERR value is not a valid float\r\n");
    EXPECT_EQ(run({"ZADD", "z", "1e400", "a"}), "-ERR value is not a valid float\r\n");  // overflows a double
    EXPECT_EQ(run({"ZADD", "z", "1", "a", "2"}), kSyntax);
    EXPECT_EQ(run({"ZADD", "z", "1"}), "-ERR wrong number of arguments for 'zadd' command\r\n");
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(CommandsTest, ScoresParseAndFormatLikeRedis) {
    run({"ZADD", "z", "1.5", "a", "-inf", "b", "+inf", "c", "1e3", "d", "0.1", "e", "1e21", "f", "-0.000001", "g"});
    EXPECT_EQ(run({"ZSCORE", "z", "a"}), "$3\r\n1.5\r\n");
    EXPECT_EQ(run({"ZSCORE", "z", "b"}), "$4\r\n-inf\r\n");
    EXPECT_EQ(run({"ZSCORE", "z", "c"}), "$3\r\ninf\r\n");
    EXPECT_EQ(run({"ZSCORE", "z", "d"}), "$4\r\n1000\r\n");  // integral: no ".0"
    EXPECT_EQ(run({"ZSCORE", "z", "e"}), "$3\r\n0.1\r\n");   // shortest form, not 0.10000000000000001
    EXPECT_EQ(run({"ZSCORE", "z", "f"}), "$5\r\n1e+21\r\n");
    EXPECT_EQ(run({"ZSCORE", "z", "g"}), "$9\r\n-0.000001\r\n");
    EXPECT_EQ(run({"ZSCORE", "z", "missing"}), kNil);
    EXPECT_EQ(run({"ZSCORE", "nokey", "a"}), kNil);
}

TEST_F(CommandsTest, Zrank) {
    run({"ZADD", "z", "10", "a", "20", "b", "30", "c"});
    EXPECT_EQ(run({"ZRANK", "z", "a"}), ":0\r\n");
    EXPECT_EQ(run({"ZRANK", "z", "c"}), ":2\r\n");
    EXPECT_EQ(run({"ZRANK", "z", "nope"}), kNil);
    EXPECT_EQ(run({"ZRANK", "missing", "a"}), kNil);
    EXPECT_EQ(run({"ZRANK", "z", "b", "WITHSCORE"}), "*2\r\n:1\r\n$2\r\n20\r\n");
    EXPECT_EQ(run({"ZRANK", "z", "nope", "withscore"}), "*-1\r\n");
    EXPECT_EQ(run({"ZRANK", "z", "b", "bogus"}), kSyntax);
    EXPECT_EQ(run({"ZRANK", "z", "b", "WITHSCORE", "x"}), "-ERR wrong number of arguments for 'zrank' command\r\n");
}

TEST_F(CommandsTest, ZrangeErrors) {
    run({"ZADD", "z", "1", "a"});
    EXPECT_EQ(run({"ZRANGE", "z", "x", "1"}), kNotInteger);
    EXPECT_EQ(run({"ZRANGE", "z", "0", "1", "REV"}), kSyntax);  // only WITHSCORES is supported
    EXPECT_EQ(run({"ZRANGE", "z", "0"}), "-ERR wrong number of arguments for 'zrange' command\r\n");
}

TEST_F(CommandsTest, ZremAndDeletingTheLastMember) {
    run({"ZADD", "z", "1", "a", "2", "b"});
    EXPECT_EQ(run({"ZREM", "z", "a", "nope", "a"}), ":1\r\n");
    EXPECT_EQ(run({"ZREM", "missing", "a"}), ":0\r\n");
    EXPECT_EQ(run({"ZRANGE", "z", "0", "-1"}), "*1\r\n$1\r\nb\r\n");
    EXPECT_EQ(run({"ZREM", "z", "b"}), ":1\r\n");
    EXPECT_EQ(run({"EXISTS", "z"}), ":0\r\n");
}

TEST_F(CommandsTest, SortedSetCommandsCheckTheType) {
    run({"SET", "s", "v"});
    EXPECT_EQ(run({"ZADD", "s", "1", "a"}), kWrongType);
    EXPECT_EQ(run({"ZREM", "s", "a"}), kWrongType);
    EXPECT_EQ(run({"ZSCORE", "s", "a"}), kWrongType);
    EXPECT_EQ(run({"ZRANK", "s", "a"}), kWrongType);
    EXPECT_EQ(run({"ZRANGE", "s", "0", "-1"}), kWrongType);
    EXPECT_EQ(run({"ZCARD", "s"}), kWrongType);
    run({"ZADD", "z", "1", "a"});
    EXPECT_EQ(run({"GET", "z"}), kWrongType);
    EXPECT_EQ(run({"LPUSH", "z", "x"}), kWrongType);
}
