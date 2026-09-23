# Benchmarks

Every number here was measured in this repo's GitHub Codespace on
2026-09-23. Each configuration was run **5 times** against a freshly started
server. Cells show **median (min – max)** over the 5 runs, so the run-to-run
noise of a shared cloud VM stays visible. Raw logs for every run are in
[`bench/results/codespace/`](bench/results/codespace/). The tables are generated from them
by [`scripts/summarize_benchmarks.py`](scripts/summarize_benchmarks.py).

## Hardware and toolchain

| | |
|---|---|
| CPU | AMD EPYC 7763 64-Core Processor (Azure VM, Microsoft hypervisor) |
| vCPUs | **4** = **2 physical cores × 2 SMT threads**, 1 socket, 1 NUMA node |
| Caches | L1d 64 KiB (2 instances), L2 1 MiB (2 instances), L3 32 MiB |
| Memory | 15 GiB |
| Kernel | Linux 6.8.0-1064-azure x86_64 |
| Compiler | g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| CMake | 3.28.3 |
| Build | `CMAKE_BUILD_TYPE=Release` → `-O3 -DNDEBUG` |
| TCP buffers | `tcp_wmem` 4096 16384 4194304, `tcp_rmem` 4096 131072 6291456 |

<details><summary>Full <code>lscpu</code> output</summary>

```
Architecture:                            x86_64
CPU op-mode(s):                          32-bit, 64-bit
Address sizes:                           48 bits physical, 48 bits virtual
Byte Order:                              Little Endian
CPU(s):                                  4
On-line CPU(s) list:                     0-3
Vendor ID:                               AuthenticAMD
Model name:                              AMD EPYC 7763 64-Core Processor
CPU family:                              25
Model:                                   1
Thread(s) per core:                      2
Core(s) per socket:                      2
Socket(s):                               1
Stepping:                                1
BogoMIPS:                                4890.86
Flags:                                   fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ht syscall nx mmxext fxsr_opt pdpe1gb rdtscp lm constant_tsc rep_good nopl tsc_reliable nonstop_tsc cpuid extd_apicid aperfmperf tsc_known_freq pni pclmulqdq ssse3 fma cx16 pcid sse4_1 sse4_2 movbe popcnt aes xsave avx f16c rdrand hypervisor lahf_lm cmp_legacy svm cr8_legacy abm sse4a misalignsse 3dnowprefetch osvw topoext vmmcall fsgsbase bmi1 avx2 smep bmi2 erms invpcid rdseed adx smap clflushopt clwb sha_ni xsaveopt xsavec xgetbv1 xsaves user_shstk clzero xsaveerptr rdpru arat npt nrip_save tsc_scale vmcb_clean flushbyasid decodeassists pausefilter pfthreshold v_vmsave_vmload umip vaes vpclmulqdq rdpid fsrm
Virtualization:                          AMD-V
Hypervisor vendor:                       Microsoft
Virtualization type:                     full
L1d cache:                               64 KiB (2 instances)
L1i cache:                               64 KiB (2 instances)
L2 cache:                                1 MiB (2 instances)
L3 cache:                                32 MiB (1 instance)
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-3
Vulnerability Gather data sampling:      Not affected
Vulnerability Indirect target selection: Not affected
Vulnerability Itlb multihit:             Not affected
Vulnerability L1tf:                      Not affected
Vulnerability Mds:                       Not affected
Vulnerability Meltdown:                  Not affected
Vulnerability Mmio stale data:           Not affected
Vulnerability Reg file data sampling:    Not affected
Vulnerability Retbleed:                  Not affected
Vulnerability Spec rstack overflow:      Vulnerable: Safe RET, no microcode
Vulnerability Spec store bypass:         Vulnerable
Vulnerability Spectre v1:                Mitigation; usercopy/swapgs barriers and __user pointer sanitization
Vulnerability Spectre v2:                Mitigation; Retpolines; STIBP disabled; RSB filling; PBRSB-eIBRS Not affected; BHI Not affected
Vulnerability Srbds:                     Not affected
Vulnerability Tsa:                       Vulnerable: Clear CPU buffers attempted, no microcode
Vulnerability Tsx async abort:           Not affected
Vulnerability Vmscape:                   Not affected
```
</details>

