#include <doctest/doctest.h>

#include <map>
#include <stdexcept>
#include <string>

#include "periph/sctrl.h"

using namespace bk;

namespace {

Sctrl make(std::map<uint32_t, uint32_t>      cold    = {},
           std::map<uint32_t, uint32_t>      warm    = {},
           std::map<std::string, SignalSpec> signals = {},
           std::vector<ReadAdjust>           adjust  = {}) {
    return Sctrl(
        "sctrl", 0x0007221a, 0x18221020, std::move(cold), std::move(warm), std::move(signals), std::move(adjust));
}

uint32_t r(Sctrl &s, uint32_t off) { return s.read("regs", off, 4); }
void     w(Sctrl &s, uint32_t off, uint32_t v) { s.write("regs", off, 4, v); }

} // namespace

TEST_CASE("sctrl has one range of 0x200 bytes") {
    Sctrl s = make();
    REQUIRE(s.ranges().size() == 1);
    CHECK(s.ranges()[0].name == "regs");
    CHECK(s.ranges()[0].size == 0x200);
}

TEST_CASE("sctrl identity words ignore writes") {
    Sctrl s = make();
    CHECK(r(s, 0) == 0x0007221a);
    CHECK(r(s, 4) == 0x18221020);
    w(s, 0, 1);
    w(s, 4, 2);
    CHECK(r(s, 0) == 0x0007221a);
    CHECK(r(s, 4) == 0x18221020);
}

TEST_CASE("sctrl words read back what was written, unknown words read 0") {
    Sctrl s = make({
        {0x0c, 0x1ff}
    });
    CHECK(r(s, 0x0c) == 0x1ff);
    CHECK(r(s, 0x10) == 0);
    w(s, 0x10, 0xDEADBEEF);
    CHECK(r(s, 0x10) == 0xDEADBEEF);
    // Registers are found by exact offset only.
    CHECK(r(s, 0x11) == 0);
    CHECK(r(s, 0x1ff) == 0);
}

TEST_CASE("sctrl starts cold and a reset overlays the warm words") {
    Sctrl s = make(
        {
            {0x84, 0xffe08410},
            {0x18, 5         }
    },
        {{0x84, 0xffe08010}, {0x20, 7}});
    CHECK(r(s, 0x84) == 0xffe08410);
    CHECK(r(s, 0x20) == 0);
    w(s, 0x18, 99);
    w(s, 0x30, 1);
    s.reset();
    CHECK(r(s, 0x84) == 0xffe08010);
    CHECK(r(s, 0x20) == 7);
    CHECK(r(s, 0x18) == 5);
    CHECK(r(s, 0x30) == 0);
}

TEST_CASE("sctrl read_adjust biases a field on read and clamps it") {
    Sctrl s = make(
        {
            {0x08, 0x00020000}
    },
        {},
        {},
        {{0x08, 4, 4, 1}});
    CHECK(r(s, 0x08) == 0x00020010);
    w(s, 0x08, 0x000200E5);
    CHECK(r(s, 0x08) == 0x000200F5);
    // Clamped to the field maximum; the other bits are untouched.
    w(s, 0x08, 0x000200F5);
    CHECK(r(s, 0x08) == 0x000200F5);
    // Other offsets are not affected.
    w(s, 0x0c, 0x30);
    CHECK(r(s, 0x0c) == 0x30);
}

TEST_CASE("sctrl read-modify-write stores the bias back (divider drifts up)") {
    Sctrl s = make(
        {
            {0x08, 0x00020000}
    },
        {},
        {},
        {{0x08, 4, 4, 1}});
    for (int i = 0; i < 20; ++i) w(s, 0x08, r(s, 0x08));
    CHECK(r(s, 0x08) == 0x000200F0);
}

TEST_CASE("sctrl read_adjust with add larger than the field saturates") {
    Sctrl s = make(
        {
            {0x08, 0x0000000A}
    },
        {},
        {},
        {{0x08, 0, 4, 100}});
    CHECK(r(s, 0x08) == 0x0000000F);
    Sctrl full = make(
        {
            {0x08, 0xFFFFFFFF}
    },
        {},
        {},
        {{0x08, 0, 32, 5}});
    CHECK(r(full, 0x08) == 0xFFFFFFFF);
}

TEST_CASE("sctrl signal reads the register field at the time of the question") {
    Sctrl s = make(
        {
    },
        {},
        {{"mac_powered", SignalSpec{0x10c, 16, 16, 1}}, {"bit", SignalSpec{0x20, 3, 1, 1}}});
    Signal mac = s.signal("mac_powered");
    Signal bit = s.signal("bit");
    CHECK_FALSE(mac());
    w(s, 0x10c, 0x00010000);
    CHECK(mac());
    w(s, 0x10c, 0xD8020000);
    CHECK_FALSE(mac());
    w(s, 0x10c, 0x0001FFFF);
    CHECK(mac());
    CHECK_FALSE(bit());
    w(s, 0x20, 8);
    CHECK(bit());
}

TEST_CASE("sctrl signal uses the biased read") {
    Sctrl s = make(
        {
            {0x08, 0x00}
    },
        {},
        {{"div1", SignalSpec{0x08, 4, 4, 1}}},
        {{0x08, 4, 4, 1}});
    Signal div = s.signal("div1");
    CHECK(div()); // stored divider 0 reads as 1
}

TEST_CASE("sctrl: the deep sleep word in the sleep register is noted until reset") {
    Sctrl s = make();
    CHECK_FALSE(s.deep_sleep());
    // The other sleep modes and other registers do not count.
    w(s, 0x104, 0x4F89);
    w(s, 0x104, 0xB706);
    w(s, 0x100, 0xADC1);
    w(s, 0x108, 0xADC1);
    CHECK_FALSE(s.deep_sleep());
    // Bits above the mode field are the firmware's own.
    w(s, 0x104, 0x00A6ADC1);
    CHECK(s.deep_sleep());
    CHECK(r(s, 0x104) == 0x00A6ADC1);
    s.reset();
    CHECK_FALSE(s.deep_sleep());
}

TEST_CASE("sctrl unknown signal is refused") {
    Sctrl s = make();
    CHECK_THROWS_WITH_AS(s.signal("nope"), "sctrl: no signal nope", std::invalid_argument);
}
