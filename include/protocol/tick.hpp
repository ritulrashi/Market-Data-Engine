#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mde {

// Fixed-size binary wire message. Every tick sent over the wire is exactly
// this many bytes, back to back, with no length prefix and no framing.
//
// Byte layout (24 bytes total, every field little-endian, no padding):
//
//   offset  size  field          type
//   ------  ----  -------------  --------
//   0       4     symbol_id      uint32_t
//   4       8     price          double (IEEE-754 binary64)
//   12      4     size           uint32_t
//   16      8     timestamp_ns   uint64_t (ns since Unix epoch)
//
// Bytes on the wire are produced/consumed only by encode_tick/decode_tick
// below. #pragma pack(1) additionally keeps the in-memory struct at 24 bytes
// (a multiple of 8, as the ring buffer's word-atomic slot copies require).
#pragma pack(push, 1)
struct TickMessage {
    std::uint32_t symbol_id;
    double price;
    std::uint32_t size;
    std::uint64_t timestamp_ns;
};
#pragma pack(pop)

static_assert(sizeof(TickMessage) == 24,
              "TickMessage must stay exactly 24 bytes -- wire protocol has no length prefix");
static_assert(std::is_trivially_copyable_v<TickMessage>,
              "TickMessage must be trivially copyable to be memcpy'd on/off the wire");

inline constexpr std::size_t kWireMessageSize = sizeof(TickMessage);

namespace detail {
template <typename U>
inline void put_le(std::uint8_t* out, U v) noexcept {
    for (std::size_t i = 0; i < sizeof(U); ++i) out[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
template <typename U>
inline U get_le(const std::uint8_t* in) noexcept {
    U v = 0;
    for (std::size_t i = 0; i < sizeof(U); ++i) v |= static_cast<U>(in[i]) << (8 * i);
    return v;
}
} // namespace detail

// Serializes `t` into exactly kWireMessageSize bytes at `out`, explicitly
// little-endian field by field, so the wire format does not depend on host
// byte order or struct layout. On x86-64 this compiles to plain moves.
inline void encode_tick(const TickMessage& t, std::uint8_t* out) noexcept {
    detail::put_le<std::uint32_t>(out + 0, t.symbol_id);
    detail::put_le<std::uint64_t>(out + 4, std::bit_cast<std::uint64_t>(t.price));
    detail::put_le<std::uint32_t>(out + 12, t.size);
    detail::put_le<std::uint64_t>(out + 16, t.timestamp_ns);
}

// Inverse of encode_tick. `in` must point at kWireMessageSize bytes.
inline TickMessage decode_tick(const std::uint8_t* in) noexcept {
    TickMessage t;
    t.symbol_id = detail::get_le<std::uint32_t>(in + 0);
    t.price = std::bit_cast<double>(detail::get_le<std::uint64_t>(in + 4));
    t.size = detail::get_le<std::uint32_t>(in + 12);
    t.timestamp_ns = detail::get_le<std::uint64_t>(in + 16);
    return t;
}

} // namespace mde