Note: 4 vCPUs is only **2 physical cores**. Two busy threads on sibling
hyperthreads share one core's execution units.

## Method

- `scripts/run_benchmarks.sh`: server + client on the same VM over loopback
  TCP. Server: `--workers 2` (the default on this machine: hardware threads / 2),
  10 symbols, ring capacity 65,536. Client: `--duration 10`, one thread
  and one TCP connection per consumer.
- Runs are **interleaved** (run 1 of all 6 configurations, then run 2, …) so
  slow drift in the shared VM spreads across configurations.
- Two producer modes:
  - **steady**: producer rate-limited to **200,000 ticks/s**, the sustained
    workload. The expected result is every consumer receiving everything.
  - **stress**: unthrottled producer. Deliberate overload to exercise
    drop-oldest.
- **Throughput** = ticks received per consumer ÷ that connection's measured
  connect-to-close time. Aggregate = sum over consumers.
- **Producer ticks/s** comes from the server's once-per-second counters while
  all clients were connected.
- **Latency** = client receive wall clock − producer timestamp, recorded
  for **every** message (not sampled). Both clocks are the same machine's
  `CLOCK_REALTIME`. P50/P99/P99.9 are **nearest-rank**, so each is an
  actually observed sample. Messages from one `recv()` batch share one
  receive timestamp.
- **Dropped %** = dropped ÷ (sent + dropped), from the server's per-consumer
  counters.

## Results

### Mode: steady

| Consumers | Runs | Aggregate ticks/s delivered | Per-consumer ticks/s | Producer ticks/s | P50 µs | P99 µs | P99.9 µs | Max µs | Dropped % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 5 | 199,999 (199,997 – 200,003) | 199,999 (199,997 – 200,003) | 200,053 (200,030 – 200,169) | 31.5 (29.4 – 33.6) | 1,499.6 (1,011.8 – 2,701.3) | 2,763.5 (2,632.3 – 7,465.3) | 5,977.9 (5,347.7 – 9,073.3) | 0.00 (0.00 – 0.00) |
| 4 | 5 | 800,001 (799,986 – 800,239) | 200,000 (199,996 – 200,060) | 200,094 (200,073 – 200,105) | 53.6 (50.9 – 54.3) | 1,345.8 (1,025.2 – 1,526.1) | 3,141.7 (2,752.8 – 3,242.8) | 7,765.1 (6,309.2 – 10,272.2) | 0.00 (0.00 – 0.00) |
| 12 | 5 | 2,400,038 (2,399,791 – 2,400,138) | 200,003 (199,983 – 200,012) | 200,085 (200,063 – 200,158) | 84.3 (84.1 – 87.9) | 1,959.0 (1,881.8 – 2,161.1) | 4,175.3 (3,515.0 – 4,720.2) | 8,848.1 (7,262.2 – 12,058.5) | 0.00 (0.00 – 0.00) |

### Mode: stress

| Consumers | Runs | Aggregate ticks/s delivered | Per-consumer ticks/s | Producer ticks/s | P50 µs | P99 µs | P99.9 µs | Max µs | Dropped % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 5 | 11,167,068 (10,423,563 – 11,280,070) | 11,167,068 (10,423,563 – 11,280,070) | 11,317,574 (10,404,965 – 11,444,428) | 39.3 (37.9 – 42.2) | 3,108.4 (2,731.6 – 4,747.8) | 7,250.0 (5,933.2 – 8,353.4) | 12,395.6 (10,793.7 – 13,367.7) | 0.06 (0.00 – 0.16) |
| 4 | 5 | 37,949,293 (37,416,420 – 39,034,410) | 9,487,323 (9,354,105 – 9,758,602) | 10,018,725 (9,894,748 – 10,414,948) | 542.7 (334.6 – 672.4) | 7,969.8 (7,725.0 – 8,222.1) | 11,213.4 (10,846.0 – 14,244.1) | 22,501.4 (16,860.2 – 24,408.8) | 2.86 (2.53 – 3.26) |
| 12 | 5 | 46,068,855 (45,873,540 – 46,376,919) | 3,839,071 (3,822,795 – 3,864,743) | 9,421,540 (8,868,785 – 9,801,598) | 6,765.3 (6,758.2 – 6,983.7) | 14,977.5 (14,522.2 – 16,429.3) | 22,280.4 (19,865.1 – 23,691.2) | 28,387.5 (25,399.4 – 44,160.6) | 56.75 (56.67 – 58.11) |

