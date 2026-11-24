// Unit tests for AOF persistence: what gets logged (effects, not raw commands),
// round trips through a real file, recovery from a truncated tail cut at every
// possible byte, refusal of corrupt files, and TTLs that survive a restart
// without being extended. A fake clock stands in for "the server was down".
#include "aof.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "commands.h"
#include "keyspace.h"

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& data) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << data;
}

std::string command(const Args& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return out;
}

class AofTest : public ::testing::Test {
protected:
    void SetUp() override {
        char dirTemplate[] = "/tmp/velokv-aof-test-XXXXXX";
        ASSERT_NE(::mkdtemp(dirTemplate), nullptr);
        dir = dirTemplate;
        path = dir + "/test.aof";
    }
    void TearDown() override {
        ::unlink(path.c_str());
        ::rmdir(dir.c_str());
    }

    // A "server" writing to the AOF: commands run against `db`, their effects
    // and any expirations go to the file, as in main.cpp and server.cpp.
    std::string run(Keyspace& db, Aof& aof, const Args& args) {
        std::string out;
        executeCommand(db, args, out, [&aof](const Args& change) { aof.append(change); });
        aof.flush();
        return out;
    }

    // Records what a command propagated, without any file.
    std::vector<Args> effectsOf(Keyspace& db, const Args& args) {
        std::vector<Args> effects;
        std::string out;
        executeCommand(db, args, out, [&effects](const Args& change) { effects.push_back(change); });
        return effects;
    }

    static std::string get(Keyspace& db, const std::string& key) {
        std::string out;
        executeCommand(db, {"GET", key}, out);
        return out;
    }

    std::string dir;
    std::string path;
    std::int64_t now = 1'700'000'000'000;  // fake Unix ms clock
    Keyspace db{[this] { return now; }};
};

}  // namespace

// ------------------------------------------------------------------ what is logged

TEST_F(AofTest, ParsesFsyncPolicies) {
    EXPECT_EQ(parseFsyncPolicy("always"), FsyncPolicy::Always);
    EXPECT_EQ(parseFsyncPolicy("everysec"), FsyncPolicy::EverySec);
    EXPECT_EQ(parseFsyncPolicy("no"), FsyncPolicy::No);
    EXPECT_FALSE(parseFsyncPolicy("sometimes"));
}

TEST_F(AofTest, DeterministicWritesAreLoggedAsIs) {
    EXPECT_EQ(effectsOf(db, {"MSET", "a", "1", "b", "2"}), (std::vector<Args>{{"MSET", "a", "1", "b", "2"}}));
    EXPECT_EQ(effectsOf(db, {"INCRBY", "a", "5"}), (std::vector<Args>{{"INCRBY", "a", "5"}}));
    EXPECT_EQ(effectsOf(db, {"APPEND", "b", "x"}), (std::vector<Args>{{"APPEND", "b", "x"}}));
    EXPECT_EQ(effectsOf(db, {"DEL", "a", "missing"}), (std::vector<Args>{{"DEL", "a", "missing"}}));
    EXPECT_EQ(effectsOf(db, {"flushall"}), (std::vector<Args>{{"FLUSHALL"}}));
}

TEST_F(AofTest, SetDropsConditionsThatAlreadyHeld) {
    EXPECT_EQ(effectsOf(db, {"set", "k", "v", "NX"}), (std::vector<Args>{{"SET", "k", "v"}}));
}

TEST_F(AofTest, RelativeTtlsAreLoggedAsAbsoluteDeadlines) {
    EXPECT_EQ(effectsOf(db, {"SET", "k", "v", "EX", "10"}),
              (std::vector<Args>{{"SET", "k", "v", "PXAT", std::to_string(now + 10000)}}));
    EXPECT_EQ(effectsOf(db, {"PEXPIRE", "k", "500"}),
              (std::vector<Args>{{"PEXPIREAT", "k", std::to_string(now + 500)}}));
    EXPECT_EQ(effectsOf(db, {"EXPIRE", "k", "-1"}), (std::vector<Args>{{"DEL", "k"}}));  // deleted on the spot
}

TEST_F(AofTest, WritesThatChangeNothingAreNotLogged) {
    effectsOf(db, {"SET", "k", "v"});
    EXPECT_TRUE(effectsOf(db, {"SET", "k", "x", "NX"}).empty());      // condition failed
    EXPECT_TRUE(effectsOf(db, {"DEL", "missing"}).empty());           // nothing removed
    EXPECT_TRUE(effectsOf(db, {"EXPIRE", "missing", "10"}).empty());  // no such key
    EXPECT_TRUE(effectsOf(db, {"PERSIST", "k"}).empty());             // had no TTL
    EXPECT_TRUE(effectsOf(db, {"INCR", "k"}).empty());                // error: not an integer
    EXPECT_TRUE(effectsOf(db, {"GET", "k"}).empty());                 // reads never log
}

// ------------------------------------------------------------------ file round trip

