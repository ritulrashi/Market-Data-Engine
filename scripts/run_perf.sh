#!/usr/bin/env bash
# Profiles the server hot path under sustained load with perf.
#
# Uses a separate build (build-perf): same -O3 Release flags as the benchmark
# build plus -g -fno-omit-frame-pointer so call graphs resolve.
#
# NOTE: on the GitHub Codespace this was written for, the hypervisor exposes
# no hardware PMU (no `cpu` device under /sys/bus/event_source/devices), so
# cycles/instructions/cache-misses are "<not supported>". Only software
# events and timer-based (cpu-clock) sampling are available. perf also
# requires sudo there (kernel.perf_event_paranoid = 4).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-perf"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/bench/results/codespace/perf}"
WORKERS="${WORKERS:-2}"
CLIENTS="${CLIENTS:-4}"
mkdir -p "$OUT_DIR"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DMDE_BUILD_TESTS=OFF \
    -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" > /dev/null
cmake --build "$BUILD_DIR" -j > /dev/null

profile() {
    local mode="$1" rate="$2" port="$3"
    local tag="${mode}_c${CLIENTS}"
    "$BUILD_DIR/market_data_server" --port "$port" --workers "$WORKERS" --rate "$rate" \
        > "$OUT_DIR/${tag}_server.log" 2>&1 &
    local spid=$!
    sleep 1.5
    "$BUILD_DIR/market_data_client" --port "$port" --clients "$CLIENTS" --duration 30 \
        > "$OUT_DIR/${tag}_client.log" 2>&1 &
    local cpid=$!
    sleep 3 # let the load reach steady state

    sudo perf stat -e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions \
        --per-thread -p "$spid" -o "$OUT_DIR/${tag}_perf_stat.txt" -- sleep 10
    sudo perf record -F 999 -g -p "$spid" -o "$OUT_DIR/${tag}_perf.data" -- sleep 10 \
        2> "$OUT_DIR/${tag}_perf_record.log"
    sudo perf report -i "$OUT_DIR/${tag}_perf.data" --stdio --no-children --sort comm,dso,symbol \
        --percent-limit 0.5 -g none > "$OUT_DIR/${tag}_perf_report.txt" 2>/dev/null
    sudo perf report -i "$OUT_DIR/${tag}_perf.data" --stdio --no-children --sort symbol \
        --percent-limit 2 -g caller,0.5,callee --max-stack 12 > "$OUT_DIR/${tag}_perf_callgraph.txt" 2>/dev/null
    sudo rm -f "$OUT_DIR/${tag}_perf.data" "$OUT_DIR/${tag}_perf.data.old"

    wait "$cpid"
    kill -INT "$spid"
    wait "$spid" || true
    echo "$tag done" >&2
}

profile stress 0 9501
profile steady 200000 9502