### Headline numbers (medians)

| Consumers | Steady: aggregate ticks/s | Steady: P50 / P99 / P99.9 µs | Stress: aggregate ticks/s | Stress: P50 / P99 / P99.9 µs | Stress: dropped |
|---:|---:|---:|---:|---:|---:|
| 1  | 199,999   | 31.5 / 1,499.6 / 2,763.5 | 11,167,068 | 39.3 / 3,108.4 / 7,250.0 | 0.06% |
| 4  | 800,001   | 53.6 / 1,345.8 / 3,141.7 | 37,949,293 | 542.7 / 7,969.8 / 11,213.4 | 2.86% |
| 12 | 2,400,038 | 84.3 / 1,959.0 / 4,175.3 | 46,068,855 | 6,765.3 / 14,977.5 / 22,280.4 | 56.75% |

## What the numbers say

**Steady (200k ticks/s):** every configuration delivered the full rate to
every consumer (199,983 – 200,060 ticks/s per consumer across all 15 runs)
with **0 drops**. Median latency is tens of µs (31.5 µs at 1 consumer,
84.3 µs at 12). The tail is in milliseconds: P99 1.3–2.0 ms, P99.9
2.8–4.2 ms. The next section explains why.

**Stress (unthrottled):** the producer tops out at about **10–11M ticks/s**.
The profile shows this limit is the tick *generation* (OU step, normal RNG
with `log`, and `clock_gettime` per tick), not the ring buffer publish.
- 1 consumer keeps up almost fully (11.17M/s delivered, 0.06% dropped).
- 4 consumers: 37.9M/s aggregate, 9.49M/s each, 2.86% dropped.
- 12 consumers: aggregate rises to 46.1M/s, but each consumer gets only
  3.84M/s of the ~9.4M/s produced. **56.75% of ticks are dropped per
  consumer.** This is drop-oldest working as designed: the producer is never
  slowed (its rate at 12 consumers, 9.42M/s, is close to its rate at 4), and
  lagging consumers skip ahead instead of falling behind. Latency stays
  bounded (P99.9 22.3 ms, max 28.4 ms median) because the backlog is capped.
  No tick can be older than what the ring (65,536), the 64 KB send queue
  and the kernel socket buffers hold.

## 12 consumers on 4 vCPUs: oversubscription

12 consumers on this VM is oversubscribed by a wide margin, and the numbers
show it. The server's two worker threads **busy-poll**: they call
`epoll_wait(timeout = 0)` and check the ring in a tight loop, so each burns
~0.9–1.0 CPU **even when there is little to send**. Measured with
`perf stat --per-thread` over 10 s in steady mode with 4 clients: workers used
9,901.64 ms and 9,850.25 ms of CPU, while the producer (sleeping between
ticks) used 1,193.13 ms. In the steady profile, 17%+ of all samples per
worker are `epoll_wait` itself, and each worker spends 17–18% of samples
in the kernel.