TEST_F(AofTest, FlushWritesRespCommandsToTheFile) {
    Aof aof(path, FsyncPolicy::No);
    run(db, aof, {"SET", "k", "v"});
    EXPECT_EQ(readFile(path), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
}

TEST_F(AofTest, ReplayRebuildsTheKeyspace) {
    {
        Aof aof(path, FsyncPolicy::Always);
        run(db, aof, {"MSET", "a", "1", "b", "2", "c", "3"});
        run(db, aof, {"INCRBY", "a", "41"});
        run(db, aof, {"APPEND", "b", "xyz"});
        run(db, aof, {"DEL", "c"});
        run(db, aof, {"SET", "bin", std::string("\0\r\n", 3)});
    }
    Keyspace restored([this] { return now; });
    const AofLoadResult result = loadAof(path, restored);
    EXPECT_EQ(result.commands, 5u);
    EXPECT_EQ(result.truncatedBytes, 0u);
    EXPECT_EQ(restored.size(), 3u);
    EXPECT_EQ(get(restored, "a"), "$2\r\n42\r\n");
    EXPECT_EQ(get(restored, "b"), "$4\r\n2xyz\r\n");
    EXPECT_EQ(get(restored, "c"), "$-1\r\n");
    EXPECT_EQ(get(restored, "bin"), std::string("$3\r\n\0\r\n\r\n", 9));
}

TEST_F(AofTest, MissingFileLoadsNothing) {
    const AofLoadResult result = loadAof(path, db);
    EXPECT_EQ(result.commands, 0u);
    EXPECT_EQ(db.size(), 0u);
}

TEST_F(AofTest, NewWritesAreAppendedAfterExistingOnes) {
    { Aof aof(path, FsyncPolicy::No); run(db, aof, {"SET", "a", "1"}); }
    { Aof aof(path, FsyncPolicy::No); run(db, aof, {"SET", "b", "2"}); }
    Keyspace restored;
    EXPECT_EQ(loadAof(path, restored).commands, 2u);
    EXPECT_EQ(restored.size(), 2u);
}

// ------------------------------------------------------------------ crash recovery

TEST_F(AofTest, TruncatedLastCommandIsDroppedAtEveryPossibleCutPoint) {
    const std::string intact = command({"SET", "a", "1"}) + command({"SET", "b", "2"});
    const std::string last = command({"SET", "c", "a longer value"});
    for (std::size_t cut = 1; cut < last.size(); ++cut) {
        writeFile(path, intact + last.substr(0, cut));
        Keyspace restored;
        const AofLoadResult result = loadAof(path, restored);
        EXPECT_EQ(result.commands, 2u) << "cut after " << cut << " bytes";
        EXPECT_EQ(result.truncatedBytes, cut);
        EXPECT_EQ(restored.size(), 2u);
        EXPECT_EQ(readFile(path), intact) << "file not cut back to the last complete command";
    }
}

TEST_F(AofTest, WritingContinuesCleanlyAfterATruncatedTail) {
    writeFile(path, command({"SET", "a", "1"}) + "*3\r\n$3\r\nSET\r\n$1\r\nb");
    loadAof(path, db);
    {
        Aof aof(path, FsyncPolicy::No);
        run(db, aof, {"SET", "c", "3"});
    }
    Keyspace restored;
    EXPECT_EQ(loadAof(path, restored).commands, 2u);  // a and c; the fragment is gone
    EXPECT_EQ(get(restored, "c"), "$1\r\n3\r\n");
}

TEST_F(AofTest, CorruptionBeforeTheEndRefusesToLoad) {
    const std::string good = command({"SET", "a", "1"});
    for (const std::string& bad : {
             good + "garbage\r\n" + good,                       // not a RESP array
             good + "*1\r\n:5\r\n",                             // malformed RESP
             good + std::string(16, '\0'),                       // zero-filled tail after a power cut
             good + command({"NOSUCHCOMMAND"}),                 // fails on replay
             command({"SET", "s", "v"}) + command({"INCR", "s"}),  // fails on replay
         }) {
        writeFile(path, bad);
        Keyspace restored;
        EXPECT_THROW(loadAof(path, restored), std::runtime_error);
    }
}

// ------------------------------------------------------------------ expiry across restarts

TEST_F(AofTest, TtlIsNotExtendedByARestart) {
    {
        Aof aof(path, FsyncPolicy::No);
        run(db, aof, {"SET", "session", "abc", "EX", "10"});
        run(db, aof, {"SET", "other", "x"});
        run(db, aof, {"PEXPIRE", "other", "8000"});
    }
    now += 4000;  // the server was down for 4 seconds
    Keyspace restored([this] { return now; });
    loadAof(path, restored);
    std::string out;
    executeCommand(restored, {"TTL", "session"}, out);
    executeCommand(restored, {"PTTL", "other"}, out);
    EXPECT_EQ(out, ":6\r\n:4000\r\n");
}

TEST_F(AofTest, KeyThatExpiredWhileDownIsGoneAfterLoading) {
    {
        Aof aof(path, FsyncPolicy::No);
        run(db, aof, {"SET", "short", "v", "PX", "100"});
    }
    now += 10'000;
    Keyspace restored([this] { return now; });
    loadAof(path, restored);
    EXPECT_EQ(get(restored, "short"), "$-1\r\n");
}

TEST_F(AofTest, ExpiredKeysAreLoggedAsDelSoReplayDoesNotDependOnWhenItRuns) {
    const auto start = now;
    {
        Aof aof(path, FsyncPolicy::No);
        db.setExpiredHook([&aof](const std::string& key) { aof.append({"DEL", key}); });
        run(db, aof, {"SET", "n", "5", "PX", "100"});
        now += 200;
        // n has expired, so INCR starts again from 0. The lazy expiry inside this
        // command must reach the log (as DEL n) before the INCR itself.
        EXPECT_EQ(run(db, aof, {"INCR", "n"}), ":1\r\n");
    }
    // Replaying before or after the original expiry time gives the same result.
    for (const std::int64_t replayAt : {start + 50, start + 200, start + 100'000}) {
        now = replayAt;
        Keyspace restored([this] { return now; });
        loadAof(path, restored);
        EXPECT_EQ(get(restored, "n"), "$1\r\n1\r\n") << "replayed at +" << (replayAt - start) << " ms";
    }
}

TEST_F(AofTest, ActiveExpiryIsLoggedToo) {
    Aof aof(path, FsyncPolicy::No);
    db.setExpiredHook([&aof](const std::string& key) { aof.append({"DEL", key}); });
    run(db, aof, {"SET", "k", "v", "PX", "10"});
    now += 20;
    EXPECT_EQ(db.expireDue(100), 1u);
    aof.flush();
    EXPECT_EQ(readFile(path), command({"SET", "k", "v", "PXAT", std::to_string(now - 10)}) + command({"DEL", "k"}));
}

TEST_F(AofTest, ListAndHashWritesAreLoggedOnlyWhenTheyChangeSomething) {
    EXPECT_EQ(effectsOf(db, {"RPUSH", "l", "a", "b"}), (std::vector<Args>{{"RPUSH", "l", "a", "b"}}));
    EXPECT_EQ(effectsOf(db, {"LPOP", "l"}), (std::vector<Args>{{"LPOP", "l"}}));
    EXPECT_TRUE(effectsOf(db, {"LPOP", "missing"}).empty());
    EXPECT_TRUE(effectsOf(db, {"LPOP", "l", "0"}).empty());
    EXPECT_EQ(effectsOf(db, {"HSET", "h", "f", "v"}), (std::vector<Args>{{"HSET", "h", "f", "v"}}));
    EXPECT_TRUE(effectsOf(db, {"HDEL", "h", "nope"}).empty());
    EXPECT_EQ(effectsOf(db, {"HDEL", "h", "f"}), (std::vector<Args>{{"HDEL", "h", "f"}}));
}

TEST_F(AofTest, ListsAndHashesSurviveARestart) {
    {
        Aof aof(path, FsyncPolicy::No);
        run(db, aof, {"RPUSH", "queue", "j1", "j2", "j3"});
        run(db, aof, {"LPOP", "queue"});
        run(db, aof, {"LPUSH", "queue", "urgent"});
        run(db, aof, {"HSET", "user:1", "name", "alice", "age", "30"});
        run(db, aof, {"HDEL", "user:1", "age"});
        run(db, aof, {"RPUSH", "drained", "x"});
        run(db, aof, {"RPOP", "drained"});
    }
    Keyspace restored([this] { return now; });
    loadAof(path, restored);
    std::string out;
    executeCommand(restored, {"LRANGE", "queue", "0", "-1"}, out);
    executeCommand(restored, {"HGETALL", "user:1"}, out);
    executeCommand(restored, {"EXISTS", "drained"}, out);
    EXPECT_EQ(out, "*3\r\n$6\r\nurgent\r\n$2\r\nj2\r\n$2\r\nj3\r\n"
                   "*2\r\n$4\r\nname\r\n$5\r\nalice\r\n"
                   ":0\r\n");
}

TEST_F(AofTest, SortedSetsSurviveARestart) {
    {
        Aof aof(path, FsyncPolicy::No);
        run(db, aof, {"ZADD", "board", "100", "alice", "250", "bob", "50", "carol"});
        run(db, aof, {"ZADD", "board", "300", "alice"});
        run(db, aof, {"ZREM", "board", "carol"});
        EXPECT_TRUE(effectsOf(db, {"ZADD", "board", "250", "bob"}).empty());  // unchanged: not logged
    }
    Keyspace restored([this] { return now; });
    loadAof(path, restored);
    std::string out;
    executeCommand(restored, {"ZRANGE", "board", "0", "-1", "WITHSCORES"}, out);
    EXPECT_EQ(out, "*4\r\n$3\r\nbob\r\n$3\r\n250\r\n$5\r\nalice\r\n$3\r\n300\r\n");
}
