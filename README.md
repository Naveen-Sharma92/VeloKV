# VeloKV

A Redis-compatible, in-memory key-value store written from scratch in **C++17**:
about 2,600 lines of code, built only on the C++ standard library and Linux system
calls.

VeloKV speaks the Redis wire protocol (RESP2), so `redis-cli`, `redis-benchmark`
and RESP2 client libraries work with it out of the box. A single-threaded,
non-blocking **epoll** event loop serves many clients at once, including
pipelined ones. VeloKV supports strings, lists, hashes and sorted sets, with key
expiry and crash-safe **append-only-file** persistence. Sorted sets run on a
hand-written **skip list** with O(log n) rank queries.

```
$ redis-cli -p 6380 ZADD board 100 alice 250 bob 50 carol
(integer) 3
$ redis-cli -p 6380 ZRANGE board 0 -1 WITHSCORES
1) "carol"
2) "50"
3) "alice"
4) "100"
5) "bob"
6) "250"
$ redis-cli -p 6380 ZRANK board bob
(integer) 2
```

## Features

- **Redis wire protocol (RESP2).** The incremental parser handles a command
  split across many TCP reads, many commands in one read (pipelining), and
  plain-text commands typed into `nc` or telnet. Error messages use Redis's
  exact wording.
- **Event loop.** One thread, non-blocking sockets, level-triggered `epoll`, no
  locks anywhere. A reply that does not fit in the socket buffer is finished
  later, when the socket becomes writable, so a slow client never stalls the
  others.
- **Data types.** Strings, lists, hashes and sorted sets, with Redis's
  `WRONGTYPE` errors, and a key is deleted when its last element is removed.
- **Key expiry.** `EX`/`PX` on `SET`, `EXPIRE`, `TTL`, `PERSIST`. Expired keys
  are deleted when a command touches them (lazy expiry). A min-heap of deadlines
  is also swept every 100 ms (active expiry), so keys nobody reads are freed too.
- **Persistence.** An append-only file (AOF) with a choice of fsync policy
  (`always`, `everysec`, `no`), replayed at startup.
  - It survives `kill -9`: every write is in the file before its reply is sent.
  - A half-written last entry is detected and repaired.
  - TTLs are stored as absolute timestamps, so a restart never extends them.
- **Sorted sets.** A skip list with a span on every link gives `ZRANK` and
  `ZRANGE` in O(log n), plus a hash map for O(1) `ZSCORE`.
- **Protection against bad clients.** Caps on argument size, line length,
  argument count and buffered input. Malformed input gets an error reply and a
  disconnect.
- **Tested and measured.** 138 GoogleTest unit tests plus an end-to-end test
  script with 105 checks, also run under AddressSanitizer + UBSan.
  Throughput is benchmarked against Redis with `redis-benchmark`.

## Quick start

Requires Linux (for `epoll`), a C++17 compiler and CMake 3.16+. On Windows or
macOS, use WSL or Docker.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/velokv --port 6380 --aof velokv.aof --fsync everysec
```

In another terminal:

```bash
redis-cli -p 6380 PING
redis-cli -p 6380 SET greeting hello EX 60
redis-cli -p 6380 GET greeting
redis-cli -p 6380 RPUSH jobs build test deploy
redis-cli -p 6380 HSET user:1 name alice role admin
redis-cli -p 6380 ZADD board 100 alice 250 bob
```

Command-line options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--port N` | `6380` | TCP port; `0` lets the kernel pick a free port |
| `--bind ADDR` | `127.0.0.1` | IPv4 address to listen on (`0.0.0.0` for all interfaces) |
| `--aof FILE` | none | Persist every change to this file and replay it at startup |
| `--fsync POLICY` | `everysec` | `always`, `everysec` or `no` |

### Docker

```bash
docker build -t velokv .
docker run --rm -p 6380:6380 velokv
docker run -d -p 6380:6380 -v velokv-data:/data velokv   # keep the AOF across containers
```

The image runs VeloKV as a non-root user, listening on `0.0.0.0:6380`, with
the AOF in `/data`. It shuts down cleanly on `docker stop`.

### Using a client library

Clients that speak RESP2 work unchanged. Some newer clients default to RESP3
(for example `redis-py` 8 sends `HELLO 3` on connect); select RESP2 for them:

```python
import redis
r = redis.Redis(port=6380, protocol=2, decode_responses=True)
r.set("greeting", "hello")
r.zadd("board", {"alice": 100, "bob": 250})
print(r.zrange("board", 0, -1, withscores=True))   # [('alice', 100.0), ('bob', 250.0)]
```

## Supported commands

41 commands:

| Group | Commands |
| --- | --- |
| Connection | `PING`, `ECHO`, `COMMAND` and `CONFIG GET` (stubs that reply with an empty array, which is what `redis-cli` and `redis-benchmark` need) |
| Keys | `DEL`, `EXISTS`, `TYPE`, `EXPIRE`, `PEXPIRE`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST`, `DBSIZE`, `FLUSHALL` |
| Strings | `GET`, `SET` (`NX`/`XX`, `EX`/`PX`/`EXAT`/`PXAT`), `MGET`, `MSET`, `INCR`, `DECR`, `INCRBY`, `APPEND` |
| Lists | `LPUSH`, `RPUSH`, `LPOP`, `RPOP` (with optional count), `LRANGE`, `LLEN` |
| Hashes | `HSET`, `HGET`, `HDEL`, `HGETALL`, `HLEN`, `HEXISTS` |
| Sorted sets | `ZADD`, `ZREM`, `ZSCORE`, `ZRANK` (with optional `WITHSCORE`), `ZRANGE` (with optional `WITHSCORES`), `ZCARD` |

Replies, error messages and edge cases follow Redis 7.2. During development,
about 400 hand-written edge cases and more than 7,000 randomly generated commands
were sent to both VeloKV and `redis-server` 7.2.7 and the replies compared byte
for byte. The only differences are listed under
[Limitations](#limitations-and-non-goals).

## Architecture

```
   redis-cli / client libraries / redis-benchmark
                    │  TCP (RESP2)
                    ▼
        ┌───────────────────────┐
        │   epoll event loop    │  accept · read · write · 100 ms tick
        └───────────┬───────────┘
                    │ per-connection input / output buffers
                    ▼
        ┌───────────────────────┐
        │  RESP parser/encoder  │  partial reads, pipelining, inline commands
        └───────────┬───────────┘
                    ▼
        ┌───────────────────────┐   effects   ┌──────────────────────┐
        │   command dispatcher  │────────────►│  AOF writer          │
        │   (table + arity)     │             │  fsync policy        │
        └───────────┬───────────┘             └──────────────────────┘
                    ▼
        ┌───────────────────────┐             ┌──────────────────────┐
        │  keyspace             │◄───────────►│  expiry              │
        │  key → variant<string,│             │  deadline map +      │
        │  list, hash, zset>    │             │  min-heap sweep      │
        └───────────────────────┘             └──────────────────────┘