So 2 of the 4 vCPUs (one full physical core's worth) are already taken by the
workers. In stress mode the producer takes a third. The 12 client receive
threads share what's left.

Measured directly from `/proc/<pid>/task/*/schedstat` (time spent
**runnable but waiting for a CPU**) over an 8 s window, 12 clients:

| Thread(s) | Steady: CPU ran | Steady: waited for CPU | Stress: CPU ran | Stress: waited for CPU |
|---|---:|---:|---:|---:|
| 12 client receive threads + client main | 10,232 ms | **17,004 ms** | 7,098 ms | **11,900 ms** |
| producer | 957 ms | 1,268 ms | 7,044 ms | 1,174 ms |
| worker-0 | 7,535 ms | 618 ms | 7,229 ms | 995 ms |
| worker-1 | 7,301 ms | 845 ms | 7,199 ms | 1,021 ms |
| hypervisor steal (whole VM) | 0 | | 0 | |

The 13 client threads together spent **17.0 s waiting for a CPU during an
8 s window**. That runqueue wait is where the millisecond tails come from:
a tick is sent promptly, but the thread that will read it isn't scheduled
yet. Steal time was 0, so the contention is inside this VM, not from
neighbouring tenants. In stress mode the same contention is why per-consumer
throughput falls from 9.49M/s (4 consumers) to 3.84M/s (12): the consumers
can't get enough CPU to keep up, get lapped, and drop.

Two caveats:
- The client harness runs on the same 4 vCPUs as the server, so these numbers
  measure server + clients sharing one small machine. A real deployment
  would put clients elsewhere.
- Busy-polling is a latency-for-CPU trade that pays off with dedicated cores
  and hurts on an oversubscribed box like this one. A spin-then-block
  backoff (sleep in `epoll_wait` after N empty polls) would likely cut
  steady-state CPU sharply, at some cost to P50. That is a design change and
  was **not** made here.

## Profiling (perf)

**Hardware counters were not available.** The Azure hypervisor exposes no CPU
PMU to this VM (`/sys/bus/event_source/devices/` has no `cpu` entry), so
`cycles`, `instructions`, `cache-misses` and `branch-misses` all report
`<not supported>`, even with `sudo`. Without `sudo`, perf is refused entirely
(`perf_event_paranoid = 4`). **There are therefore no IPC or cache-miss
figures in this report.** What follows uses software events (`task-clock`,
`context-switches`, `cpu-migrations`, `page-faults`) and timer-based
`cpu-clock` sampling at 999 Hz, via `scripts/run_perf.sh`. That script uses a
separate `-O3 -g -fno-omit-frame-pointer` build so call stacks resolve, and
loads the server with 4 clients for 30 s. Raw output is in
[`bench/results/codespace/perf/`](bench/results/codespace/perf/).

`perf stat --per-thread`, 10 s window, 4 clients:

| Thread | Stress: task-clock | Stress: ctx switches | Steady: task-clock | Steady: ctx switches |
|---|---:|---:|---:|---:|
| mde-producer | 9,126.52 ms | 16,871 | 1,193.13 ms | 74,485 |
| mde-worker-0 | 9,175.48 ms | 19,053 | 9,901.64 ms | 1,931 |
| mde-worker-1 | 9,399.45 ms | 13,248 | 9,850.25 ms | 3,475 |
| mde-acceptor | 2.06 ms | 199 | 1.92 ms | 200 |
| page faults (all threads) | 0 | | 3 | |

The steady producer's 74,485 context switches in 10 s come from
`sleep_until` pacing. At 200k ticks/s the 5 µs interval is below timer
resolution, so the producer wakes ~7.4k times/s and publishes a small burst
each time. That short burstiness is part of the P50.

`perf record` hot spots, stress mode (share of all samples; entries below
0.5% omitted, so the shown rows sum to ~75%):

| Share | Thread | Symbol | What it is |
|---:|---|---|---|
| 18.2% / 17.5% | worker-0 / worker-1 | worker loop lambda | `try_read` + `encode_tick` + queue append, all inlined into the loop |
| 12.3% | producer | `TickProducer::step_symbol` | OU price step (normal RNG) |
| 8.0% | producer | `clock_gettime` (vDSO) | `system_clock::now()` for every tick's timestamp |
| 5.0% | producer | `TickProducer::run` | loop body incl. the inlined ring `publish()` |
| 3.3% / 3.0% | workers | `_raw_spin_unlock_irqrestore` (kernel) | waking the receiving socket inside loopback `send()` |
| 2.6% | producer | `__ieee754_log_fma` | `log` inside `std::normal_distribution` |
| ~1% each | workers | `epoll_wait` | the busy poll |

User vs kernel split of the shown samples: stress workers 19.1% / 4.4%
(worker-0) and 18.6% / 3.7% (worker-1), producer 29.3% user. Steady workers
22.3% / 17.3% and 21.0% / 18.4%, i.e. at low load nearly half of a worker's
time is the `epoll_wait` syscall path.

Takeaways:
1. The ring buffer is not the bottleneck. `publish()` is inlined into
   `TickProducer::run` (5.0% total for the loop), and the producer's cost is
   dominated by generating the tick (`step_symbol` + `log` + `clock_gettime`
   ≈ 23% of all samples).
2. The workers' userspace cost (ring read + encode + queue) sits in one
   inlined lambda. Without hardware counters we can't split it further into
   cache misses vs instructions.
3. At steady load the dominant cost is the busy poll itself.

## Problems found during benchmarking (reported, not hidden)

1. **Client harness artifact inflated stress tail latency up to ~100×.**
   This was the first full matrix run in this codespace. The client appended
   every latency to one `std::vector`. Each doubling copied hundreds of MB
   and page-faulted new memory **on the receiving thread**, stalling it for
   100+ ms, so ticks queued meanwhile were measured as 100–900 ms old.
   *Evidence:* a 10 s 1-client stress run with latency storage disabled had
   **0 of 108,996,052** messages over 50 ms. With storage in fixed 512 KB
   chunks, the same run's P99 went from ~125 ms to 3.5 ms and max from up to
   899 ms to 10.5 ms. Before finding this, I had guessed kernel socket
   buffers were responsible. `ss` showed they held at most 1,461,312 bytes
   (~61k ticks, a few ms) across 4 sockets, which **ruled that out**. All
   30 runs were redone with the fixed harness. The superseded results are
   kept below and their raw logs are in
   [`bench/results/codespace_superseded_harness_bug/`](bench/results/codespace_superseded_harness_bug/).
2. **A 30 s profiling client was killed.** With per-message latency storage
   at ~30M msgs/s, the client reached 11.2 GB resident and was terminated with
   SIGTERM (exit 143; no kernel OOM-kill record, cgroup `oom_kill 0`). Load
   generators used for profiling now pass `--record-latency 0`. The 10 s
   benchmark runs were unaffected: all 30 exited 0.
3. **Hardware perf counters are unavailable** (see above).

### Superseded: first codespace matrix (client harness bug, not used)

Same hardware and method, before the harness fix. Throughput and steady
medians are similar. The stress tail latency is dominated by the harness
stalls described above.

#### steady

| Consumers | Runs | Aggregate ticks/s delivered | Per-consumer ticks/s | Producer ticks/s | P50 µs | P99 µs | P99.9 µs | Max µs | Dropped % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 5 | 200,002 (199,997 – 200,002) | 200,002 (199,997 – 200,002) | 200,039 (200,030 – 200,099) | 32.5 (31.5 – 33.4) | 1,397.5 (1,327.5 – 1,739.2) | 3,073.8 (2,809.2 – 7,590.0) | 8,459.6 (5,530.3 – 13,524.3) | 0.00 (0.00 – 0.00) |
| 4 | 5 | 799,999 (799,984 – 800,036) | 200,000 (199,996 – 200,009) | 200,121 (200,048 – 200,124) | 55.6 (54.6 – 56.3) | 1,752.5 (1,252.1 – 1,952.0) | 7,671.6 (6,911.9 – 13,213.9) | 22,838.9 (18,403.6 – 27,172.1) | 0.00 (0.00 – 0.00) |
| 12 | 5 | 2,399,985 (2,399,931 – 2,400,016) | 199,999 (199,994 – 200,001) | 200,190 (200,131 – 200,247) | 86.0 (84.8 – 91.5) | 2,854.7 (2,578.4 – 3,637.8) | 29,499.0 (24,346.6 – 101,551.2) | 54,264.1 (42,663.2 – 219,117.4) | 0.00 (0.00 – 0.00) |

#### stress

| Consumers | Runs | Aggregate ticks/s delivered | Per-consumer ticks/s | Producer ticks/s | P50 µs | P99 µs | P99.9 µs | Max µs | Dropped % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 5 | 9,730,551 (9,510,882 – 9,985,229) | 9,730,551 (9,510,882 – 9,985,229) | 10,894,967 (10,554,252 – 11,164,464) | 48.2 (38.3 – 58.1) | 125,170.4 (109,371.2 – 151,812.7) | 621,725.3 (511,418.1 – 866,225.2) | 629,426.2 (519,599.4 – 899,276.8) | 10.15 (8.27 – 12.73) |
| 4 | 5 | 30,119,738 (26,767,794 – 30,674,208) | 7,529,934 (6,691,948 – 7,668,552) | 9,394,007 (8,325,880 – 10,316,990) | 1,179.9 (373.0 – 1,685.5) | 364,117.4 (314,740.1 – 481,953.8) | 1,065,940.7 (984,025.5 – 1,747,124.1) | 1,123,095.4 (1,047,547.4 – 1,823,015.0) | 16.84 (12.47 – 23.51) |
| 12 | 5 | 39,494,208 (38,827,684 – 39,660,414) | 3,291,184 (3,235,640 – 3,305,034) | 8,672,131 (8,103,531 – 9,450,348) | 6,919.3 (6,758.4 – 7,400.1) | 182,462.6 (152,449.7 – 328,662.4) | 554,164.2 (355,900.5 – 594,319.4) | 714,457.5 (552,161.3 – 875,559.0) | 57.25 (55.89 – 58.84) |

## Earlier runs on thermally throttled hardware (not used)

These are from the original development laptop, before this repo moved to
Codespaces. That machine was overheating, so these numbers aren't
comparable with anything above. They are kept only for the record. Raw logs:
[`bench/results/laptop_throttled_not_used/`](bench/results/laptop_throttled_not_used/).

- Hardware: 11th Gen Intel Core i7-1165G7 @ 2.80GHz, 4 cores / 8 threads,
  WSL2 (Linux 6.6.87.1-microsoft-standard-WSL2), GCC 13.3.0.
- Config: 4 workers, 20 s, **one run each** (no repeats), steady rate
  200,000/s.
- Code differences: the old ring buffer (with the phantom-read and
  over-drop bugs), an 8 MB per-client send queue, and a client that made one
  `recv()` per message and sampled latency for 1 in 64 messages with
  interpolated percentiles.

| Mode | Consumers | Aggregate ticks/s | P50 µs | P99 µs | P99.9 µs | Server dropped |
|---|---:|---:|---:|---:|---:|---:|
| steady | 1 | 199,999 | 143.69 | 39,304.2 | 60,791.1 | 0 |
| steady | 4 | 799,944 | 1,545.52 | 39,392.3 | 219,887 | 0 |
| steady | 12 | 2.39959e+06 | 108,204 | 1.5766e+06 | 1.79361e+06 | 0 |
| stress | 1 | 501,489 | 925,461 | 1.50306e+06 | 1.52274e+06 | 48,300,032 |
| stress | 4 | 1.46885e+06 | 1.38825e+06 | 2.29216e+06 | 2.46853e+06 | 184,942,592 |
| stress | 12 | 2.49753e+06 | 2.33211e+06 | 3.68031e+06 | 3.78821e+06 | 303,955,968 |

(Values are copied exactly as the old logs printed them, including their
scientific notation.)

## Appendix: every run

| Mode | Consumers | Run | Agg ticks/s | P50 µs | P99 µs | P99.9 µs | Max µs | Dropped % | Latency samples |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| steady | 1 | 1 | 199,999 | 33.6 | 2,701.3 | 7,465.3 | 9,073.3 | 0.00 | 1,999,996 |
| steady | 1 | 2 | 200,001 | 31.2 | 1,265.8 | 2,758.4 | 6,549.1 | 0.00 | 2,000,017 |
| steady | 1 | 3 | 199,997 | 31.5 | 1,682.1 | 2,862.3 | 5,528.9 | 0.00 | 1,999,994 |
| steady | 1 | 4 | 199,998 | 29.4 | 1,011.8 | 2,632.3 | 5,347.7 | 0.00 | 1,999,987 |
| steady | 1 | 5 | 200,003 | 31.5 | 1,499.6 | 2,763.5 | 5,977.9 | 0.00 | 2,000,051 |
| steady | 4 | 1 | 799,986 | 53.9 | 1,526.1 | 3,107.6 | 6,309.2 | 0.00 | 7,999,916 |
| steady | 4 | 2 | 800,005 | 50.9 | 1,345.8 | 3,242.8 | 7,213.3 | 0.00 | 8,000,094 |
| steady | 4 | 3 | 800,001 | 54.3 | 1,025.2 | 2,752.8 | 8,533.6 | 0.00 | 8,000,064 |
| steady | 4 | 4 | 799,990 | 53.6 | 1,329.4 | 3,166.8 | 10,272.2 | 0.00 | 7,999,958 |
| steady | 4 | 5 | 800,239 | 52.5 | 1,363.4 | 3,141.7 | 7,765.1 | 0.00 | 8,002,453 |
| steady | 12 | 1 | 2,400,138 | 87.9 | 2,161.1 | 4,720.2 | 12,058.5 | 0.00 | 24,002,381 |
| steady | 12 | 2 | 2,399,994 | 84.3 | 1,943.4 | 4,451.9 | 8,888.2 | 0.00 | 24,000,077 |
| steady | 12 | 3 | 2,399,791 | 84.3 | 1,881.8 | 3,515.0 | 7,262.2 | 0.00 | 23,999,842 |
| steady | 12 | 4 | 2,400,038 | 84.1 | 1,959.0 | 3,967.5 | 8,369.0 | 0.00 | 24,000,518 |
| steady | 12 | 5 | 2,400,046 | 85.3 | 1,996.6 | 4,175.3 | 8,848.1 | 0.00 | 24,001,021 |
| stress | 1 | 1 | 10,423,563 | 39.3 | 4,747.8 | 8,353.4 | 12,395.6 | 0.16 | 104,235,736 |
| stress | 1 | 2 | 11,167,068 | 41.1 | 2,731.6 | 5,933.2 | 10,793.7 | 0.00 | 111,670,957 |
| stress | 1 | 3 | 10,800,297 | 38.3 | 2,990.6 | 6,434.4 | 13,067.3 | 0.00 | 108,002,985 |
| stress | 1 | 4 | 11,280,070 | 42.2 | 3,582.5 | 7,250.0 | 10,985.6 | 0.16 | 112,806,033 |
| stress | 1 | 5 | 11,274,550 | 37.9 | 3,108.4 | 8,084.0 | 13,367.7 | 0.06 | 112,745,727 |
| stress | 4 | 1 | 37,416,420 | 542.7 | 8,102.0 | 11,213.4 | 22,501.4 | 3.26 | 374,171,221 |
| stress | 4 | 2 | 38,610,312 | 672.4 | 7,969.8 | 14,244.1 | 24,408.8 | 2.53 | 386,106,219 |
| stress | 4 | 3 | 37,576,592 | 334.6 | 7,940.6 | 11,030.7 | 16,860.2 | 2.70 | 375,767,396 |
| stress | 4 | 4 | 37,949,293 | 468.9 | 8,222.1 | 12,779.5 | 23,081.0 | 3.02 | 379,496,850 |
| stress | 4 | 5 | 39,034,410 | 645.0 | 7,725.0 | 10,846.0 | 16,963.7 | 2.86 | 390,370,290 |
| stress | 12 | 1 | 45,873,540 | 6,983.7 | 16,429.3 | 22,280.4 | 28,387.5 | 56.68 | 458,752,998 |
| stress | 12 | 2 | 45,905,518 | 6,765.3 | 14,522.2 | 20,276.3 | 26,368.6 | 58.11 | 459,087,606 |
| stress | 12 | 3 | 46,068,855 | 6,758.2 | 14,787.9 | 23,691.2 | 44,160.6 | 56.75 | 460,701,988 |
| stress | 12 | 4 | 46,376,919 | 6,758.2 | 15,938.6 | 22,662.3 | 38,992.6 | 56.67 | 463,786,917 |
| stress | 12 | 5 | 46,301,937 | 6,809.8 | 14,977.5 | 19,865.1 | 25,399.4 | 57.44 | 463,034,984 |

## Reproduce

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
scripts/run_benchmarks.sh                                   # ~9 min
python3 scripts/summarize_benchmarks.py bench/results/codespace
scripts/run_perf.sh                                         # needs sudo
```
