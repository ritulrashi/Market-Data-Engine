#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mde {

// Lock-free, bounded, single-producer / multi-consumer ring buffer.
//
// Design (sequence-tagged slots, same family as the LMAX Disruptor):
//   - Capacity must be a power of two so `index & mask` replaces `% Capacity`.
//   - Each slot stores the payload plus an atomic `sequence` number: the
//     0-based publish count of whichever item currently occupies the slot.
//   - The producer is the only thread that ever writes slot payloads, and
//     publishes by release-storing `sequence` after writing the payload.
//   - Each consumer owns a private cursor (ConsumerHandle) and never
//     mutates shared state other than reading it. To read the next item it
//     acquire-loads the target slot's sequence and compares it to the
//     cursor:
//       seq <  cursor  -> not published yet, caller should retry later
//       seq == cursor  -> exactly the item we want; copy it out, then
//                         re-check `sequence` (acquire) to confirm the
//                         producer did not overwrite the slot while we were
//                         copying (a torn read). If it changed, retry.
//       seq >  cursor  -> the producer has lapped us: this slot has been
//                         overwritten one or more times since we last read.
//                         The unread items are gone. Count them as dropped
//                         and fast-forward the cursor to the newest
//                         available sequence.
//   - On overflow the producer never blocks and never drops what it's
//     writing -- it always publishes. It is *readers* who may discover
//     later that older, unread data was overwritten (drop-oldest
//     semantics), which is exactly what gets counted per consumer.
//
// Payload copies are word-atomic (see NOTE below) rather than plain
// load/store, because a slow consumer can genuinely be mid-copy of a slot
// when the producer wraps around and starts overwriting that same slot --
// that overlap is inherent to "drop-oldest without ever blocking the
// producer" and is exercised for real under sustained load, not just in
// theory.
//
// NOTE on two bugs ThreadSanitizer found here, and how each was fixed:
//
// 1. Data race (UB), plain payload copy. An earlier version copied slot
//    payloads with a plain (non-atomic) `T` load/store, relying solely on
//    the sequence-number recheck to detect and discard torn reads. That is
//    memory-safe on real x86_64 hardware (a torn struct copy just contains
//    a mix of old/new bytes, still readable memory), but it is formally a
//    concurrent unsynchronized read/write of the same non-atomic object,
//    which the C++ memory model treats as a data race (undefined behavior)
//    regardless of whether the result is later discarded. TSan caught this
//    under tests/test_ring_buffer.cpp's test_multi_consumer_concurrent (8
//    consumers vs. a fast producer over a small ring, guaranteeing laps): a
//    plain read in try_read() raced with a plain write in publish() on the
//    same slot. Fix: copy the payload one 8-byte word at a time through
//    std::atomic<uint64_t> with memory_order_relaxed. Relaxed atomics have
//    no data races by definition, and 8-byte aligned atomics are native,
//    single-instruction loads/stores on x86_64, so this stays lock-free
//    with no measurable cost over the plain version.
//
// 2. Correctness bug (not a race, still wrong), in-progress overwrite not
//    detected. Fixing (1) alone left a logic hole: the before/after
//    sequence check only detects an overwrite that has *finished*
//    (`sequence` already advanced to the next generation). It cannot
//    detect one that is *in progress* -- the producer can be partway
//    through overwriting a slot's words while `sequence` still shows the
//    old (matching) value, because `publish()` only stores the new
//    `sequence` after all words are written. A reader whose word-by-word
//    read overlaps exactly that window can assemble a mix of old and new
//    words while both its before- and after-checks still see the same old
//    sequence number -- a torn value that looks "verified". This showed up
//    as a real, reproducible test failure (monotonically increasing
//    per-consumer values coming back out of order) once (1) was fixed and
//    the race noise was gone. Fix: a standard seqlock busy marker. The
//    producer stores a reserved `kBusy` sentinel into `sequence` *before*
//    touching any words, and only stores the real (new) sequence number
//    after all words are written. Any reader whose read window overlaps an
//    overwrite -- in progress or freshly completed -- is now guaranteed to
//    observe *some* change away from the exact value it started with
//    (kBusy, or the new sequence), so it always retries instead of
//    assembling a torn value. See tests/test_ring_buffer.cpp for a stress
//    test that reproduces the original failure and stays green with this
//    fix.
//
// Slots are padded to a cache line to avoid false sharing between adjacent
// slots when the producer is publishing and consumers are reading nearby
// indices concurrently.
template <typename T, std::size_t Capacity>
class SpmcRingBuffer {
public:
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable for word-atomic slot access");
    static_assert(sizeof(T) % sizeof(std::uint64_t) == 0,
                  "T's size must be a multiple of 8 bytes for word-atomic slot copies");
    // Note: T itself need not be 8-byte aligned (TickMessage is #pragma
    // pack(1), so alignof(TickMessage) == 1). write_payload/read_payload
    // only ever reinterpret an intermediate std::uint64_t[] buffer as
    // atomic words, never T's own storage directly, so T's alignment is
    // irrelevant to the atomics -- memcpy in/out of T is alignment-agnostic.

