#!/usr/bin/env bash
# End-to-end test: starts the real velokv binary on a kernel-chosen port, drives
# it with redis-cli and raw TCP (bash's /dev/tcp), checks the replies, and shuts
# it down. Usage: tests/integration.sh <path-to-velokv-binary>
set -euo pipefail

BIN=${1:?usage: integration.sh <path-to-velokv-binary>}
command -v redis-cli >/dev/null || { echo "FAIL: redis-cli not found (install redis-tools)" >&2; exit 1; }

WORK=$(mktemp -d)
LOG="$WORK/server.log"
SERVER_PID=""
PORT=""

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    echo "--- server log ---" >&2
    cat "$LOG" >&2 || true
    exit 1
}

pass() { echo "ok - $*"; }

# expect "<description>" "<actual>" "<expected>"
expect() {
    [[ "$2" == "$3" ]] || fail "$1: expected '$3', got '$2'"
    pass "$1"
}

cli() { redis-cli -p "$PORT" "$@"; }

# Starts the server with --port 0 and reads the real port from its log line, so
# there is no race between "find a free port" and "bind it".
start_server() {
    # Empty the log here, before the fork: otherwise this loop can read the
    # previous server's "listening on" line (and its dead port) before the
    # child's own 2>"$LOG" redirection gets round to truncating the file.
    : >"$LOG"
    "$BIN" --port 0 "$@" 2>>"$LOG" &
    SERVER_PID=$!
    for _ in $(seq 1 100); do
        PORT=$(sed -n 's/.*listening on .*:\([0-9][0-9]*\).*/\1/p' "$LOG")
        [[ -n "$PORT" ]] && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || fail "server exited during startup"
        sleep 0.1
    done
    fail "server did not report a listening port"
}

# SIGTERM must lead to a clean exit with status 0.
stop_server() {
    kill -TERM "$SERVER_PID"
    local status=0
    wait "$SERVER_PID" || status=$?
    SERVER_PID=""
    [[ $status -eq 0 ]] || fail "server exited with status $status after SIGTERM"
}

start_server

# ------------------------------------------------------------ M2: RESP via redis-cli
expect "redis-cli PING" "$(cli PING)" "PONG"
expect "redis-cli PING with message" "$(cli PING hello)" "hello"
expect "redis-cli ECHO" "$(cli ECHO hi)" "hi"
expect "wrong number of arguments" "$(cli ECHO)" "ERR wrong number of arguments for 'echo' command"
expect "unknown command" "$(cli FOO a b)" "ERR unknown command 'FOO', with args beginning with: 'a' 'b' "
expect "COMMAND DOCS stub (sent by redis-cli 7 on connect)" "$(cli COMMAND DOCS)" ""
expect "CONFIG GET stub (sent by redis-benchmark)" "$(cli CONFIG GET save)" ""

# An 8 MB binary argument and reply, much larger than the socket buffers.
head -c 8000000 /dev/urandom >"$WORK/big.in"
cli -x ECHO <"$WORK/big.in" >"$WORK/big.out"
head -c 8000000 "$WORK/big.out" | cmp -s - "$WORK/big.in" || fail "8 MB ECHO came back different"
pass "8 MB binary ECHO round-trips intact"

# ------------------------------------------------------------ M3: strings and keys
expect "SET" "$(cli SET greeting hello)" "OK"
expect "GET" "$(cli GET greeting)" "hello"
expect "GET of a missing key is nil" "$(cli GET nope)" ""
expect "SET NX on an existing key does nothing" "$(cli SET greeting x NX)" ""
expect "SET XX on an existing key" "$(cli SET greeting hi XX)" "OK"
expect "SET with an unknown option" "$(cli SET greeting x BOGUS)" "ERR syntax error"
expect "MSET" "$(cli MSET a 1 b 2)" "OK"
expect "MGET with a missing key" "$(cli MGET a nope b | tr '\n' ' ')" "1  2 "
expect "INCR creates the key" "$(cli INCR counter)" "1"
expect "INCRBY" "$(cli INCRBY counter 41)" "42"
expect "DECR" "$(cli DECR counter)" "41"
expect "INCR on a non-integer" "$(cli INCR greeting)" "ERR value is not an integer or out of range"
cli SET big 9223372036854775807 >/dev/null
expect "INCR overflow" "$(cli INCR big)" "ERR increment or decrement would overflow"
expect "value unchanged after overflow" "$(cli GET big)" "9223372036854775807"
expect "APPEND" "$(cli APPEND greeting ' world')" "8"
expect "GET after APPEND" "$(cli GET greeting)" "hi world"
expect "TYPE" "$(cli TYPE greeting)" "string"
expect "TYPE of a missing key" "$(cli TYPE nope)" "none"
expect "EXISTS counts repeats" "$(cli EXISTS a a nope)" "2"
expect "DEL" "$(cli DEL a b nope)" "2"
expect "DBSIZE" "$(cli DBSIZE)" "3"

