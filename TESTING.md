# Testing

All numbers below come from real runs in the GitHub Codespace described in
[BENCHMARKS.md](BENCHMARKS.md) (AMD EPYC 7763, 4 vCPUs, GCC 13.3.0).

## How to run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure        # everything
./build/unit_tests                                # ring buffer, protocol, producer
./build/integration_tests                         # real server + producer + TCP clients
```

GoogleTest v1.15.2 is fetched by CMake `FetchContent` (pinned by SHA-256).

## Test inventory and results

**30 tests, 30 passed, 0 failed.** Full suite also run 10 times back to back
in the Release build: 10/10 clean, no flaky failures.

| Binary | Suite | Tests | Result |
|---|---|---:|---|
| unit_tests | `RingBuffer` (single-threaded) | 8 | 8 pass |
| unit_tests | `RingBufferConcurrent` | 3 | 3 pass |
| unit_tests | `Protocol` | 8 | 8 pass |
| unit_tests | `Producer` | 4 | 4 pass |
| integration_tests | `Integration` | 7 | 7 pass |

### Ring buffer
- `EmptyRingHasNothingToRead`: regression test for bug 1 below.
- `SingleThreadedOrder`, `OrderPreservedAcrossManyWraparounds` (1000 items
  through a ring of 8), `LateConsumerSeesOnlyNewItems`.
- `DropOldestAfterSeveralLaps`: 24 items into a ring of 8 → first read returns
  item 16, `dropped == 16`, then 17..23 in order.
- `DropOldestDropsOnlyOverwrittenItems`: regression test for bug 2 below.
- `DropCountersArePerConsumer`: a fast and a slow consumer on one ring; only
  the slow one reports drops.
- `TickMessagePayloadRoundTrip`: the real 24-byte packed payload.
- `MultiConsumerOrderedAndFullyAccounted`: 8 consumer threads, ring of 1024,
  200,000 items. Per consumer: strictly increasing values and
  `received + dropped == 200000` (nothing silently vanishes).
- `NoTornReadsUnderHeavyLapping`: 6 consumers, ring of **16**, 300,000
  items of a 32-byte payload whose 4 words must all be equal. Any mix of
  words from two different publishes counts as corruption. Result: 0 torn
  reads, 0 order violations, full accounting.
- `NoDropsWhenRingNeverFills`: 4 consumers, 50,000 items into a 65,536 ring.
  Every consumer gets every item exactly once.

### Wire protocol
- Layout: size 24, offsets 0/4/12/16.
- Golden bytes: a known message encodes to an exact little-endian byte string.
- Round trips: typical values; **all zeros** (also checks the encoding is 24
  zero bytes); **max `symbol_id`, max `size`, max `timestamp_ns`**
  (`UINT32_MAX`, `UINT32_MAX`, `UINT64_MAX`); **extreme prices**: `DBL_MAX`,
  `-DBL_MAX` (lowest), `DBL_MIN`, `-DBL_MIN`, smallest denormal, epsilon,
  `+0.0`, `-0.0`, `±inf`, `0.01`; NaN keeps its exact bit pattern. Prices are
  compared bit for bit, not with `==`.
- Three back-to-back messages in one buffer decode independently.

### Producer
- Symbols strictly round-robin 0..9, size in [1, 1000], price > 0,
  timestamps non-decreasing (20,000 ticks).
- Same seed → identical price/size path.
- After 6,000 steps per symbol, prices are positive and within 50% of their
  mean. This is a sanity check against a broken or exploding walk. It does
  **not** statistically prove mean reversion.
- Throttled mode: 10,000/s for 0.5 s lands between 3,000 and 7,000 ticks.

### Integration (real server + producer thread + TCP clients)
The producer emits symbols 0,1,…,9,0,1,… so any gap, duplicate or reordering
on the wire breaks that pattern. Every client stream is checked for this
pattern, for non-decreasing timestamps, and for valid field ranges.
- `AllClientsReceiveCorrectOrderedTicks`: 3 clients, 20,000 ticks/s, 1.5 s:
  each gets > 10,000 ticks, 0 order breaks, 0 invalid, server `dropped == 0`.
- `ClientDisconnectMidStreamOthersKeepReceiving`: client A leaves after
  300 ms (normal FIN). The server drops to 2 active clients, and B and C each
  receive > 10,000 ticks stamped *after* A left, with 0 order breaks.
- `AbruptResetDisconnectIsHandled`: a raw client closes with RST
  (`SO_LINGER` 0). The server drops it and the other client is unaffected.
- `SlowClientIsDroppedOldestWithoutAffectingFastClient`: one client never
  reads, at 200,000 ticks/s. The server records drops (drop-oldest) and the fast
  client still gets a complete, ordered stream.
- `InboundClientBytesAreIgnored`: a client writing to the server is neither
  disconnected nor stalled.
- `ClientSeesServerShutdown`, `ConnectToClosedPortThrows`.

## Coverage

gcov + lcov 2.0, Debug build with `-DMDE_ENABLE_COVERAGE=ON` (adds
`--coverage -fprofile-update=atomic`; without the atomic flag, multithreaded
counter updates made geninfo abort with a negative count). Counted over
`include/` and `src/` only (not tests, GoogleTest or the STL):

| File | Lines |
|---|---|
| `include/ring_buffer/spmc_ring_buffer.hpp` | 45/45 (100.0%) |
| `include/protocol/tick.hpp` | 19/19 (100.0%) |
| `include/producer/tick_producer.hpp` | 3/3 (100.0%) |
| `include/network/server.hpp` | 1/1 (100.0%) |
| `src/producer/tick_producer.cpp` | 53/54 (98.1%) |
| `src/network/server.cpp` | 160/177 (90.4%) |
| `src/client/tick_client.cpp` | 48/54 (88.9%) |
| **Total** | **329/353 (93.2%)** |

`main_server.cpp` / `main_client.cpp` (CLI wrappers) are not covered by the
test binaries. They are exercised by the benchmark runs.

Uncovered lines are error paths: `socket()`/`epoll_create1()`/`bind()`
failures, `send()` returning a hard error (a peer reset is always caught
first by the EPOLLHUP/EPOLLRDHUP branch in these tests), a `recv()` error
other than EAGAIN, an invalid host string in the client, and the producer's
price floor clamp (never reached with the test seeds).

```bash
cmake -B build-cov -DCMAKE_BUILD_TYPE=Debug -DMDE_ENABLE_COVERAGE=ON
cmake --build build-cov -j && find build-cov -name '*.gcda' -delete
ctest --test-dir build-cov
lcov --capture --directory build-cov --output-file build-cov/cov.info \
     --ignore-errors mismatch,inconsistent,unused,gcov
