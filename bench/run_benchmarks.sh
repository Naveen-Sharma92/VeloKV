#!/usr/bin/env bash
# Benchmarks VeloKV against a stock redis-server on the same machine. Part 1 uses
# the README's exact redis-benchmark commands (which hit a single key). Part 2
# spreads the same workload over a million random keys (-r), which also
# exercises hashing, allocation and CPU caches. Rounds alternate between the two
# servers so that background noise hits both equally; tables report the median
# round. Raw output, with the machine's CPU/RAM/OS, is saved under bench/results/.
#
# Usage: bench/run_benchmarks.sh [path-to-velokv] [rounds]
# Override the Redis tools with REDIS_SERVER, REDIS_CLI, REDIS_BENCHMARK.
set -euo pipefail

VELOKV=${1:-build/velokv}
ROUNDS=${2:-5}
REDIS_SERVER=${REDIS_SERVER:-redis-server}
REDIS_CLI=${REDIS_CLI:-redis-cli}
REDIS_BENCHMARK=${REDIS_BENCHMARK:-redis-benchmark}
VELOKV_PORT=6380
REDIS_PORT=6379

HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/results"
RAW="$HERE/results/$(date +%Y-%m-%d_%H%M%S).txt"
SAMPLES=$(mktemp)

# Persistence off on both: this compares request handling, not disks.
"$VELOKV" --port "$VELOKV_PORT" 2>/dev/null &
VELOKV_PID=$!
"$REDIS_SERVER" --port "$REDIS_PORT" --save "" --appendonly no >/dev/null &
REDIS_PID=$!
cleanup() {
    kill "$VELOKV_PID" "$REDIS_PID" 2>/dev/null || true
    wait "$VELOKV_PID" "$REDIS_PID" 2>/dev/null || true
    rm -f "$SAMPLES"
}
trap cleanup EXIT

for port in "$VELOKV_PORT" "$REDIS_PORT"; do
    for _ in $(seq 1 50); do
        [[ "$("$REDIS_CLI" -p "$port" PING 2>/dev/null)" == "PONG" ]] && break
        sleep 0.1
    done
done

{
    echo "# VeloKV vs Redis, $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "CPU:    $(lscpu | sed -n 's/^Model name: *//p'), $(nproc) logical CPUs available"
    echo "Memory: $(free -h | awk '/^Mem:/ {print $2}')"
    echo "OS:     $(. /etc/os-release && echo "$PRETTY_NAME"), kernel $(uname -r)"
    echo "Redis:  $("$REDIS_SERVER" --version)"
    echo "Rounds: $ROUNDS"
    echo
} >"$RAW"

# run <label> <port> [extra redis-benchmark flags...]
run() {
    local label=$1 port=$2
    shift 2
    local out
    out=$("$REDIS_BENCHMARK" -p "$port" -t set,get -n 100000 -c 50 "$@" -q 2>&1 | tr '\r' '\n' | grep 'requests per second')
    printf '%s\n%s\n' "== $label" "$out" >>"$RAW"
    # "SET: 123456.79 requests per second, p50=..." -> "label SET 123456.79"
    awk -v label="$label" '{ sub(":", "", $1); print label, $1, $2 }' <<<"$out" >>"$SAMPLES"
}

for round in $(seq 1 "$ROUNDS"); do
    echo "round $round/$ROUNDS" >&2
    run "velokv/P1" "$VELOKV_PORT"
    run "redis/P1" "$REDIS_PORT"
    run "velokv/P16" "$VELOKV_PORT" -P 16
    run "redis/P16" "$REDIS_PORT" -P 16
    # Part 2: start each run from an empty database, so both servers do the same work.
    for server in velokv redis; do
        port=$VELOKV_PORT
        [[ $server == redis ]] && port=$REDIS_PORT
        "$REDIS_CLI" -p "$port" FLUSHALL >/dev/null
        run "$server/R1" "$port" -r 1000000 -n 300000
        "$REDIS_CLI" -p "$port" FLUSHALL >/dev/null
        run "$server/R16" "$port" -r 1000000 -n 1000000 -P 16
    done
done

median() { # label test -> median requests/sec
    awk -v l="$1" -v t="$2" '$1 == l && $2 == t { print $3 }' "$SAMPLES" | sort -n |
        awk '{ v[NR] = $1 } END { print (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }'
}

table() { # title, then "label:row name" pairs
    echo
    echo "$1"
    echo
    echo "| Test                    | VeloKV (ops/sec) | Redis (ops/sec) | VeloKV / Redis |"
    echo "| ----------------------- | ---------------- | --------------- | -------------- |"
    shift
    for spec in "$@"; do
        local p=${spec%%:*} suffix=${spec#*:}
        for t in SET GET; do
            local v r
            v=$(median "velokv/$p" "$t")
            r=$(median "redis/$p" "$t")
            printf '| %-23s | %16.0f | %15.0f | %14s |\n' "$t, 50 clients$suffix" "$v" "$r" \
                "$(awk -v v="$v" -v r="$r" 'BEGIN { printf "%.2f", v / r }')"
        done
    done
}

{
    table "README commands (-n 100000, one key)" "P1:" "P16:, P=16"
    table "Random keys (-r 1000000; -n 300000, or 1000000 with -P 16)" "R1:" "R16:, P=16"
} | tee -a "$RAW"
echo "raw results: $RAW" >&2