    // Per-consumer read state. Not shared between threads -- each consumer
    // (e.g. each connected TCP client) owns exactly one of these and reads
    // through it from a single thread at a time.
    struct ConsumerHandle {
        std::uint64_t next_seq = 0;
        std::uint64_t dropped = 0;
    };

    SpmcRingBuffer() = default;
    SpmcRingBuffer(const SpmcRingBuffer&) = delete;
    SpmcRingBuffer& operator=(const SpmcRingBuffer&) = delete;

    // Producer-only. Must not be called concurrently from more than one
    // thread (single-producer invariant).
    void publish(const T& item) noexcept {
        const std::uint64_t seq = next_write_seq_.load(std::memory_order_relaxed);
        Slot& slot = buffer_[seq & kMask];
        // Mark the slot busy before touching any words, so a reader whose
        // read window overlaps this overwrite is guaranteed to see *some*
        // change away from the value it started with (see class-level NOTE).
        slot.sequence.store(kBusy, std::memory_order_relaxed);
        write_payload(slot, item);
        slot.sequence.store(seq, std::memory_order_release);     // publish
        next_write_seq_.store(seq + 1, std::memory_order_relaxed);
    }

    // Attach a new consumer starting at the current write position, i.e. it
    // will only see ticks published from this point forward.
    ConsumerHandle create_consumer() const noexcept {
        return ConsumerHandle{next_write_seq_.load(std::memory_order_relaxed), 0};
    }

    // Non-blocking. Returns true and fills `out` if an item was available.
    // Returns false if the consumer is caught up to the producer (nothing
    // new yet) -- the caller should back off and retry. Never blocks.
    bool try_read(ConsumerHandle& c, T& out) noexcept {
        for (;;) {
            Slot& slot = buffer_[c.next_seq & kMask];
            const std::uint64_t seq_before = slot.sequence.load(std::memory_order_acquire);

            if (seq_before == kBusy) {
                return false; // producer is mid-overwrite of this slot right now; try later
            }
            if (seq_before < c.next_seq) {
                return false; // not published yet
            }
            if (seq_before > c.next_seq) {
                // Lapped: the slot we wanted has since been overwritten.
                c.dropped += (seq_before - c.next_seq);
                c.next_seq = seq_before;
                continue; // re-evaluate the same index against the new cursor
            }

            T candidate = read_payload(slot);
            const std::uint64_t seq_after = slot.sequence.load(std::memory_order_acquire);
            if (seq_after != seq_before) {
                if (seq_after == kBusy) {
                    // Producer started overwriting mid-read; we don't yet
                    // know the new sequence number. Retry once it publishes.
                    continue;
                }
                // Producer finished overwriting this slot while we were
                // copying it -- the copy we just took is torn. Treat as a
                // lap and retry.
                c.dropped += (seq_after - seq_before);
                c.next_seq = seq_after;
                continue;
            }

            out = candidate;
            ++c.next_seq;
            return true;
        }
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;
    static constexpr std::size_t kWords = sizeof(T) / sizeof(std::uint64_t);
    // Reserved sequence value marking "producer is mid-overwrite of this
    // slot". Never a valid publish count (would require 2^64 publishes).
    static constexpr std::uint64_t kBusy = ~std::uint64_t{0};

    struct alignas(64) Slot {
        std::atomic<std::uint64_t> sequence{0};
        std::array<std::atomic<std::uint64_t>, kWords> words{};
    };

    static void write_payload(Slot& slot, const T& item) noexcept {
        std::uint64_t tmp[kWords];
        std::memcpy(tmp, &item, sizeof(T));
        for (std::size_t i = 0; i < kWords; ++i) {
            slot.words[i].store(tmp[i], std::memory_order_relaxed);
        }
    }

    static T read_payload(const Slot& slot) noexcept {
        std::uint64_t tmp[kWords];
        for (std::size_t i = 0; i < kWords; ++i) {
            tmp[i] = slot.words[i].load(std::memory_order_relaxed);
        }
        T out;
        std::memcpy(&out, tmp, sizeof(T));
        return out;
    }

    std::array<Slot, Capacity> buffer_{};
    alignas(64) std::atomic<std::uint64_t> next_write_seq_{0};
};

} // namespace mde
