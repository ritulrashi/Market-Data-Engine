# Concurrent Market Data Distribution Engine

A C++20 market data distribution engine built around a lock-free,
single-producer/multi-consumer (SPMC) ring buffer for fanning out tick data
from a producer to multiple network clients with drop-oldest overflow
semantics.

## Layout

- `include/ring_buffer/spmc_ring_buffer.hpp` — lock-free SPMC ring buffer
  (sequence-tagged slots, Disruptor-style)
- `include/producer/`, `src/producer/` — tick producer
- `include/network/`, `src/network/` — distribution server
- `include/client/`, `src/client/` — tick client
- `include/protocol/tick.hpp` — wire protocol
- `tests/` — unit tests (ring buffer, protocol)
- `bench/` — benchmarks

## Build

```
cmake -B build
cmake --build build
```

Sanitizer builds:

```
cmake -B build-asan -DMDE_ENABLE_ASAN=ON
cmake -B build-tsan -DMDE_ENABLE_TSAN=ON
```

Run tests via CTest from a build directory:

```
ctest --test-dir build
```

## Wire format

Every message is exactly 24 bytes, sent back to back on the TCP stream with
no length prefix. All fields are little-endian; see `encode_tick` /
`decode_tick` in `include/protocol/tick.hpp`.

| Offset | Size | Field          | Type                                  |
|-------:|-----:|----------------|---------------------------------------|
| 0      | 4    | `symbol_id`    | `uint32`                              |
| 4      | 8    | `price`        | IEEE-754 binary64 (`double`)          |
| 12     | 4    | `size`         | `uint32`                              |
| 16     | 8    | `timestamp_ns` | `uint64`, ns since Unix epoch (producer wall clock) |