```

**Life of a request.**

1. `epoll_wait` reports a readable socket, and the server does one `read()` of
   up to 16 KB into that client's input buffer.
2. The parser pulls out every complete command. A partial command stays in the
   buffer for the next read.
3. Each command is looked up, its argument count checked, and its handler run
   against the keyspace; the reply is appended to the client's output buffer.
4. Write commands report their effect to the AOF buffer, which is written to
   the file *before* any reply is sent.
5. The replies go out with `send()`. Anything the kernel cannot take yet stays
   buffered, and the socket is watched for writability until it is drained.

### Design decisions

- **Single-threaded, like Redis.** Commands run one at a time to completion, so
  the keyspace needs no locks and every command is atomic. For an in-memory
  store the network is the bottleneck, not the CPU.
- **Level-triggered epoll, one read per event.** Simpler to get right than
  edge-triggered, and a client sending a flood cannot starve the others.
- **A stateless, incremental parser.** Given the bytes so far, it returns either
  one complete command plus how many bytes it used, or "need more data", without
  consuming anything. Partial reads and pipelining fall out naturally, and every
  split point is unit-tested.
- **Typed values in a `std::variant`.** A type check is a tag comparison. Typed
  lookups throw `WrongTypeError`, and the dispatcher turns it into the
  `WRONGTYPE` reply in exactly one place.
- **Lazy + active expiry.** A lazy check on every lookup means a command never
  sees an expired key. The 100 ms sweep pops due deadlines from a min-heap, so
  keys nobody reads are freed too. When a key's TTL changes, its old heap entry
  is not searched for: it is skipped when popped, and the heap is rebuilt once
  such stale entries outnumber live ones.
- **The AOF records effects, not commands as typed.**
  - `SET k v EX 60` is stored as `SET k v PXAT <unix-ms>`, and `EXPIRE` as
    `PEXPIREAT`.
  - Expired keys are stored as `DEL`.
  - Expiry is paused while the file is replayed.

  So replaying gives the same state whenever it happens, and a restart can never
  extend a TTL.
- **A skip list for sorted sets.** It gives the same expected O(log n) as a
  balanced tree with far simpler code, and a span on each link makes rank
  queries O(log n). Plain `std::map` cannot do that. At a million members a
  `ZRANK` takes about 2.7 µs, against 62 ms for walking the list.
- **RAII everywhere.** Sockets and the epoll descriptor are owned by a small
  move-only `Fd` wrapper; skip-list nodes are owned through `std::unique_ptr`;
  there is no `new` or `delete` in the code.

## Benchmarks

The same machine for both servers, persistence off on both, the same
`redis-benchmark`, 50 clients, 100,000 requests. Each figure is the median of 5
rounds alternating between the two servers.

| Test | VeloKV (ops/sec) | Redis 7.2.7 (ops/sec) | VeloKV / Redis |
| --- | --- | --- | --- |
| SET | 154,560 | 170,358 | 0.91 |
| GET | 150,602 | 169,779 | 0.89 |
| SET, pipelined (P=16) | 2,777,778 | 2,040,816 | 1.36 |
| GET, pipelined (P=16) | 2,439,024 | 2,439,024 | 1.00 |

The same tests spread over a million random keys (`-r 1000000`):

| Test | VeloKV (ops/sec) | Redis 7.2.7 (ops/sec) | VeloKV / Redis |
| --- | --- | --- | --- |
| SET | 157,315 | 176,783 | 0.89 |
| GET | 153,846 | 174,419 | 0.88 |
| SET, pipelined (P=16) | 684,932 | 1,183,432 | 0.58 |
| GET, pipelined (P=16) | 814,332 | 1,418,440 | 0.57 |

Machine: 13th Gen Intel Core i5-13450HX, with 2 logical CPUs and 3.8 GiB of RAM
available to an Ubuntu 22.04 VM (kernel 6.8). Redis 7.2.7 was built from source
with its default allocator (jemalloc). [Raw results](bench/results/2026-11-24_122911.txt).

How to read these:

- **Without pipelining, VeloKV runs at about 90% of Redis.** Both servers spend
  most of their time on network round trips and system calls.
- **Pipelined on a single key** (`redis-benchmark`'s default), per-command CPU
  dominates, and VeloKV's shorter command path keeps up with Redis or passes it.
  These runs last only 40–50 ms, so treat them as ±20%.
- **Pipelined over a million random keys, Redis is clearly ahead.** Now memory
  layout and allocation dominate, and Redis's compact encodings and jemalloc
  win.

Reproduce with:

```bash
bash bench/run_benchmarks.sh build/velokv      # needs redis-server, redis-cli, redis-benchmark
```

## Testing

```bash
ctest --test-dir build --output-on-failure
```

- **Unit tests (GoogleTest, 138 tests):**
  - the RESP parser, including every split point of a command and pipelined
    input;
  - every command handler, with expected replies taken from real Redis;
  - expiry, on a fake clock;
  - the AOF, including a final command truncated at every possible byte;
  - the skip list, including a randomized stress test against `std::map` with
    an invariant checker that recomputes every span;
  - the event loop, including 16 MiB replies, half-closed connections and
    clients that disconnect mid-reply.
- **End-to-end test (`tests/integration.sh`, 105 checks):** starts the real
  binary on a free port and drives it with `redis-cli` and raw TCP. It includes
  `kill -9` crash recovery, a TTL that keeps counting down across a restart, a
  repaired half-written AOF, and a corrupt AOF that is refused.
- **Sanitizers:** configure with `-DVELOKV_SANITIZE=ON` to build everything with
  AddressSanitizer and UndefinedBehaviorSanitizer; `-DVELOKV_WERROR=ON` turns
  warnings into errors.
- **CI:** GitHub Actions builds and runs all tests on every push, once as an
  optimized build and once under the sanitizers.

## Project structure

```
VeloKV/
├── CMakeLists.txt
├── Dockerfile
├── src/
│   ├── main.cpp            # command-line options, signals, startup and shutdown
│   ├── fd.h                # RAII owner of a file descriptor
│   ├── server.{h,cpp}      # listening socket, epoll loop, connection lifecycle, tick
│   ├── connection.{h,cpp}  # per-client input/output buffers, non-blocking I/O
│   ├── resp.{h,cpp}        # incremental RESP2 parser and reply encoders
│   ├── commands.{h,cpp}    # command table and handlers
│   ├── keyspace.{h,cpp}    # key -> typed value, WRONGTYPE checks, lazy expiry
│   ├── expiry.{h,cpp}      # deadlines and the min-heap behind the active sweep
│   ├── skiplist.{h,cpp}    # skip list with spans, and the sorted set built on it
│   └── aof.{h,cpp}         # append-only file: writer, fsync policies, replay
├── tests/                  # GoogleTest unit tests and integration.sh
├── bench/                  # benchmark script and raw results
└── .github/workflows/ci.yml
```

## Limitations and non-goals

VeloKV is a learning project, not a Redis replacement. It does not have:

- **RESP3.** `HELLO` is not supported, so clients that default to RESP3 must be
  set to RESP2 (see above).
- **Most of Redis's command set**, or these options:
  - `ZRANGE`'s `BYSCORE`, `BYLEX`, `REV` and `LIMIT`;
  - `ZADD`'s `NX`, `XX`, `GT`, `LT`, `CH` and `INCR`;
  - `EXPIRE`'s `NX`, `XX`, `GT` and `LT`;
  - multiple databases, `KEYS` and `SCAN`, transactions, pub/sub, scripting.
- **AOF rewrite.** The file grows with every write.
- **Replication, clustering, authentication or TLS.** It listens on IPv4 only.

Other known differences from Redis:

- `HGETALL` returns fields in a different (equally unspecified) order.
- A score of `-0` reads back as `-0`; Redis prints `0` for small sorted sets.
- Memory use is higher: about twice Redis's in the million-key benchmark.

## License

MIT. See [LICENSE](LICENSE).
