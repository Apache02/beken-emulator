#include <doctest/doctest.h>

#include <optional>
#include <utility>
#include <vector>

#include "periph/uart.h"

using namespace bk;

namespace {

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

struct FakeEndpoint : ByteEndpoint {
    std::vector<uint8_t> sent, incoming;
    int                  writes = 0;
    void                 write(std::span<const uint8_t> d) override {
        ++writes;
        sent.insert(sent.end(), d.begin(), d.end());
    }
    std::vector<uint8_t> read_nowait() override { return std::exchange(incoming, {}); }
    std::string          description() const override { return "fake"; }
};

constexpr uint32_t kFifoConfig = 0x04, kFifoStatus = 0x08, kData = 0x0C, kIntEnable = 0x10, kIntStatus = 0x14;

struct Fixture {
    FakeClock         clock;
    FakeEndpoint      ep;
    std::vector<bool> calls;
    Uart              uart;

    Fixture() : uart("uart", clock, [this](bool v) { calls.push_back(v); }) { uart.attach(&ep); }

    void     w(uint32_t off, uint32_t v) { uart.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return uart.read("regs", off, 4); }
    void     feed(size_t n, uint8_t first = 1) {
        for (size_t i = 0; i < n; ++i) ep.incoming.push_back(static_cast<uint8_t>(first + i));
    }
};

} // namespace

TEST_CASE("tx goes to endpoint") {
    Fixture f;
    f.w(kData, 0x1A5);
    f.w(kData, 0x42);
    f.uart.tick(1'000'000);
    CHECK(f.ep.sent == std::vector<uint8_t>{0xA5, 0x42});
    CHECK(f.uart.name() == "uart");
    CHECK(f.uart.ranges().at(0).name == "regs");
    CHECK(f.uart.ranges().at(0).size == 0x20);
}

TEST_CASE("tx burst reaches the endpoint in one write, 1 ms after its first byte") {
    Fixture f;
    f.clock.now = 5000;
    f.w(kData, '\r');
    f.clock.now = 105000; // 100 us later: still the same burst
    f.w(kData, '\n');
    CHECK(f.uart.next_event_ns() == std::optional<uint64_t>(1'005'000));
    f.uart.tick(1'004'999);
    CHECK(f.ep.writes == 0);
    f.uart.tick(1'005'000);
    CHECK(f.ep.writes == 1);
    CHECK(f.ep.sent == std::vector<uint8_t>{'\r', '\n'});
    CHECK_FALSE(f.uart.next_event_ns().has_value());
    // The next byte starts a new burst.
    f.clock.now = 2'000'000;
    f.w(kData, 'x');
    CHECK(f.uart.next_event_ns() == std::optional<uint64_t>(3'000'000));
    f.uart.tick(3'000'000);
    CHECK(f.ep.writes == 2);
    CHECK(f.ep.sent == std::vector<uint8_t>{'\r', '\n', 'x'});
}

TEST_CASE("tx deadline and rx stop-end deadline: next event is the earlier one") {
    Fixture f;
    f.feed(1);
    f.uart.tick(100); // stop-end armed for 1'000'100
    f.clock.now = 0;
    f.w(kData, 'a'); // tx deadline 1'000'000
    CHECK(f.uart.next_event_ns() == std::optional<uint64_t>(1'000'000));
}

TEST_CASE("reset sends pending tx bytes") {
    Fixture f;
    f.w(kData, 'o');
    f.w(kData, 'k');
    f.uart.reset();
    CHECK(f.ep.sent == std::vector<uint8_t>{'o', 'k'});
    CHECK_FALSE(f.uart.next_event_ns().has_value());
}

TEST_CASE("tx without endpoint is dropped") {
    Fixture f;
    f.uart.attach(nullptr);
    f.w(kData, 0x55);
    CHECK(f.ep.sent.empty());
    CHECK((f.r(kIntStatus) & 0x20) == 0); // enable is 0, but latch is internal
    f.w(kIntEnable, 0x20);
    CHECK((f.r(kIntStatus) & 0x20) == 0x20);
}

