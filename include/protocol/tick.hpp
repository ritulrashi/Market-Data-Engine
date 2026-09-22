#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mde {

// Fixed-size binary wire message. Every tick sent over the wire is exactly
// this many bytes, back to back, with no length prefix and no framing.
//
// Byte layout (24 bytes total, little-endian, native x86_64 layout):
//
//   offset  size  field          type
//   ------  ----  -------------  --------
//   0       4     symbol_id      uint32_t
//   4       8     price          double (IEEE-754 binary64)
//   12      4     size           uint32_t
//   16      8     timestamp_ns   uint64_t (ns since Unix epoch)
//
// #pragma pack(1) removes any compiler-inserted padding so the in-memory
// struct layout is identical to the wire layout on any x86_64 build. The
// static_assert below fails the build if that ever stops being true.
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

} // namespace mde
