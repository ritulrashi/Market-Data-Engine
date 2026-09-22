#!/usr/bin/env bash
# Runs the 1/4/12-consumer benchmark matrix against a freshly started server
# for each trial (so per-consumer drop/sent counters start clean), in two
# producer modes:
#   steady - rate-limited producer (see STEADY_RATE below), the sustained
#            workload the system was designed to carry without drops.
#   stress - unthrottled producer (as fast as it can generate ticks), a
#            deliberate overload test of the drop-oldest-without-blocking
#            behavior under an unrealistic ingestion rate.
# Results (raw client/server output) are written under bench/results/.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OUT_DIR="$ROOT_DIR/bench/results"
PORT_BASE=9401
DURATION_S=20
WORKERS=4
STEADY_RATE=200000
SYMBOLS=10

mkdir -p "$OUT_DIR"

run_trial() {
    local mode="$1" clients="$2" rate="$3" port="$4"
    local tag="${mode}_c${clients}"
    echo "=== ${tag} (rate=${rate}, clients=${clients}, duration=${DURATION_S}s) ===" >&2

    "$BUILD_DIR/market_data_server" --port "$port" --workers "$WORKERS" \
        --symbols "$SYMBOLS" --rate "$rate" > "$OUT_DIR/${tag}_server.log" 2>&1 &
    local server_pid=$!
    sleep 1.5

    "$BUILD_DIR/market_data_client" --host 127.0.0.1 --port "$port" \
        --clients "$clients" --duration "$DURATION_S" > "$OUT_DIR/${tag}_client.log" 2>&1

    sleep 0.5
    kill -INT "$server_pid"
    wait "$server_pid" 2>/dev/null || true

    grep '^CLIENTS=' "$OUT_DIR/${tag}_client.log"
    grep '^FINAL' "$OUT_DIR/${tag}_server.log"
    echo >&2
}

port=$PORT_BASE
for clients in 1 4 12; do
    run_trial steady "$clients" "$STEADY_RATE" "$port"
    port=$((port + 1))
done
for clients in 1 4 12; do
    run_trial stress "$clients" 0 "$port"
    port=$((port + 1))
done

{
    echo "=== environment ==="
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "uname: $(uname -a)"
    echo "nproc: $(nproc)"
    echo "cpu model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
    echo "cpu cores (physical): $(lscpu | awk -F: '/^Core\(s\) per socket/{print $2}' | tr -d ' ') x $(lscpu | awk -F: '/^Socket\(s\)/{print $2}' | tr -d ' ') socket(s)"
    echo "threads per core: $(lscpu | awk -F: '/^Thread\(s\) per core/{print $2}' | tr -d ' ')"
    echo "compiler: $(c++ --version | head -1)"
    echo "cmake: $(cmake --version | head -1)"
    echo "ring capacity: 65536"
    echo "workers: $WORKERS"
    echo "steady rate: ${STEADY_RATE}/s"
    echo "duration per trial: ${DURATION_S}s"
} | tee "$OUT_DIR/environment.txt" >&2

echo "Done. Raw results in $OUT_DIR" >&2