lcov --extract build-cov/cov.info "$PWD/include/*" "$PWD/src/*" -o build-cov/cov_src.info
```

## Bugs found

Bugs found by the audit and tests (all fixed):

1. **Phantom first read (ring buffer).** Slots started with sequence 0 and a
   fresh consumer's cursor is also 0, so `try_read()` on a ring where nothing
   had been published returned `true` with an all-zero tick. *Fix:* slots
   carry 1-based stamps; 0 means "never written".
2. **Over-dropping on lap (ring buffer).** When lapped, the cursor jumped to
   whatever sequence sat in the wanted slot, discarding up to `Capacity - 1`
   ticks that were still readable. Publishing 9 items into a ring of 8 read
   back 1 item and reported 8 dropped (should be 8 read, 1 dropped). *Fix:*
   skip to the oldest item still in the ring (`head - Capacity`).
3. **Missing seqlock fences (ring buffer).** The relaxed payload loads and
   stores were ordered only by acquire/release on the stamp itself. That does
   not stop a relaxed payload load from moving after the stamp re-check, or a
   payload store from moving before the busy marker. It can't fail on x86-64
   (TSO) and no test can show it there, but the C++ memory model doesn't
   guarantee it. *Fix:* release fence after the busy store, acquire fence
   before the re-check.
4. **Queue defeated drop-oldest (server).** The per-client queue was
   capped at 8 MB (~350k ticks), so under overload ticks were delivered up to
   ~1 s late (old laptop stress runs) instead of being dropped. *Fix:* cap
   lowered to 64 KB (agreed with the project owner).
5. **Client harness was its own bottleneck.** One `recv()` syscall per 24-byte
   message and latency sampled for only 1 in 64 messages. *Fix:* 64 KB batched
   reads; latency recorded for every message; nearest-rank percentiles (each
   reported percentile is an actual observed sample).

Proof that the tests catch bugs 1 and 2: the new `test_ring_buffer.cpp` was
compiled against the original ring buffer (commit `1fb6dad`). 3 of 11 tests
failed: `EmptyRingHasNothingToRead`, `DropOldestDropsOnlyOverwrittenItems`
and `DropCountersArePerConsumer`. All pass against the fixed version.

Not a bug but worth knowing, found while writing
`SlowClientIsDroppedOldestWithoutAffectingFastClient`: the per-consumer drop
counter is updated **lazily**. A consumer only discovers it was lapped on its
next read. The server stops reading the ring for a client whose 64 KB queue
is full, so a fully stalled client shows `dropped = 0` until it starts
reading again. The first version of that test failed on exactly this.

## Correctness checks (sanitizers, Helgrind)

Each check uses its own build directory, separate from the Release
benchmark build:

```bash
cmake -B build-tsan  -DCMAKE_BUILD_TYPE=Debug -DMDE_ENABLE_TSAN=ON && cmake --build build-tsan -j
cmake -B build-asan  -DCMAKE_BUILD_TYPE=Debug -DMDE_ENABLE_ASAN=ON && cmake --build build-asan -j
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug                      && cmake --build build-debug -j   # for Helgrind
```

### Results

| Check | Workload | Result |
|---|---|---|
| TSan | `unit_tests` (23) | 23 pass, **0 TSan warnings** |
| TSan | `integration_tests` (7) | 7 pass, **0 TSan warnings** |
| TSan | TSan server (4 workers) + 12 TSan client connections, 200k ticks/s, 15 s | **0 warnings**, both exit 0 |
| TSan | same, unthrottled producer, 15 s | **0 warnings**, both exit 0 |
| ASan + LSan | `unit_tests`, `integration_tests` | 30 pass, **0 errors, 0 leaks** |
| ASan + LSan | ASan server + 12 ASan clients, 200k/s and unthrottled, 15 s each | **0 errors, 0 leaks** |
| Helgrind | `RingBufferConcurrent.*` (8, 6 and 4 consumer threads) | 3 pass, **0 errors** (14 hits from Valgrind's default glibc suppressions) |
| Helgrind | Debug server (4 workers), 12 clients, 20k/s, 20 s | **0 errors** (565,349 hits from default suppression `helgrind-glibc2X-005`; see below) |

Under TSan's slowdown the 12-client runs could not keep up (1.09M ticks/s
delivered vs 2.4M needed at 200k/s). The TSan server recorded 7,096,904 dropped
ticks in the steady run and 11,816,971 in the unthrottled run. That was useful:
it means the lapping / drop-oldest / torn-read-retry paths ran under TSan, not
just the happy path. The Helgrind server run also lapped (2,217,508 drops).

**About the Helgrind suppressions.** `helgrind-glibc2X-005` hides any race
whose top frame is anywhere in `libc.so.6`, which could in principle hide a
race on our own data inside a libc call. So the server run was repeated with
`--default-suppressions=no` (12 clients, 8 s). It produced 8 distinct
contexts. We checked the top frames of all of them: every one is inside glibc
itself (`pthread_mutex_lock` / `__pthread_mutex_unlock_usercnt` internals and
`__set_vma_name` during thread stack setup). These are Helgrind's known false
positives on glibc's own mutex implementation. None has a top frame in
engine code.

### Issues found in step 3 and how they were fixed

1. **TSan could not start at all** (`FATAL: ThreadSanitizer: unexpected
   memory mapping`), which also broke the build, because CMake's
   `gtest_discover_tests` runs the binary after linking. Cause: the kernel
   (6.8) uses 32 bits of mmap ASLR entropy, which GCC 13's TSan runtime does
   not support. *Fix (environment, not code):*
   `sudo sysctl -w vm.mmap_rnd_bits=28`. This does not persist across a
   codespace restart; rerun it before building `build-tsan`.
2. **No new data races, memory errors or leaks** were found in the engine by
   TSan, ASan/LSan or Helgrind in this session, so no code fixes were needed
   in step 3.

### Limits of these checks (stated plainly)
- GCC warns `'atomic_thread_fence' is not supported with '-fsanitize=thread'`.
  TSan does not model standalone fences, so **TSan cannot verify the seqlock
  fence fix (bug 3 above)**. That fix rests on the standard seqlock
  argument, not on a tool result.
- The ring buffer's payload words are `std::atomic<uint64_t>`, so no
  race detector can flag a torn payload read as a *data race*. Torn reads are
  instead checked functionally by `NoTornReadsUnderHeavyLapping`.
- Earlier TSan findings (a plain-copy data race and an undetected
  in-progress overwrite) are documented in the ring buffer header. They were
  found and fixed on the previous machine, before this repo's first commit.
  They could not be reproduced here because that history is not in git.
