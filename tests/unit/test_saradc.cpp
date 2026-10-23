#include <doctest/doctest.h>

#include <optional>
#include <vector>

#include "periph/saradc.h"

using namespace bk;

namespace {

constexpr uint32_t kConfig = 0x00, kData = 0x04, kCtrlCfg = 0x08, kSaturation = 0x0C, kDatAfterSta = 0x10;
constexpr uint32_t kChnlEn = 1u << 2, kIntClr = 1u << 8, kBusy = 1u << 29, kEmpty = 1u << 30, kFull = 1u << 31;
constexpr uint64_t kSample = 100'000;

struct Fixture {
    std::vector<bool> irq;
    Saradc            adc;

    Fixture() : adc("adc", [this](bool v) { irq.push_back(v); }, 330) {}

    void     w(uint32_t off, uint32_t v) { adc.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return adc.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
};

} // namespace

TEST_CASE("saradc has one range of 0x20 bytes and resets to an empty FIFO") {
    Fixture f;
    REQUIRE(f.adc.ranges().size() == 1);
    CHECK(f.adc.ranges()[0].name == "regs");
    CHECK(f.adc.ranges()[0].size == 0x20);
    CHECK(f.r(kConfig) == kEmpty);
    CHECK(f.r(kCtrlCfg) == 0xE0);
    CHECK(f.r(kSaturation) == 0);
    CHECK_FALSE(f.adc.next_event_ns().has_value());
}

TEST_CASE("saradc config write drops status bits and int_clr, read adds the FIFO flags") {
    Fixture f;
    f.w(kConfig, 0xFFFFFFFF & ~kChnlEn);
    CHECK(f.r(kConfig) == (0x1FFFFFFFu & ~kChnlEn & ~kIntClr) + 0 + kEmpty);
    CHECK((f.r(kConfig) & kBusy) == 0);
}

TEST_CASE("saradc enabling starts conversion 100 us after the last tick") {
    Fixture f;
    f.adc.tick(5000);
    f.w(kConfig, kChnlEn);
    CHECK(f.adc.next_event_ns() == std::optional<uint64_t>(5000 + kSample));
}

TEST_CASE("saradc samples arrive every 100 us and the interrupt rises at the threshold") {
    Fixture f;
    f.w(kCtrlCfg, 3);
    f.w(kConfig, kChnlEn);
    f.adc.tick(kSample);
    f.adc.tick(2 * kSample);
    CHECK_FALSE(f.line());
    CHECK((f.r(kConfig) & kEmpty) == 0);
    f.adc.tick(3 * kSample);
    CHECK(f.line());
    CHECK(f.adc.next_event_ns() == std::optional<uint64_t>(4 * kSample));
}

TEST_CASE("saradc a late tick adds every sample that is due") {
    Fixture f;
    f.w(kCtrlCfg, 0); // threshold is at least 1
    f.w(kConfig, kChnlEn);
    f.adc.tick(3 * kSample + 1);
    CHECK(f.line());
    CHECK(f.r(kData) == 330);
    CHECK(f.r(kData) == 330);
    CHECK(f.r(kData) == 330);
    CHECK(f.r(kData) == 0); // empty
    CHECK(f.adc.next_event_ns() == std::optional<uint64_t>(4 * kSample));
}

TEST_CASE("saradc dat_after_sta returns 0xFFFF when empty; popping keeps the flag") {
    Fixture f;
    CHECK(f.r(kDatAfterSta) == 0xFFFF);
    CHECK(f.r(kData) == 0);
    f.w(kConfig, kChnlEn);
    f.adc.tick(kSample);
    REQUIRE(f.line());
    CHECK(f.r(kDatAfterSta) == 330);
    CHECK(f.line());
    CHECK(f.r(kDatAfterSta) == 0xFFFF);
}

TEST_CASE("saradc int_clr clears the flag and reads back 0") {
    Fixture f;
    f.w(kConfig, kChnlEn);
    f.adc.tick(kSample);
    REQUIRE(f.line());
    f.w(kConfig, kChnlEn | kIntClr);
    CHECK_FALSE(f.line());
    CHECK((f.r(kConfig) & kIntClr) == 0);
    CHECK((f.r(kConfig) & kChnlEn) != 0); // conversion keeps running
    f.adc.tick(2 * kSample);
    CHECK(f.line()); // the next sample raises it again
}

TEST_CASE("saradc FIFO holds 32 samples") {
    Fixture f;
    f.w(kCtrlCfg, 0x1F);
    f.w(kConfig, kChnlEn);
    f.adc.tick(30 * kSample);
    CHECK_FALSE(f.line());
    f.adc.tick(31 * kSample); // the threshold is 31 samples
    CHECK(f.line());
    CHECK((f.r(kConfig) & kFull) == 0);
    f.adc.tick(32 * kSample);
    CHECK((f.r(kConfig) & kFull) != 0);
    f.adc.tick(40 * kSample); // more samples are dropped
    int n = 0;
    while (f.r(kData) == 330) ++n;
    CHECK(n == 32);
}

TEST_CASE("saradc clearing chnl_en stops conversion and empties the FIFO") {
    Fixture f;
    f.w(kConfig, kChnlEn);
    f.adc.tick(2 * kSample);
    f.w(kConfig, 0);
    CHECK_FALSE(f.adc.next_event_ns().has_value());
    CHECK((f.r(kConfig) & kEmpty) != 0);
    f.adc.tick(10 * kSample);
    CHECK((f.r(kConfig) & kEmpty) != 0);
    // Writing chnl_en again while it is already set does not restart the timer.
    f.adc.tick(11 * kSample);
    f.w(kConfig, kChnlEn);
    uint64_t first = *f.adc.next_event_ns();
    f.w(kConfig, kChnlEn);
    CHECK(f.adc.next_event_ns() == std::optional<uint64_t>(first));
}

TEST_CASE("saradc ctrl_cfg and saturation_cfg are plain words") {
    Fixture f;
    f.w(kCtrlCfg, 0x12345678);
    f.w(kSaturation, 0x9ABCDEF0);
    CHECK(f.r(kCtrlCfg) == 0x12345678);
    CHECK(f.r(kSaturation) == 0x9ABCDEF0);
    CHECK(f.r(0x14) == 0);
}

TEST_CASE("saradc reset stops everything") {
    Fixture f;
    f.w(kCtrlCfg, 5);
    f.w(kConfig, kChnlEn);
    f.adc.tick(kSample);
    f.adc.reset();
    CHECK_FALSE(f.line());
    CHECK(f.r(kConfig) == kEmpty);
    CHECK(f.r(kCtrlCfg) == 0xE0);
    CHECK_FALSE(f.adc.next_event_ns().has_value());
    // The last-tick time is 0 again.
    f.w(kConfig, kChnlEn);
    CHECK(f.adc.next_event_ns() == std::optional<uint64_t>(kSample));
}

TEST_CASE("saradc works without an interrupt line and with another sample value") {
    Saradc adc("adc", {}, 1023);
    adc.write("regs", kConfig, 4, kChnlEn);
    adc.tick(kSample);
    CHECK(adc.read("regs", kData, 4) == 1023);
}
