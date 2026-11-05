#include <doctest/doctest.h>

#include <optional>
#include <vector>

#include "periph/pwm.h"

using namespace bk;

namespace {

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

struct Fixture {
    FakeClock         clock;
    std::vector<bool> calls;
    Pwm               pwm;

    explicit Fixture(uint32_t hz = 26000000) : pwm("pwm", clock, [this](bool v) { calls.push_back(v); }, hz) {}

    void     w(uint32_t off, uint32_t v) { pwm.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return pwm.read("regs", off, 4); }
};

// Control bits of channel `ch`: enable, interrupt enable and `mode`.
constexpr uint32_t on(unsigned ch, uint32_t mode = 1, bool irq = true) {
    return (1u | (irq ? 2u : 0u) | mode << 2) << (4 * ch);
}

// End value, duty cycle and capture register of channel `ch`.
constexpr uint32_t end_reg(unsigned ch) { return 0x08 + 0x0C * ch; }
constexpr uint32_t duty_reg(unsigned ch) { return 0x0C + 0x0C * ch; }
constexpr uint32_t capture_reg(unsigned ch) { return 0x10 + 0x0C * ch; }

} // namespace

TEST_CASE("pwm registers read back; the capture registers read 0") {
    Fixture f;
    CHECK(f.pwm.name() == "pwm");
    CHECK(f.pwm.ranges().at(0).name == "regs");
    CHECK(f.pwm.ranges().at(0).size == 0x50);
    f.w(0x00, 0xFF123456);
    CHECK(f.r(0x00) == 0x00123456);
    f.w(end_reg(5), 0xCB20);
    f.w(duty_reg(5), 0x1234);
    f.w(capture_reg(5), 0x55);
    CHECK(f.r(end_reg(5)) == 0xCB20);
    CHECK(f.r(duty_reg(5)) == 0x1234);
    CHECK(f.r(capture_reg(5)) == 0);
    CHECK(f.r(0x4C) == 0);
}

TEST_CASE("pwm channel runs once enabled with a non-zero end value, in either order") {
    // The vendor bootloader enables channel 0 first, then writes its end value.
    Fixture f;
    f.clock.now = 500;
    f.w(0x00, on(0));
    CHECK_FALSE(f.pwm.next_event_ns().has_value());
    f.w(end_reg(0), 52000); // 2 ms at 26 MHz
    CHECK(f.pwm.next_event_ns() == std::optional<uint64_t>(2'000'500));

    Fixture g;
    g.w(end_reg(2), 26);
    g.w(0x00, on(2, 0)); // PWM mode counts periods too
    CHECK(g.pwm.next_event_ns() == std::optional<uint64_t>(1000));
}

TEST_CASE("pwm period end sets the status bit and the line; write 1 clears it") {
    Fixture f;
    f.w(end_reg(0), 26);
    f.w(0x00, on(0));
    f.pwm.tick(999);
    CHECK(f.r(0x04) == 0);
    CHECK(f.calls.back() == false);
    f.pwm.tick(1000);
    CHECK(f.r(0x04) == 1);
    CHECK(f.calls.back() == true);
    CHECK(f.pwm.next_event_ns() == std::optional<uint64_t>(2000));
    // Late ticks skip the missed periods.
    f.pwm.tick(4500);
    CHECK(f.pwm.next_event_ns() == std::optional<uint64_t>(5000));
    f.w(0x04, 1);
    CHECK(f.r(0x04) == 0);
    CHECK(f.calls.back() == false);
}

TEST_CASE("pwm channel 3 raises status bit 3") {
    Fixture f;
    f.w(end_reg(3), 26);
    f.w(0x00, on(3));
    f.pwm.tick(1000);
    CHECK(f.r(0x04) == (1u << 3));
}

TEST_CASE("pwm without its interrupt enabled counts but sets no status") {
    Fixture f;
    f.w(end_reg(1), 26);
    f.w(0x00, on(1, 1, false));
    CHECK(f.pwm.next_event_ns() == std::optional<uint64_t>(1000));
    f.pwm.tick(1000);
    CHECK(f.r(0x04) == 0);
    CHECK(f.calls.back() == false);
}

TEST_CASE("pwm turning the interrupt off clears the status bit") {
    Fixture f;
    f.w(end_reg(0), 26);
    f.w(0x00, on(0));
    f.pwm.tick(1000);
    REQUIRE(f.r(0x04) == 1);
    f.w(0x00, on(0, 1, false));
    CHECK(f.r(0x04) == 0);
    CHECK(f.calls.back() == false);
}

TEST_CASE("pwm capture modes and a disabled channel never end a period") {
    Fixture f;
    f.w(end_reg(0), 26);
    f.w(0x00, on(0, 2) | on(1, 3));
    f.w(end_reg(1), 26);
    CHECK_FALSE(f.pwm.next_event_ns().has_value());
    f.w(0x00, on(0));
    REQUIRE(f.pwm.next_event_ns().has_value());
    f.w(0x00, 0);
    CHECK_FALSE(f.pwm.next_event_ns().has_value());
    f.w(0x00, on(0));
    f.w(end_reg(0), 0);
    CHECK_FALSE(f.pwm.next_event_ns().has_value());
}

TEST_CASE("pwm clock is hz") {
    Fixture f(32000);
    f.w(end_reg(0), 32);
    f.w(0x00, on(0));
    CHECK(f.pwm.next_event_ns() == std::optional<uint64_t>(1'000'000));
}

TEST_CASE("pwm reset clears everything and lowers the line") {
    Fixture f;
    f.w(end_reg(0), 26);
    f.w(duty_reg(0), 3);
    f.w(0x00, on(0));
    f.pwm.tick(1000);
    f.pwm.reset();
    CHECK(f.r(0x00) == 0);
    CHECK(f.r(0x04) == 0);
    CHECK(f.r(end_reg(0)) == 0);
    CHECK(f.r(duty_reg(0)) == 0);
    CHECK_FALSE(f.pwm.next_event_ns().has_value());
    CHECK(f.calls.back() == false);
}
