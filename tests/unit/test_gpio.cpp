#include <doctest/doctest.h>

#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "periph/gpio.h"

using namespace bk;

namespace {

constexpr uint32_t kOutput = 1u << 1, kOutputOff = 1u << 3, kPullUp = 1u << 4, kPullEn = 1u << 5, kFunc = 1u << 6;
constexpr uint32_t kIntEnable = 0x84, kLevel0 = 0x88, kLevel1 = 0x8C, kStatus = 0x90, kExtra = 0x98, kDetect = 0x9C,
                   kEnable2 = 0xE0, kLevel3 = 0xE4, kStatus2 = 0xE8;
constexpr unsigned kLow = 0, kHigh = 1, kRising = 2, kFalling = 3;

uint32_t pin_off(unsigned pin) { return pin < 32 ? pin * 4 : 0xC0 + (pin - 32) * 4; }

struct Fixture {
    std::vector<bool>                      irq;
    std::vector<std::pair<unsigned, bool>> changes;
    Gpio                                   gpio;

    explicit Fixture(uint32_t pin_reset = 0x28, std::map<uint32_t, uint32_t> words = {})
        : gpio("gpio", [this](bool v) { irq.push_back(v); }, pin_reset, std::move(words)) {
        gpio.watch([this](unsigned p, bool l) { changes.emplace_back(p, l); });
    }

