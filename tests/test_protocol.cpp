#include "protocol/tick.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstring>

void test_wire_layout() {
    using mde::TickMessage;

    CHECK(sizeof(TickMessage) == 24);
    CHECK(offsetof(TickMessage, symbol_id) == 0);
    CHECK(offsetof(TickMessage, price) == 4);
    CHECK(offsetof(TickMessage, size) == 12);
    CHECK(offsetof(TickMessage, timestamp_ns) == 16);

    TickMessage t{};
    t.symbol_id = 7;
    t.price = 123.456;
    t.size = 42;
    t.timestamp_ns = 1234567890123456789ULL;

    unsigned char raw[sizeof(TickMessage)];
    std::memcpy(raw, &t, sizeof(t));

    TickMessage roundtrip{};
    std::memcpy(&roundtrip, raw, sizeof(roundtrip));

    CHECK(roundtrip.symbol_id == t.symbol_id);
    CHECK(roundtrip.price == t.price);
    CHECK(roundtrip.size == t.size);
    CHECK(roundtrip.timestamp_ns == t.timestamp_ns);
}