TEST_CASE("rx fifo status and pop") {
    Fixture f;
    CHECK(f.r(kFifoStatus) == ((1u << 17) | (1u << 19) | (1u << 20)));
    f.feed(3, 0x10);
    f.uart.tick(100);
    CHECK(f.r(kFifoStatus) == ((3u << 8) | (1u << 17) | (1u << 20) | (1u << 21)));
    CHECK(f.r(kData) == 0x1000);
    CHECK(f.r(kData) == 0x1100);
    CHECK(f.r(kData) == 0x1200);
    CHECK(f.r(kData) == 0);
    CHECK(f.r(kFifoStatus) == ((1u << 17) | (1u << 19) | (1u << 20)));
    f.w(kFifoStatus, 0xFFFFFFFF); // read-only
    CHECK(f.r(kFifoStatus) == ((1u << 17) | (1u << 19) | (1u << 20)));
}

TEST_CASE("rx threshold interrupt") {
    Fixture f;
    f.w(kFifoConfig, 3u << 8);
    CHECK(f.r(kFifoConfig) == (3u << 8));
    f.w(kIntEnable, 0x02);
    f.feed(2);
    f.uart.tick(10);
    CHECK(f.r(kIntStatus) == 0);
    CHECK(f.calls.back() == false);
    f.feed(1, 9);
    f.uart.tick(20);
    CHECK(f.r(kIntStatus) == 0x02);
    CHECK(f.calls.back() == true);
    f.r(kData);
    CHECK(f.r(kIntStatus) == 0);
    CHECK(f.calls.back() == false);
}

TEST_CASE("rx threshold zero needs at least one byte") {
    Fixture f;
    f.w(kIntEnable, 0x02);
    CHECK(f.r(kIntStatus) == 0);
    f.feed(1);
    f.uart.tick(1);
    CHECK(f.r(kIntStatus) == 0x02);
}

TEST_CASE("tx_stop_end latched on data write and cleared by write-1") {
    Fixture f;
    f.w(kIntEnable, 0x20);
    CHECK(f.r(kIntStatus) == 0);
    f.w(kData, 0x41);
    CHECK(f.r(kIntStatus) == 0x20);
    CHECK(f.calls.back() == true);
    f.w(kIntStatus, 0x01); // wrong bit: stays
    CHECK(f.r(kIntStatus) == 0x20);
    f.w(kIntStatus, 0x20);
    CHECK(f.r(kIntStatus) == 0);
    CHECK(f.calls.back() == false);
}

TEST_CASE("tx_need_write is always raw") {
    Fixture f;
    f.w(kIntEnable, 0x01);
    CHECK(f.r(kIntStatus) == 0x01);
    CHECK(f.calls.back() == true);
    f.w(kIntStatus, 0x01); // not latched: cannot be cleared
    CHECK(f.r(kIntStatus) == 0x01);
}

TEST_CASE("fifo depth 128 with backlog") {
    Fixture f;
    f.feed(200, 0);
    f.uart.tick(5);
    CHECK(f.r(kFifoStatus) == ((128u << 8) | (1u << 17) | (1u << 18) | (1u << 20) | (1u << 21)));
    for (unsigned i = 0; i < 200; ++i) {
        CHECK(f.r(kData) == (i << 8));
        // each pop pulls one byte from the backlog while it lasts
        unsigned remaining = 199 - i;
        CHECK(((f.r(kFifoStatus) >> 8) & 0xFF) == (remaining < 128 ? remaining : 128));
    }
    CHECK(f.r(kFifoStatus) == ((1u << 17) | (1u << 19) | (1u << 20)));
}

