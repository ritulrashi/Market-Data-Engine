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
