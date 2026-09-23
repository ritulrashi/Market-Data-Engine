#include "protocol/tick.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

using mde::TickMessage;

namespace {
TickMessage round_trip(const TickMessage& in) {
    std::array<std::uint8_t, mde::kWireMessageSize> buf{};
    mde::encode_tick(in, buf.data());
    return mde::decode_tick(buf.data());
}

void expect_bit_identical(const TickMessage& a, const TickMessage& b) {
    EXPECT_EQ(a.symbol_id, b.symbol_id);
    // Compare bit patterns so NaN, -0.0 and denormals are checked exactly.
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.price), std::bit_cast<std::uint64_t>(b.price));
    EXPECT_EQ(a.size, b.size);
    EXPECT_EQ(a.timestamp_ns, b.timestamp_ns);
}
} // namespace

TEST(Protocol, LayoutIs24PackedBytes) {
    EXPECT_EQ(sizeof(TickMessage), 24u);
    EXPECT_EQ(mde::kWireMessageSize, 24u);
    EXPECT_EQ(offsetof(TickMessage, symbol_id), 0u);
    EXPECT_EQ(offsetof(TickMessage, price), 4u);
    EXPECT_EQ(offsetof(TickMessage, size), 12u);
    EXPECT_EQ(offsetof(TickMessage, timestamp_ns), 16u);
}

TEST(Protocol, EncodesExactLittleEndianBytes) {
    TickMessage t{0x01020304u, 1.0, 0x0A0B0C0Du, 0x1122334455667788ull};
    std::array<std::uint8_t, 24> buf{};
    mde::encode_tick(t, buf.data());
    const std::array<std::uint8_t, 24> expected{
        0x04, 0x03, 0x02, 0x01,                         // symbol_id
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F, // 1.0 = 0x3FF0000000000000
        0x0D, 0x0C, 0x0B, 0x0A,                         // size
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // timestamp_ns
    };
    EXPECT_EQ(buf, expected);
}

TEST(Protocol, RoundTripTypical) {
    TickMessage t{7, 123.456, 42, 1234567890123456789ull};
    expect_bit_identical(round_trip(t), t);
}

TEST(Protocol, RoundTripAllZeros) {
    TickMessage t{0, 0.0, 0, 0};
    std::array<std::uint8_t, 24> buf;
    buf.fill(0xAB);
    mde::encode_tick(t, buf.data());
    for (auto b : buf) EXPECT_EQ(b, 0u);
    expect_bit_identical(mde::decode_tick(buf.data()), t);
}

TEST(Protocol, RoundTripMaxIntegerFields) {
    TickMessage t{std::numeric_limits<std::uint32_t>::max(), 1.0,
                  std::numeric_limits<std::uint32_t>::max(),
                  std::numeric_limits<std::uint64_t>::max()};
    expect_bit_identical(round_trip(t), t);
}

TEST(Protocol, RoundTripExtremePrices) {
    using L = std::numeric_limits<double>;
    for (double p : {L::max(), L::lowest(), L::min(), -L::min(), L::denorm_min(), L::epsilon(),
                     0.0, -0.0, L::infinity(), -L::infinity(), 0.01}) {
        SCOPED_TRACE(p);
        TickMessage t{1, p, 1, 1};
        expect_bit_identical(round_trip(t), t);
    }
}

TEST(Protocol, RoundTripNaNKeepsBitPattern) {
    TickMessage t{3, std::numeric_limits<double>::quiet_NaN(), 5, 6};
    TickMessage r = round_trip(t);
    EXPECT_TRUE(std::isnan(r.price));
    expect_bit_identical(r, t);
}

TEST(Protocol, BackToBackMessagesDecodeIndependently) {
    std::array<std::uint8_t, 72> stream{};
    TickMessage msgs[3] = {{0, 10.5, 1, 100}, {1, 20.25, 2, 200}, {9, 30.125, 3, 300}};
    for (int i = 0; i < 3; ++i) mde::encode_tick(msgs[i], stream.data() + 24 * i);
    for (int i = 0; i < 3; ++i) expect_bit_identical(mde::decode_tick(stream.data() + 24 * i), msgs[i]);
}