head -c 1000000 /dev/urandom >"$WORK/value.bin"
cli -x SET binary <"$WORK/value.bin" >/dev/null
cli GET binary >"$WORK/value.out"
head -c 1000000 "$WORK/value.out" | cmp -s - "$WORK/value.bin" || fail "1 MB binary value came back different"
pass "1 MB binary value stored and read back intact"

expect "FLUSHALL" "$(cli FLUSHALL)" "OK"
expect "DBSIZE after FLUSHALL" "$(cli DBSIZE)" "0"

# ------------------------------------------------------------ M4: expiry
expect "SET with PX" "$(cli SET k v PX 200)" "OK"
expect "GET before the deadline" "$(cli GET k)" "v"
sleep 0.3
expect "GET 300 ms later is nil" "$(cli GET k)" ""

# Active expiry: 100 keys that nobody reads again must still disappear.
# (DBSIZE only counts keys; it never looks at them, so lazy expiry cannot help.)
for i in $(seq 1 100); do echo "SET temp:$i x PX 500"; done | cli >/dev/null
cli SET keeper v >/dev/null
expect "DBSIZE right after writing short-lived keys" "$(cli DBSIZE)" "101"
sleep 1
expect "DBSIZE drops without any reads (active expiry)" "$(cli DBSIZE)" "1"

expect "EXPIRE" "$(cli EXPIRE keeper 100)" "1"
expect "TTL" "$(cli TTL keeper)" "100"
expect "PERSIST" "$(cli PERSIST keeper)" "1"
expect "TTL after PERSIST" "$(cli TTL keeper)" "-1"
expect "TTL of a missing key" "$(cli TTL nope)" "-2"
cli SET keeper v EX 100 >/dev/null
cli SET keeper v2 >/dev/null
expect "SET without EX clears the TTL" "$(cli TTL keeper)" "-1"
expect "SET with an invalid expire time" "$(cli SET k v EX 0)" "ERR invalid expire time in 'set' command"
expect "EXPIRE with a negative TTL deletes the key" "$(cli EXPIRE keeper -1)" "1"
expect "key is gone" "$(cli EXISTS keeper)" "0"

# ------------------------------------------------------------ M6: lists and hashes
expect "LPUSH" "$(cli LPUSH jobs c b a)" "3"
expect "RPUSH" "$(cli RPUSH jobs d e)" "5"
expect "LRANGE whole list" "$(cli LRANGE jobs 0 -1 | tr '\n' ' ')" "a b c d e "
expect "LRANGE negative indices" "$(cli LRANGE jobs -2 -1 | tr '\n' ' ')" "d e "
expect "LRANGE out of range" "$(cli LRANGE jobs 10 20)" ""
expect "LPOP" "$(cli LPOP jobs)" "a"
expect "RPOP with count" "$(cli RPOP jobs 2 | tr '\n' ' ')" "e d "
expect "LLEN" "$(cli LLEN jobs)" "2"
cli LPOP jobs 2 >/dev/null
expect "popping the last element deletes the key" "$(cli EXISTS jobs)" "0"

expect "HSET counts new fields" "$(cli HSET user:1 name alice age 30)" "2"
expect "HSET overwrite" "$(cli HSET user:1 age 31)" "0"
expect "HGET" "$(cli HGET user:1 age)" "31"
expect "HEXISTS" "$(cli HEXISTS user:1 name)" "1"
expect "HLEN" "$(cli HLEN user:1)" "2"
expect "HGETALL" "$(cli HGETALL user:1 | paste - - | sort | tr '\t\n' '= ')" "age=31 name=alice "
expect "HDEL" "$(cli HDEL user:1 age nope)" "1"
cli HDEL user:1 name >/dev/null
expect "deleting the last field deletes the key" "$(cli EXISTS user:1)" "0"