    void     w(uint32_t off, uint32_t v) { gpio.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return gpio.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
    // Sets the mode (0-3) of `pin` in the right level register, keeping the others.
    void set_mode(unsigned pin, unsigned mode) {
        uint32_t reg   = pin < 16 ? kLevel0 : pin < 32 ? kLevel1 : kLevel3;
        unsigned index = pin < 16 ? pin : pin < 32 ? pin - 16 : pin - 32;
        w(reg, (r(reg) & ~(3u << index * 2)) | mode << index * 2);
    }
    void enable(unsigned pin) {
        if (pin < 32)
            w(kIntEnable, r(kIntEnable) | 1u << pin);
        else
            w(kEnable2, r(kEnable2) | 1u << (pin - 32));
    }
};

} // namespace

TEST_CASE("gpio has one range of 0x100 bytes") {
    Fixture f;
    REQUIRE(f.gpio.ranges().size() == 1);
    CHECK(f.gpio.ranges()[0].name == "regs");
    CHECK(f.gpio.ranges()[0].size == 0x100);
}

TEST_CASE("gpio pin words are at p*4 and 0xC0 + (p-32)*4 and reset to pin_reset") {
    Fixture f;
    for (unsigned p = 0; p < 40; ++p) CHECK_MESSAGE(f.r(pin_off(p)) == 0x28, p);
    // The words between the two banks are plain words, not pins.
    CHECK(f.r(0xA0) == 0);
    CHECK(f.r(0xBC) == 0);
    CHECK(f.r(0xE0) == 0);
}

TEST_CASE("gpio reset_words overlay pin words and set plain words") {
    Fixture f(0x28,
              {
                  {0x50, 0x68  },
                  {0x9C, 0x02  },
                  {0xB8, 0x5500}
    });
    CHECK(f.r(0x50) == 0x68);
    CHECK(f.r(0x4C) == 0x28);
    CHECK(f.r(0x9C) == 0x02);
    CHECK(f.r(0xB8) == 0x5500);
}

TEST_CASE("gpio pin word writes keep bits 1-6 and read bit 0 as the pad") {
    Fixture f;
    f.w(pin_off(3), 0xFFFFFFFF);
    // Pull enabled, pull up, driver off: pad high. Bits 1-6 read back, bit 0 is the pad.
    CHECK(f.r(pin_off(3)) == (0x7E | 1));
    f.w(pin_off(3), 0x01);       // bit 0 is not writable
    CHECK(f.r(pin_off(3)) == 0); // driver on, output 0
    f.w(pin_off(3), kOutput);
    CHECK(f.r(pin_off(3)) == (kOutput | 1)); // driver on, output 1: pad high
}

TEST_CASE("gpio pad level rules") {
    Fixture  f;
    unsigned p = 5;
    // Driver on (bit 3 clear), no function: the output level.
    f.w(pin_off(p), kOutput);
    CHECK(f.gpio.pad_level(p));
    f.w(pin_off(p), 0);
    CHECK_FALSE(f.gpio.pad_level(p));
    // The driver wins over a forced level.
    f.gpio.set_input(p, true);
    CHECK_FALSE(f.gpio.pad_level(p));
    // Driver off: the forced level, then the pull, then 0.
    f.w(pin_off(p), kOutputOff);
    CHECK(f.gpio.pad_level(p));
    f.gpio.set_input(p, std::nullopt);
    CHECK_FALSE(f.gpio.pad_level(p));
    f.w(pin_off(p), kOutputOff | kPullEn | kPullUp);
    CHECK(f.gpio.pad_level(p));
    f.w(pin_off(p), kOutputOff | kPullEn);
    CHECK_FALSE(f.gpio.pad_level(p));
    f.w(pin_off(p), kOutputOff | kPullUp); // pull mode without pull enable: nothing
    CHECK_FALSE(f.gpio.pad_level(p));
    // A pin with the function bit set is not driven even with the driver on.
    f.w(pin_off(p), kOutput | kFunc);
    CHECK_FALSE(f.gpio.pad_level(p));
    f.gpio.set_input(p, true);
    CHECK(f.gpio.pad_level(p));
}

TEST_CASE("gpio watchers see every pad change, not repeats") {
    Fixture f;
    f.gpio.set_input(1, true);
    f.gpio.set_input(1, true);
    f.gpio.set_input(1, false);
    f.w(pin_off(2), kOutput);     // driver on, output 1
    f.w(pin_off(2), kOutput | 0); // same
    f.w(pin_off(2), 0);
    CHECK(f.changes == std::vector<std::pair<unsigned, bool>>{
                           {1, true },
                           {1, false},
                           {2, true },
                           {2, false}
    });
}

TEST_CASE("gpio pins outside 0-39 are an error") {
    Fixture f;
    CHECK_THROWS_WITH_AS(f.gpio.set_input(40, true), "GPIO pin 40 out of range (0-39)", std::invalid_argument);
    CHECK_THROWS_AS(f.gpio.pad_level(100), std::invalid_argument);
    CHECK_NOTHROW(f.gpio.set_input(39, true));
}

TEST_CASE("gpio rising edge latches until acknowledged, falling edge likewise") {
    Fixture f;
    f.set_mode(4, kRising); // before enabling: the default mode is level low and the pad is low
    f.enable(4);
    CHECK_FALSE(f.line());
    f.gpio.set_input(4, true);
    CHECK(f.line());
    CHECK(f.r(kStatus) == 1u << 4);
    f.gpio.set_input(4, false); // the falling edge is not wanted: still pending from before
    CHECK(f.line());
    f.w(kStatus, 1u << 4);
    CHECK_FALSE(f.line());
    CHECK(f.r(kStatus) == 0);

    f.set_mode(4, kFalling);
    f.gpio.set_input(4, true);
    CHECK_FALSE(f.line());
    f.gpio.set_input(4, false);
    CHECK(f.line());
    f.w(kStatus, 0xFFFFFFFF);
    CHECK_FALSE(f.line());
}

TEST_CASE("gpio edge does not latch while the pin is disabled, and enabling does not latch it") {
    Fixture f;
    f.set_mode(4, kRising);
    f.gpio.set_input(4, true);
    f.enable(4);
    CHECK_FALSE(f.line());
    CHECK(f.r(kStatus) == 0);
}

TEST_CASE("gpio level mode latches again at once after an acknowledge while the level holds") {
    Fixture f;
    f.set_mode(7, kHigh); // before enabling: the default mode would latch the low pad at once
    f.enable(7);
    CHECK_FALSE(f.line());
    f.gpio.set_input(7, true);
    CHECK(f.line());
    f.w(kStatus, 1u << 7);
    CHECK(f.line()); // still high
    CHECK(f.r(kStatus) == 1u << 7);
    f.gpio.set_input(7, std::nullopt); // pull-down: the level goes away, but the latch stays
    CHECK(f.line());
    f.w(kStatus, 1u << 7);
    CHECK_FALSE(f.line());
}

TEST_CASE("gpio low level mode: an unforced pulled-down pin is pending as soon as it is enabled") {
    Fixture f;
    f.set_mode(9, kLow);
    f.enable(9);
    CHECK(f.line());
    CHECK(f.r(kStatus) == 1u << 9);
}

TEST_CASE("gpio changing the mode or enable re-runs the level check") {
    Fixture f;
    f.enable(2);
    // The mode defaults to 0 = level low and the pad is low, so enabling latched it.
    CHECK(f.line());
    CHECK(f.r(kStatus) == 1u << 2);
    f.w(kStatus, 1u << 2);
    CHECK(f.line()); // still low, latched again at once
    f.set_mode(2, kHigh);
    f.w(kStatus, 1u << 2);
    CHECK_FALSE(f.line());
}

TEST_CASE("gpio mode registers: pins 0-15, 16-31 and 32-39") {
    Fixture f;
    f.set_mode(20, kRising);
    f.enable(20);
    CHECK(f.r(kLevel1) == kRising << 8);
    f.gpio.set_input(20, true);
    CHECK(f.line());
    CHECK(f.r(kStatus) == 1u << 20);

    f.set_mode(35, kRising);
    f.enable(35);
    CHECK(f.r(kLevel3) == kRising << 6);
    f.gpio.set_input(35, true);
    CHECK(f.r(kStatus2) == 1u << 3);
    CHECK(f.r(kStatus) == 1u << 20); // bank 1 does not show up here
    f.w(kStatus2, 1u << 3);
    CHECK(f.r(kStatus2) == 0);
    CHECK(f.r(kStatus) == 1u << 20);
    f.w(kStatus, 1u << 20);
    CHECK_FALSE(f.line());
}

TEST_CASE("gpio config write that changes the pad is an edge too") {
    Fixture f;
    f.set_mode(6, kRising);
    f.enable(6);
    f.w(pin_off(6), kOutput); // driver on, output high: pad rises
    CHECK(f.line());
}

TEST_CASE("gpio interrupt line follows pending state") {
    Fixture f;
    f.set_mode(1, kRising);
    f.enable(1);
    f.gpio.set_input(1, true);
    CHECK(f.line());
    f.w(kIntEnable, 0); // disabling hides it
    CHECK_FALSE(f.line());
    f.w(kIntEnable, 2); // still latched
    CHECK(f.line());
}

TEST_CASE("gpio extra_int_cfg keeps bits 0-1 and 4-5; detect is read-only") {
    Fixture f(0x28,
              {
                  {0x9C, 2}
    });
    f.w(kExtra, 0xFFFFFFFF);
    CHECK(f.r(kExtra) == 0x33);
    f.w(kDetect, 0xFFFF);
    CHECK(f.r(kDetect) == 2);
}

TEST_CASE("gpio other offsets are plain words") {
    Fixture f;
    f.w(0xB8, 0xDEADBEEF);
    CHECK(f.r(0xB8) == 0xDEADBEEF);
    CHECK(f.r(0xFC) == 0);
    // Lookup is by exact offset.
    CHECK(f.r(0xB9) == 0);
}

TEST_CASE("gpio reset restores words, clears latches, keeps forced levels") {
    Fixture f(0x28,
              {
                  {0x50, 0x68}
    });
    f.set_mode(4, kRising);
    f.enable(4);
    f.gpio.set_input(4, true);
    f.w(pin_off(4), 0x28); // driver off: the forced level shows
    f.w(0xB8, 5);
    REQUIRE(f.line());
    f.gpio.reset();
    CHECK_FALSE(f.line());
    CHECK(f.r(0xB8) == 0);
    CHECK(f.r(kStatus) == 0);
    CHECK(f.r(kIntEnable) == 0);
    CHECK(f.r(0x50) == 0x68);
    // Forced level survived: pin 4 is back at the reset word (driver off) and reads the forced level.
    CHECK(f.gpio.pad_level(4));
    CHECK(f.r(pin_off(4)) == (0x28 | 1));
    // A reset reports no edge.
    CHECK(f.changes.size() == 1);
}

TEST_CASE("gpio works without an interrupt line") {
    Gpio g("gpio", {}, 0x28, {});
    g.write("regs", kIntEnable, 4, 1);
    g.write("regs", 0x00, 4, kOutput);
    CHECK(g.pad_level(0));
}
