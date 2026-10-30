#include <doctest/doctest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "gdma_fakes.h"
#include "periph/gdma_bk7252n.h"

using namespace bk;
using namespace bk::gdma_test;

namespace {

constexpr uint32_t kControl = 0x08, kIntStatus = 0x18;
constexpr uint32_t kFin = 1u << 18, kHalf = 1u << 19;
constexpr uint32_t ch(unsigned n) { return 0x40 + n * 0x40; }

struct Fixture {
    std::ostringstream out;
    FakeClock          clock;
    FakeMemory         mem;
    std::vector<bool>  irq;
    GdmaBk7252n        dma;

    Fixture() : dma("gdma", clock, [this](bool v) { irq.push_back(v); }, mem, 120'000'000) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    void     w(uint32_t off, uint32_t v) { dma.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return dma.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
    void     start(unsigned n, uint32_t src, uint32_t dst, uint32_t config, uint32_t mux = 0) {
        w(ch(n) + 0x04, dst);
        w(ch(n) + 0x08, src);
        w(ch(n) + 0x1C, mux);
        w(ch(n), config | kEnable);
    }
    void run_to_end() {
        while (auto next = dma.next_event_ns()) {
            clock.now = *next;
            dma.tick(clock.now);
            if (clock.now > 10'000'000'000ULL) FAIL("runs away");
        }
    }
};

} // namespace

TEST_CASE("gdma_bk7252n has one range of 0x240 bytes: globals and eight channels of 0x40") {
    Fixture f;
    REQUIRE(f.dma.ranges().size() == 1);
    CHECK(f.dma.ranges()[0].name == "regs");
    CHECK(f.dma.ranges()[0].size == 0x240);
    f.w(ch(7) + 0x04, 0x1234);
    f.w(ch(7) + 0x08, 0x5678);
    f.w(ch(7) + 0x0C, 0x55);
    f.w(ch(7) + 0x18, 0x66);
    f.w(ch(7) + 0x1C, 0x77);
    f.w(ch(7) + 0x20, 7);
    f.w(ch(7) + 0x24, 9);
    CHECK(f.r(ch(7) + 0x04) == 0x1234);
    CHECK(f.r(ch(7) + 0x08) == 0x5678);
    CHECK(f.r(ch(7) + 0x0C) == 0x55);
    CHECK(f.r(ch(7) + 0x18) == 0x66);
    CHECK(f.r(ch(7) + 0x1C) == 0x77);
    CHECK(f.r(ch(7) + 0x20) == 7);
    CHECK(f.r(ch(7) + 0x24) == 9);
    CHECK(f.r(ch(6) + 0x04) == 0);
    CHECK(f.r(0x00) == 0); // dev_id
    CHECK(f.r(0x04) == 0); // ver_id
}

TEST_CASE("gdma_bk7252n memory copy: the enable bit clears, the finish flag is in the channel status") {
    Fixture f;
    f.start(7, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    CHECK((f.r(ch(7)) & 1) == 1);
    CHECK((f.r(ch(7) + 0x30) & 0x1FFFF) == 64);
    f.run_to_end();
    for (uint32_t i = 0; i < 64; ++i) CHECK(f.mem.bytes[0x400 + i] == f.mem.bytes[0x100 + i]);
    CHECK((f.r(ch(7)) & 1) == 0);
    CHECK((f.r(ch(7) + 0x30) & 0x1FFFF) == 0);
    CHECK((f.r(ch(7) + 0x30) & kFin) != 0);
    CHECK((f.r(ch(7) + 0x30) >> 24 & 0xF) == 1); // finish count
    CHECK(f.r(kIntStatus) == 1u << 7);
    CHECK(f.r(ch(7) + 0x28) == 0x140); // src_rd_addr
    CHECK(f.r(ch(7) + 0x2C) == 0x440); // dst_wr_addr
    CHECK(f.line());
    f.w(ch(7) + 0x30, kFin);
    CHECK((f.r(ch(7) + 0x30) & kFin) == 0);
    CHECK_FALSE(f.line());
}

TEST_CASE("gdma_bk7252n half finish flag is bit 19, cleared on its own") {
    Fixture f;
    f.start(2, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | kHalfInt));
    f.run_to_end();
    CHECK((f.r(ch(2) + 0x30) & (kFin | kHalf)) == (kFin | kHalf));
    f.w(ch(2) + 0x30, kHalf);
    CHECK((f.r(ch(2) + 0x30) & (kFin | kHalf)) == kFin);
    CHECK(f.line());
    f.w(kIntStatus, 1u << 2); // clears both flags of channel 2
    CHECK((f.r(ch(2) + 0x30) & (kFin | kHalf)) == 0);
    CHECK_FALSE(f.line());
}

TEST_CASE("gdma_bk7252n request lines 0x00 and 0x10 are memory, others stall") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc), 0x10); // DTCM read request
    f.run_to_end();
    CHECK((f.r(ch(0)) & 1) == 0);
    CHECK(f.mem.bytes[0x400] == f.mem.bytes[0x100]);
    f.start(1, 0x100, 0x500, cfg(16, 2, 2, kSrcInc | kDstInc), 0x11); // UART1 RX
    CHECK_FALSE(f.dma.next_event_ns().has_value());
    CHECK((f.r(ch(1)) & 1) == 1);
    CHECK(f.out.str().find("source request line UART1_RX is not emulated") != std::string::npos);
}

TEST_CASE("gdma_bk7252n a source with a request line is paced; the destination field is at bit 5") {
    Fixture f;
    auto    line = f.dma.request_line(0x17); // JPEG RX
    f.start(3, 0, 0x400, cfg(8, 2, 2, kDstInc), 0x17);
    CHECK((f.r(ch(3)) & 1) == 1);
    CHECK(f.out.str().find("not emulated") == std::string::npos);
    f.start(4, 0x100, 0x400, cfg(8, 2, 2, kSrcInc), 0x17 << 5); // to a peripheral: stalls
    CHECK(f.out.str().find("destination request line JPEG_RX is not emulated") != std::string::npos);
}

TEST_CASE("gdma_bk7252n priority mode is bit 2 of the control word") {
    Fixture f;
    f.w(kControl, 0x7);
    CHECK(f.r(kControl) == 0x7);
    // Fixed priority never grants priority 0.
    f.start(0, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc));
    f.run_to_end();
    CHECK((f.r(ch(0)) & 1) == 1);
    f.w(kControl, 0x3);
    CHECK(f.r(kControl) == 0x3);
}

TEST_CASE("gdma_bk7252n reset clears the channels, the flags and the global words") {
    Fixture f;
    f.w(0x10, 0xABC);
    f.w(0x28, 0x123);
    f.start(1, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc | kFinInt));
    f.run_to_end();
    REQUIRE(f.line());
    f.dma.reset();
    CHECK_FALSE(f.line());
    CHECK(f.r(kIntStatus) == 0);
    CHECK(f.r(0x10) == 0);
    CHECK(f.r(0x28) == 0);
    CHECK(f.r(ch(1) + 0x04) == 0);
}
