#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "periph/jpeg_bk7252n.h"

using namespace bk;

namespace {

constexpr uint32_t kClkCtrl = 0x08, kFifo = 0x14, kIntStatus = 0x18, kBytes = 0x1C, kPartialX = 0x20, kIntEnable = 0x30,
                   kConfig = 0x34, kTargetH = 0x38, kQuant = 0x80;
constexpr uint32_t kSof = 1u << 0, kEof = 1u << 1, kSofInt = 1u << 2, kEofInt = 1u << 3;
constexpr uint32_t kByteRev = 1u << 1, kEncEn = 1u << 4, kEncSize = 1u << 16, kFifoEmpty = 1u << 26;
constexpr uint64_t kMs = 1'000'000;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

// SOI, payload, EOI: 19 bytes, which with the 5-byte trailer fill 6 words.
std::vector<uint8_t> picture() {
    std::vector<uint8_t> p(19);
    for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint8_t>(0x10 + i);
    p[0]  = 0xFF;
    p[1]  = 0xD8;
    p[17] = 0xFF;
    p[18] = 0xD9;
    return p;
}

struct Fixture {
    FakeClock                    clock;
    std::vector<bool>            irq;
    std::shared_ptr<RequestLine> dma = std::make_shared<RequestLine>();
    JpegBk7252n                  jpeg;

    Fixture() : jpeg("JPEG", clock, [this](bool v) { irq.push_back(v); }, dma, 1000, 150, picture()) {}

    void     w(uint32_t off, uint32_t v) { jpeg.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return jpeg.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
    void     at(uint64_t ns) {
        clock.now = ns;
        jpeg.tick(ns);
    }
};

} // namespace

TEST_CASE("jpeg_bk7252n has one range of 0x100 bytes and plain configuration words") {
    Fixture f;
    REQUIRE(f.jpeg.ranges().size() == 1);
    CHECK(f.jpeg.ranges()[0].size == 0x100);
    f.w(kClkCtrl, 0x3);
    f.w(kTargetH, 0x1234);
    f.w(kQuant + 4, 0x55);
    CHECK(f.r(kClkCtrl) == 0x3);
    CHECK(f.r(kTargetH) == 0x1234);
    CHECK(f.r(kQuant + 4) == 0x55);
    CHECK(f.r(0x00) == 0); // dev_id
    CHECK(f.r(kPartialX) == kFifoEmpty);
}

TEST_CASE("jpeg_bk7252n frames start one interval after enable and raise sof then eof") {
    Fixture f;
    f.w(kIntEnable, kSofInt | kEofInt);
    f.w(kConfig, kEncEn | kByteRev);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(1000 * kMs));
    f.at(1000 * kMs);
    CHECK(f.r(kIntStatus) == kSof);
    CHECK(f.line());
    CHECK((f.r(kPartialX) & kFifoEmpty) == 0);
    CHECK(f.dma->level());
    f.w(kIntStatus, kSof);
    CHECK_FALSE(f.line());
    f.at(1150 * kMs);
    CHECK(f.r(kIntStatus) == kEof);
    CHECK(f.line());
    f.w(kIntStatus, kEof);
    CHECK_FALSE(f.line());
}

TEST_CASE("jpeg_bk7252n an event without its enable leaves the line down") {
    Fixture f;
    f.w(kIntEnable, kEofInt);
    f.w(kConfig, kEncEn);
    f.at(1000 * kMs);
    CHECK(f.r(kIntStatus) == kSof);
    CHECK_FALSE(f.line());
}

TEST_CASE("jpeg_bk7252n the byte count reads the frame size minus 1, as the driver adds 1") {
    Fixture f;
    CHECK(f.r(kBytes) == 0);
    f.w(kConfig, kEncEn | kEncSize | kByteRev);
    f.at(1000 * kMs);
    f.at(1150 * kMs);
    CHECK(f.r(kBytes) + 1 == 19);
}

TEST_CASE("jpeg_bk7252n the FIFO gives the picture and the size trailer, then the DMA line drops") {
    Fixture f;
    f.w(kConfig, kEncEn | kEncSize | kByteRev);
    f.at(1000 * kMs);
    std::vector<uint8_t> bytes;
    while (!(f.r(kPartialX) & kFifoEmpty)) {
        const uint32_t word = f.r(kFifo);
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(word >> (8 * i)));
    }
    REQUIRE(bytes.size() == 24);
    const std::vector<uint8_t> p = picture();
    CHECK(std::equal(p.begin(), p.end(), bytes.begin()));
    CHECK(bytes[19] == 0);
    CHECK(bytes[20] == 19); // the frame size, little endian
    CHECK_FALSE(f.dma->level());
}

TEST_CASE("jpeg_bk7252n words are big endian without video_byte_rev") {
    Fixture f;
    f.w(kConfig, kEncEn);
    f.at(1000 * kMs);
    CHECK(f.r(kFifo) == 0xFFD81213);
}

TEST_CASE("jpeg_bk7252n disabling the encoder stops the frames; reset keeps the quantization table") {
    Fixture f;
    f.w(kQuant, 7);
    f.w(kConfig, kEncEn);
    f.w(kConfig, 0);
    CHECK_FALSE(f.jpeg.next_event_ns().has_value());
    f.w(kIntEnable, kSofInt);
    f.jpeg.reset();
    CHECK(f.r(kIntEnable) == 0);
    CHECK(f.r(kQuant) == 7);
}