TEST_CASE("fifo backlog bytes arrive in order after pops") {
    Fixture f;
    f.feed(130, 0);
    f.uart.tick(5);
    CHECK(((f.r(kFifoStatus) >> 8) & 0xFF) == 128);
    CHECK(f.r(kData) == 0x0000);
    CHECK(((f.r(kFifoStatus) >> 8) & 0xFF) == 128); // refilled from backlog
    CHECK(f.r(kData) == 0x0100);
    CHECK(((f.r(kFifoStatus) >> 8) & 0xFF) == 128);
    CHECK(f.r(kData) == 0x0200);
    CHECK(((f.r(kFifoStatus) >> 8) & 0xFF) == 127);
}

TEST_CASE("irq line follows raw & enable") {
    Fixture f;
    f.w(kIntEnable, 0x20);
    CHECK(f.calls.back() == false);
    f.w(kData, 1);
    CHECK(f.calls.back() == true);
    f.w(kIntEnable, 0);
    CHECK(f.calls.back() == false);
    f.w(kIntEnable, 0x40);
    CHECK(f.calls.back() == false);
    f.feed(1);
    f.uart.tick(10);
    CHECK(f.calls.back() == false);
    f.clock.now = 10;
    f.uart.tick(1'000'010);
    CHECK(f.calls.back() == true);
    f.w(kIntStatus, 0x40);
    CHECK(f.calls.back() == false);
}

TEST_CASE("rx_stop_end latches 1 ms after last byte") {
    Fixture f;
    f.w(kIntEnable, 0x40);
    CHECK_FALSE(f.uart.next_event_ns().has_value());
    f.feed(1);
    f.uart.tick(1000);
    CHECK(f.uart.next_event_ns() == std::optional<uint64_t>(1'001'000));
    f.uart.tick(1'000'999);
    CHECK(f.r(kIntStatus) == 0);
    CHECK(f.calls.back() == false);
    f.uart.tick(1'001'000);
    CHECK(f.r(kIntStatus) == 0x40);
    CHECK(f.calls.back() == true);
    CHECK_FALSE(f.uart.next_event_ns().has_value());
    // stays latched until written 1
    f.uart.tick(5'000'000);
    CHECK(f.r(kIntStatus) == 0x40);
    f.w(kIntStatus, 0x40);
    CHECK(f.r(kIntStatus) == 0);
}

TEST_CASE("rx_stop_end is re-armed by each move and by pops") {
    Fixture f;
    f.feed(1);
    f.uart.tick(1000);
    f.feed(1);
    f.uart.tick(500'000);
    CHECK(f.uart.next_event_ns() == std::optional<uint64_t>(1'500'000));

    Fixture g;
    g.feed(129, 0);
    g.uart.tick(100);
    CHECK(g.uart.next_event_ns() == std::optional<uint64_t>(1'000'100));
    g.uart.tick(900'000); // nothing moves: backlog blocked by full FIFO
    CHECK(g.uart.next_event_ns() == std::optional<uint64_t>(1'000'100));
    g.r(kData); // pop pulls the last backlog byte, armed from last tick time
    CHECK(g.uart.next_event_ns() == std::optional<uint64_t>(1'900'000));
}

TEST_CASE("reset keeps endpoint") {
    Fixture f;
    f.w(0x1C, 0x1234);
    CHECK(f.r(0x1C) == 0x1234);
    f.w(kFifoConfig, 5u << 8);
    f.w(kIntEnable, 0x61);
    f.w(kData, 1);
    f.feed(300, 0);
    f.uart.tick(50);
    f.uart.reset();
    CHECK(f.calls.back() == false);
    CHECK(f.r(0x1C) == 0);
    CHECK(f.r(kFifoConfig) == 0);
    CHECK(f.r(kIntEnable) == 0);
    CHECK(f.r(kIntStatus) == 0);
    CHECK(f.r(kFifoStatus) == ((1u << 17) | (1u << 19) | (1u << 20)));
    CHECK_FALSE(f.uart.next_event_ns().has_value());
    f.w(kData, 0x77);
    f.uart.tick(1'000'000);
    CHECK(f.ep.sent.back() == 0x77);
    f.feed(1, 0x33);
    f.uart.tick(10);
    CHECK(f.r(kData) == 0x3300);
    CHECK(f.r(kData) == 0); // backlog was cleared
}