cli SET plain str >/dev/null
cli RPUSH alist x >/dev/null
expect "WRONGTYPE: list command on a string" "$(cli LPUSH plain a)" "WRONGTYPE Operation against a key holding the wrong kind of value"
expect "WRONGTYPE: string command on a list" "$(cli GET alist)" "WRONGTYPE Operation against a key holding the wrong kind of value"
expect "WRONGTYPE: hash command on a list" "$(cli HSET alist f v)" "WRONGTYPE Operation against a key holding the wrong kind of value"
expect "TYPE list" "$(cli TYPE alist)" "list"
cli DEL plain alist >/dev/null

# ------------------------------------------------------------ M7: sorted sets
expect "ZADD" "$(cli ZADD board 100 alice 250 bob 50 carol)" "3"
expect "ZRANGE" "$(cli ZRANGE board 0 -1 | tr '\n' ' ')" "carol alice bob "
expect "ZRANGE WITHSCORES" "$(cli ZRANGE board 0 -1 WITHSCORES | tr '\n' ' ')" "carol 50 alice 100 bob 250 "
expect "ZRANGE negative indices" "$(cli ZRANGE board -2 -1 | tr '\n' ' ')" "alice bob "
expect "ZRANK" "$(cli ZRANK board bob)" "2"
expect "ZSCORE" "$(cli ZSCORE board alice)" "100"
expect "ZADD updates a score" "$(cli ZADD board 300 alice)" "0"
expect "rank after the update" "$(cli ZRANK board alice)" "2"
expect "ties ordered by member" "$(cli ZADD ties 1 c 1 a 1 b >/dev/null; cli ZRANGE ties 0 -1 | tr '\n' ' ')" "a b c "
expect "ZCARD" "$(cli ZCARD board)" "3"
expect "ZREM" "$(cli ZREM board carol nope)" "1"
expect "ZADD with a bad score" "$(cli ZADD board abc dave)" "ERR value is not a valid float"
expect "WRONGTYPE: string command on a sorted set" "$(cli GET board)" "WRONGTYPE Operation against a key holding the wrong kind of value"
expect "TYPE zset" "$(cli TYPE board)" "zset"
cli ZREM ties a b c >/dev/null
expect "removing the last member deletes the key" "$(cli EXISTS ties)" "0"
cli DEL board >/dev/null

# ------------------------------------------------------------ raw TCP
# Two clients connected at the same time, using inline commands as nc would.
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
exec 4<>"/dev/tcp/127.0.0.1/$PORT"
printf 'ECHO from-A\r\n' >&3
printf 'PING\r\n' >&4
IFS= read -r -t 3 reply_b <&4 || fail "no reply on the second connection"
IFS= read -r -t 3 header_a <&3 || fail "no reply on the first connection"
IFS= read -r -t 3 reply_a <&3 || fail "no bulk body on the first connection"
expect "second of two concurrent clients" "$reply_b" $'+PONG\r'
expect "first of two concurrent clients" "$header_a$reply_a" $'$6\rfrom-A\r'
exec 3>&- 4>&-

