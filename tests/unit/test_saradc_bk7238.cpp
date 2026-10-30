#include <doctest/doctest.h>

#include <optional>
#include <vector>

#include "periph/saradc_bk7238.h"

using namespace bk;

namespace {

constexpr uint32_t kConfig0 = 0x00, kConfig2 = 0x08, kState = 0x0C, kData = 0x10;
constexpr uint32_t kStart     = 1u << 0;
constexpr uint32_t kFifoIntEn = 1u << 3, kFifoInt = 1u << 4, kConvIntEn = 1u << 14, kConvDone = 1u << 15,
                   kTimeoutIntEn = 1u << 16, kEmpty = 1u << 30, kFull = 1u << 31;
constexpr uint32_t kChnlEn = kFifoIntEn | kConvIntEn | kTimeoutIntEn;
constexpr uint64_t kSample = 100'000;

struct Fixture {
    std::vector<bool> irq;
    SaradcBk7238      adc;

    Fixture() : adc("adc", [this](bool v) { irq.push_back(v); }, 3200) {}

    void     w(uint32_t off, uint32_t v) { adc.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return adc.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
    void     start() {
        w(kConfig0, kStart);
        w(kState, kChnlEn);
    }
};

} // namespace

TEST_CASE("saradc_bk7238 has one range of 0x1C bytes and resets to an empty FIFO") {
    Fixture f;
    REQUIRE(f.adc.ranges().size() == 1);
    CHECK(f.adc.ranges()[0].name == "regs");
    CHECK(f.adc.ranges()[0].size == 0x1C);
    CHECK(f.r(kState) == kEmpty);
    CHECK(f.r(kData) == 0);
    CHECK_FALSE(f.adc.next_event_ns().has_value());
}

TEST_CASE("saradc_bk7238 config registers read back what was written") {
    Fixture f;
    f.w(kConfig2, 0x12345678);
    CHECK(f.r(kConfig2) == 0x12345678);
}

TEST_CASE("saradc_bk7238 state keeps the enables and drops the read-only bits") {
    Fixture f;
    f.w(kState, 0xFFFFFFFF);
    CHECK(f.r(kState) == ((0xFFFFFFFFu & ~0x7u & ~kFifoInt & ~kConvDone & ~(1u << 17) & ~kFull)));
}

TEST_CASE("saradc_bk7238 samples only while started and an interrupt is enabled") {
    Fixture f;
    f.w(kConfig0, kStart);
    CHECK_FALSE(f.adc.next_event_ns().has_value());
    f.adc.tick(5000);
    f.w(kState, kConvIntEn);
    CHECK(f.adc.next_event_ns() == std::optional<uint64_t>(5000 + kSample));
    f.w(kState, 0);
    CHECK_FALSE(f.adc.next_event_ns().has_value());
}

TEST_CASE("saradc_bk7238 a sample fills the FIFO, sets the flags and raises the interrupt") {
    Fixture f;
    f.start();
    f.adc.tick(kSample);
    CHECK(f.line());
    CHECK(f.r(kState) == (kChnlEn | kFifoInt | kConvDone | 1));
    CHECK(f.r(kData) == 3200);
    CHECK((f.r(kState) & kEmpty) != 0);
}

TEST_CASE("saradc_bk7238 writing a flag as 1 clears it and lowers the interrupt") {
    Fixture f;
    f.start();
    f.adc.tick(kSample);
    f.r(kData);
    f.w(kState, f.r(kState) | kFifoInt | kConvDone);
    CHECK((f.r(kState) & (kFifoInt | kConvDone)) == 0);
    CHECK_FALSE(f.line());
}

TEST_CASE("saradc_bk7238 the FIFO holds 8 samples, the level field saturates at 7") {
    Fixture f;
    f.start();
    f.adc.tick(20 * kSample);
    CHECK((f.r(kState) & 0x7) == 7);
    CHECK((f.r(kState) & kFull) != 0);
    for (int i = 0; i < 8; ++i) CHECK(f.r(kData) == 3200);
    CHECK((f.r(kState) & kEmpty) != 0);
}

TEST_CASE("saradc_bk7238 stopping keeps the samples for the driver to drain") {
    Fixture f;
    f.start();
    f.adc.tick(2 * kSample);
    f.w(kState, 0);
    CHECK((f.r(kState) & 0x7) == 2);
    CHECK_FALSE(f.adc.next_event_ns().has_value());
}

TEST_CASE("saradc_bk7238 reset empties the FIFO and lowers the line") {
    Fixture f;
    f.start();
    f.adc.tick(kSample);
    f.adc.reset();
    CHECK(f.r(kState) == kEmpty);
    CHECK_FALSE(f.line());
    CHECK_FALSE(f.adc.next_event_ns().has_value());
}
