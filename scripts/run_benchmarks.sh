#!/usr/bin/env bash
# Runs the 1/4/12-consumer benchmark matrix, RUNS times per configuration,
# each run against a freshly started server (so counters start clean), in
# two producer modes:
#   steady - rate-limited producer (STEADY_RATE ticks/s), the sustained
#            workload the system is meant to carry without drops.
#   stress - unthrottled producer (as fast as it can generate ticks), a
#            deliberate overload test of drop-oldest-without-blocking.
# Raw client/server output for every run goes under $OUT_DIR; summarize with
# scripts/summarize_benchmarks.py.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/bench/results/codespace}"
PORT_BASE=9401
DURATION_S="${DURATION_S:-10}"
RUNS="${RUNS:-5}"
WORKERS="${WORKERS:-2}"
STEADY_RATE="${STEADY_RATE:-200000}"
SYMBOLS=10

mkdir -p "$OUT_DIR"

port=$PORT_BASE
run_trial() {
    local mode="$1" clients="$2" rate="$3" run="$4"
    local tag="${mode}_c${clients}_r${run}"
    port=$((port + 1))

    "$BUILD_DIR/market_data_server" --port "$port" --workers "$WORKERS" \
        --symbols "$SYMBOLS" --rate "$rate" > "$OUT_DIR/${tag}_server.log" 2>&1 &
    local server_pid=$!
    sleep 1.5

    "$BUILD_DIR/market_data_client" --host 127.0.0.1 --port "$port" \
        --clients "$clients" --duration "$DURATION_S" > "$OUT_DIR/${tag}_client.log" 2>&1

    kill -INT "$server_pid"
    wait "$server_pid" 2>/dev/null || true
    echo "${tag}: $(grep '^CLIENTS=' "$OUT_DIR/${tag}_client.log" | cut -c1-160)" >&2
}

# Interleave configurations across runs (run 1 of every config, then run 2,
# ...) so slow drift in the shared VM's performance spreads across all
# configurations instead of landing on one.
for run in $(seq 1 "$RUNS"); do
    for mode in steady stress; do
        rate=$STEADY_RATE
        [[ $mode == stress ]] && rate=0
        for clients in 1 4 12; do
            run_trial "$mode" "$clients" "$rate" "$run"
        done
    done
done

{
    echo "workers: $WORKERS"
    echo "steady rate: ${STEADY_RATE}/s"
    echo "duration per run: ${DURATION_S}s"
    echo "runs per configuration: $RUNS"
    echo "ring capacity: 65536"
} > "$OUT_DIR/run_config.txt"

echo "Done. Raw results in $OUT_DIR" >&2