# Several commands, inline and RESP, in a single write: all replies, in order.
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'PING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\nPING hello\r\n*1\r\n$4\r\nPING\r\n' >&3
expected=$'+PONG\r\n$2\r\nhi\r\n$5\r\nhello\r\n+PONG\r\n'
actual=$(timeout 3 head -c ${#expected} <&3 | od -An -c | tr -s ' ')
exec 3>&-
expect "pipelined commands in one write" "$actual" "$(printf '%s' "$expected" | od -An -c | tr -s ' ')"

# Malformed RESP: an error reply, then the server hangs up.
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf '*1\r\n:5\r\nPING\r\n' >&3
IFS= read -r -t 3 reply <&3 || fail "no reply to malformed input"
expect "protocol error reply" "$reply" $'-ERR Protocol error: expected \'$\', got \':\'\r'
# EOF or a reset both count as closed; a timeout or more data does not.
if IFS= read -r -t 3 extra <&3 2>/dev/null; then fail "connection still open after protocol error (got '$extra')"; fi
pass "connection closed after protocol error"
exec 3>&-

# A client that asks for a big reply and disconnects without reading makes the
# server's send() fail; that must not kill the server (SIGPIPE is ignored).
{ printf '*2\r\n$4\r\nECHO\r\n$4000000\r\n'; head -c 4000000 /dev/zero; printf '\r\n'; } >"$WORK/big.req"
cat "$WORK/big.req" >"/dev/tcp/127.0.0.1/$PORT"
sleep 0.2
kill -0 "$SERVER_PID" 2>/dev/null || fail "server died after a client disconnected abruptly"
expect "server survives a client that disconnects mid-reply" "$(cli PING)" "PONG"

stop_server
pass "SIGTERM shuts the server down cleanly"

# ------------------------------------------------------------ M5: AOF persistence
AOF="$WORK/velokv.aof"

# kill -9: no signal handler runs, no final flush or fsync; like a crash.
crash_server() {
    kill -9 "$SERVER_PID"
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

start_server --aof "$AOF" --fsync everysec
cli MSET a 1 b 2 >/dev/null
cli INCRBY a 41 >/dev/null
cli SET gone v >/dev/null
cli DEL gone >/dev/null
cli SET session abc PX 5000 >/dev/null
cli SET short v PX 300 >/dev/null
cli -x SET binary <"$WORK/value.bin" >/dev/null
sleep 1
crash_server
sleep 0.5  # "short" expires while the server is down

start_server --aof "$AOF" --fsync everysec
# 7 writes, plus the DEL that active expiry logged when "short" expired.
grep -q "loaded 8 commands" "$LOG" || fail "unexpected load message: $(cat "$LOG")"
pass "AOF replayed on startup"
expect "data is back after kill -9" "$(cli MGET a b gone | tr '\n' ' ')" "42 2  "
pttl=$(cli PTTL session)
(( pttl > 0 && pttl <= 4000 )) || fail "PTTL after restart is $pttl; it must keep counting down from 5000, not restart"
pass "TTL survives the restart without being extended ($pttl ms left)"
expect "key that expired while the server was down" "$(cli GET short)" ""
cli GET binary >"$WORK/value.out"
head -c 1000000 "$WORK/value.out" | cmp -s - "$WORK/value.bin" || fail "binary value changed across restart"
pass "1 MB binary value survives a restart"
expect "writes continue after recovery" "$(cli INCR a)" "43"

# A crash in the middle of an append leaves half a command at the end.
crash_server
good_size=$(stat -c %s "$AOF")
printf '*3\r\n$3\r\nSET\r\n$4\r\ntorn\r\n$5\r\nval' >>"$AOF"
start_server --aof "$AOF"
grep -q "incomplete command (30 bytes); truncating to $good_size bytes" "$LOG" \
    || fail "no truncation warning in: $(cat "$LOG")"
pass "truncated AOF tail detected and cut back to the last complete command"
expect "data intact after a truncated tail" "$(cli GET a)" "43"
expect "the half-written command was dropped" "$(cli EXISTS torn)" "0"
cli SET after-repair yes >/dev/null
stop_server
start_server --aof "$AOF"
expect "writes after the repair survive another restart" "$(cli GET after-repair)" "yes"
stop_server

# Corruption in the middle is not something to guess about: refuse to start.
printf 'garbage\r\n' >"$WORK/corrupt.aof"
status=0
timeout 5 "$BIN" --port 0 --aof "$WORK/corrupt.aof" 2>"$WORK/corrupt.log" || status=$?
[[ $status -eq 1 ]] || fail "expected exit status 1 for a corrupt AOF, got $status"
grep -q "is corrupt at byte 0" "$WORK/corrupt.log" || fail "unexpected message: $(cat "$WORK/corrupt.log")"
pass "a corrupt AOF is refused with a clear error"

start_server --aof "$WORK/always.aof" --fsync always
cli SET durable yes >/dev/null
crash_server
start_server --aof "$WORK/always.aof" --fsync always
expect "fsync always: data is back after kill -9" "$(cli GET durable)" "yes"
stop_server

status=0
"$BIN" --fsync sometimes 2>/dev/null || status=$?
expect "an invalid --fsync policy is rejected" "$status" "2"

# Lists and hashes are persisted the same way.
start_server --aof "$WORK/types.aof"
cli RPUSH queue j1 j2 j3 >/dev/null
cli LPOP queue >/dev/null
cli HSET user:7 name bob role admin >/dev/null
cli HDEL user:7 role >/dev/null
crash_server
start_server --aof "$WORK/types.aof"
expect "list survives kill -9" "$(cli LRANGE queue 0 -1 | tr '\n' ' ')" "j2 j3 "
expect "hash survives kill -9" "$(cli HGETALL user:7 | tr '\n' ' ')" "name bob "
cli ZADD scores 10 x 20 y 30 z >/dev/null
cli ZADD scores 5 z >/dev/null
crash_server
start_server --aof "$WORK/types.aof"
expect "sorted set survives kill -9" "$(cli ZRANGE scores 0 -1 WITHSCORES | tr '\n' ' ')" "z 5 x 10 y 20 "
stop_server

echo "all integration tests passed"
