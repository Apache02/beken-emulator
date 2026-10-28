#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/log.h"
#include "periph/default_picture.h"
#include "periph/jpeg.h"

using namespace bk;

namespace {

constexpr uint32_t kCtrl0 = 0x00, kCtrl1 = 0x04, kTargetH = 0x08, kTargetL = 0x0C, kFifo = 0x14, kStatus = 0x18,
                   kBytes = 0x1C, kState = 0x20, kQuant = 0x80;
constexpr uint32_t kStartInt = 1u << 2, kEndInt = 1u << 3, kByteRev = 1u << 1, kEncEn = 1u << 4, kEncSize = 1u << 16;
constexpr uint64_t kMs = 1'000'000;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

// A tiny "JPEG": SOI, payload, EOI. 16 bytes, so 16 + 5 is not a multiple of 4.
std::vector<uint8_t> picture(size_t length = 16) {
    std::vector<uint8_t> p(length);
    for (size_t i = 0; i < length; ++i) p[i] = static_cast<uint8_t>(0x10 + i);
    p[0]          = 0xFF;
    p[1]          = 0xD8;
    p[length - 2] = 0xFF;
    p[length - 1] = 0xD9;
    return p;
}

struct Fixture {
    FakeClock                    clock;
    std::vector<bool>            irq;
    std::shared_ptr<RequestLine> dma = std::make_shared<RequestLine>();
    Jpeg                         jpeg;

    explicit Fixture(std::vector<uint8_t> pic         = picture(20), // 20 + 5 = 25: not aligned either
                     uint32_t             burst_words = 0)
        : jpeg("JPEG", clock, [this](bool v) { irq.push_back(v); }, dma, 1000, 150, std::move(pic), burst_words) {}

    void     w(uint32_t off, uint32_t v) { jpeg.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return jpeg.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
    void     at(uint64_t ns) {
        clock.now = ns;
        jpeg.tick(ns);
    }
    // Enables the encoder and runs to the start of the first frame.
    void first_frame(uint32_t extra_ctrl1 = 0) {
        w(kCtrl1, kEncEn | extra_ctrl1);
        at(1000 * kMs);
    }
    // All the bytes of the current stream as read by the CPU in natural order (video_byte_rev set).
    std::vector<uint8_t> drain() {
        std::vector<uint8_t> out;
        while (!(r(kState) & 1)) {
            uint32_t word = r(kFifo);
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(word >> (8 * i)));
        }
        return out;
    }
};

} // namespace

TEST_CASE("jpeg has one range of 0x100 bytes and resets idle") {
    Fixture f;
    REQUIRE(f.jpeg.ranges().size() == 1);
    CHECK(f.jpeg.ranges()[0].name == "regs");
    CHECK(f.jpeg.ranges()[0].size == 0x100);
    CHECK(f.r(kStatus) == 0);
    CHECK(f.r(kState) == 1); // no stream
    CHECK(f.r(kFifo) == 0);
    CHECK(f.r(kBytes) == 0);
    CHECK_FALSE(f.jpeg.next_event_ns().has_value());
    CHECK_FALSE(f.dma->level());
}

TEST_CASE("jpeg control registers are stored") {
    Fixture f;
    f.w(kCtrl0, 0x1234);
    f.w(kTargetH, 0xAAAA);
    f.w(kTargetL, 0x5555);
    CHECK(f.r(kCtrl0) == 0x1234);
    CHECK(f.r(kTargetH) == 0xAAAA);
    CHECK(f.r(kTargetL) == 0x5555);
    f.w(kCtrl1, 0xFFFFFFFF & ~kEncEn);
    CHECK(f.r(kCtrl1) == (0xFFFFFFFF & ~kEncEn));
    CHECK(f.r(0x10) == 0); // reserved
    CHECK(f.r(0x7C) == 0);
}

TEST_CASE("jpeg quantization table is 32 words and survives a reset") {
    Fixture f;
    for (uint32_t i = 0; i < 32; ++i) f.w(kQuant + 4 * i, 0x100 + i);
    for (uint32_t i = 0; i < 32; ++i) CHECK(f.r(kQuant + 4 * i) == 0x100 + i);
    f.w(kCtrl0, 5);
    f.jpeg.reset();
    CHECK(f.r(kCtrl0) == 0);
    for (uint32_t i = 0; i < 32; ++i) CHECK(f.r(kQuant + 4 * i) == 0x100 + i);
    CHECK(f.r(kQuant + 4 * 32) == 0); // past the table
}

TEST_CASE("jpeg enabling the encoder schedules the first frame one interval later") {
    Fixture f;
    f.clock.now = 5 * kMs;
    f.w(kCtrl1, kEncEn);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(5 * kMs + 1000 * kMs));
    // Writing it again while on changes nothing.
    f.clock.now = 6 * kMs;
    f.w(kCtrl1, kEncEn | kByteRev);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(1005 * kMs));
}

TEST_CASE("jpeg start_frame fills the stream, sets the status and schedules the next frame and the end") {
    Fixture f;
    f.w(kCtrl0, kStartInt);
    f.w(kCtrl1, kEncEn | kByteRev);
    f.at(1000 * kMs);
    CHECK(f.r(kStatus) == 1);
    CHECK(f.line());
    CHECK(f.dma->level());
    CHECK(f.r(kState) == 0);
    // The next frame is one interval after the scheduled time, the end 150 ms after it.
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(1150 * kMs));
    f.at(1150 * kMs);
    CHECK(f.r(kStatus) == 3);
    CHECK(f.r(kBytes) > 0);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(2000 * kMs));
}

TEST_CASE("jpeg frame events use the scheduled time, not the time of the tick") {
    Fixture f;
    f.w(kCtrl1, kEncEn);
    f.at(1500 * kMs); // late tick: start at 1000, end at 1150, next start at 2000
    CHECK(f.r(kStatus) == 3);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(2000 * kMs));
    f.at(3200 * kMs); // starts at 2000 and 3000, ends at 2150 and 3150
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(4000 * kMs));
}

TEST_CASE("jpeg picture is made to fit whole words with the trailer") {
    // 20 bytes: (20 + 5) % 4 != 0, so a comment segment is inserted after the SOI marker.
    Fixture f(picture(20));
    f.first_frame(kByteRev | kEncSize);
    std::vector<uint8_t> s = f.drain();
    CHECK(s.size() % 4 == 0);
    // SOI, then FF FE <len BE16> <pad zeros>, then the rest of the picture.
    CHECK(s[0] == 0xFF);
    CHECK(s[1] == 0xD8);
    CHECK(s[2] == 0xFF);
    CHECK(s[3] == 0xFE);
    const size_t pad = (4 - (20 + 4 + 5) % 4) % 4;
    CHECK(s[4] == 0);
    CHECK(s[5] == 2 + pad);
    for (size_t i = 0; i < pad; ++i) CHECK(s[6 + i] == 0);
    // The trailer: a zero byte and the little-endian length of the picture as stored.
    const size_t length = 20 + 4 + pad;
    CHECK(s.size() == length + 5);
    CHECK(s[length] == 0);
    CHECK(s[length + 1] == (length & 0xFF));
    CHECK(s[length + 2] == 0);
    CHECK(s[length + 3] == 0);
    CHECK(s[length + 4] == 0);
    // An already aligned picture is left alone: 19 + 5 = 24.
    Fixture g(picture(19));
    g.first_frame(kByteRev | kEncSize);
    CHECK(g.drain().size() == 19 + 5);
}

TEST_CASE("jpeg stream without enc_size is the picture padded with zeros to words") {
    Fixture f(picture(19)); // aligned with the trailer, not without it: 19 % 4 != 0
    f.first_frame(kByteRev);
    std::vector<uint8_t> s = f.drain();
    CHECK(s.size() == 20);
    CHECK(s[17] == 0xFF);
    CHECK(s[18] == 0xD9);
    CHECK(s[19] == 0x00); // zero padding
}

TEST_CASE("jpeg words are little endian with video_byte_rev and big endian without it") {
    Fixture f(picture(19));
    f.first_frame(kByteRev);
    CHECK(f.r(kFifo) == 0x1312D8FFu); // FF D8 12 13
    Fixture g(picture(19));
    g.first_frame(0);
    CHECK(g.r(kFifo) == 0xFFD81213u);
}

TEST_CASE("jpeg an exhausted stream returns the last word that was read") {
    Fixture f(picture(19)); // 19 + 5 = 24 bytes: six words
    f.first_frame(kByteRev | kEncSize);
    f.drain();
    // With enc_size the last word is the little-endian size trailer: 19 bytes.
    CHECK(f.r(kFifo) == 19);
    CHECK(f.r(kFifo) == 19);
    CHECK(f.r(kState) == 1);
    f.at(1150 * kMs);
    CHECK(f.r(kBytes) == 19);
    CHECK(f.r(kBytes) == f.r(kFifo)); // what the firmware sees after DMA drained the frame
}

TEST_CASE("jpeg the last word follows the byte order") {
    Fixture f(picture(19));
    f.first_frame(kEncSize); // big endian: the trailer reads byte-swapped
    f.drain();
    CHECK(f.r(kFifo) == 0x13000000u);
    Fixture g(picture(19));
    g.first_frame(kEncSize | kByteRev);
    g.drain();
    CHECK(g.r(kFifo) == 0x13u);
}

TEST_CASE("jpeg without enc_size the last word is the end of the picture") {
    Fixture f(picture(19));
    f.first_frame(kByteRev);
    f.drain();
    // The last picture bytes 0x22 FF D9 and one zero pad byte (bytes 16..19, 0x10 + 16 = 0x20).
    CHECK(f.r(kFifo) == 0x00D9FF20u);
    CHECK(f.r(kFifo) == 0x00D9FF20u);
}

TEST_CASE("jpeg the last word is not cleared by a new frame, by disabling the encoder or by reading nothing") {
    Fixture f(picture(19));
    f.first_frame(kByteRev | kEncSize);
    f.drain();
    f.at(2000 * kMs); // second frame starts: the stream is full again
    CHECK(f.r(kState) == 0);
    f.w(kCtrl1, kByteRev | kEncSize); // encoder off: stream emptied
    CHECK(f.r(kState) == 1);
    CHECK(f.r(kFifo) == 19); // still the last word
    // Only a reset clears it.
    f.jpeg.reset();
    CHECK(f.r(kFifo) == 0);
}

TEST_CASE("jpeg a new frame restarts the stream at its first word, even in the middle of the old one") {
    Fixture f(picture(19));
    f.first_frame(kByteRev | kEncSize);
    CHECK(f.r(kFifo) == 0x1312D8FFu);
    CHECK(f.r(kFifo) == 0x17161514u); // bytes 4..7
    f.at(2000 * kMs);
    CHECK(f.r(kFifo) == 0x1312D8FFu);
}

TEST_CASE("jpeg disabling the encoder drops the schedule and empties the stream") {
    Fixture f;
    f.first_frame();
    REQUIRE(f.r(kState) == 0);
    f.w(kCtrl1, 0);
    CHECK(f.r(kState) == 1);
    CHECK_FALSE(f.jpeg.next_event_ns().has_value());
    CHECK_FALSE(f.dma->level());
    // Enabling again starts the interval from the new time.
    f.clock.now = 10'000 * kMs;
    f.w(kCtrl1, kEncEn);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(11'000 * kMs));
}

TEST_CASE("jpeg status bits are write one to clear and the interrupt follows the enables") {
    Fixture f;
    f.w(kCtrl1, kEncEn);
    f.at(1150 * kMs); // both flags set
    CHECK_FALSE(f.line());
    f.w(kCtrl0, kStartInt);
    CHECK(f.line());
    f.w(kStatus, 1);
    CHECK_FALSE(f.line()); // start flag cleared, the end flag is not enabled
    f.w(kCtrl0, kEndInt);
    CHECK(f.line());
    f.w(kStatus, 2);
    CHECK_FALSE(f.line());
    CHECK(f.r(kStatus) == 0);
}

TEST_CASE("jpeg the DMA line is high while words remain and the DMA read returns little endian bytes") {
    Fixture f(picture(19));
    f.first_frame(kByteRev);
    CHECK(f.dma->level());
    std::array<uint8_t, 4> first = f.dma->read();
    CHECK(first == std::array<uint8_t, 4>{0xFF, 0xD8, 0x12, 0x13}); // video_byte_rev: stream order
    f.w(kCtrl1, kEncEn);                                            // rev off
    f.at(2000 * kMs);                                               // new frame
    std::array<uint8_t, 4> reversed = f.dma->read();
    CHECK(reversed == std::array<uint8_t, 4>{0x13, 0x12, 0xD8, 0xFF}); // reversed in memory
    while (f.dma->level()) f.dma->read();
    CHECK(f.r(kState) == 1);
}

TEST_CASE("jpeg burst_words releases the stream in bursts spread over the encode time") {
    // 59 + 5 = 64 bytes, 16 words, in bursts of 4: one every 37.5 ms of the 150.
    const uint32_t flags = kByteRev | kEncSize;
    Fixture        f(picture(59), 4);
    Fixture        whole(picture(59));
    whole.first_frame(flags);
    const std::vector<uint8_t> frame = whole.drain();
    REQUIRE(frame.size() == 64);

    f.first_frame(flags);
    CHECK(f.r(kState) == 1);
    CHECK_FALSE(f.dma->level());
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(1000 * kMs + 37'500'000));

    std::vector<uint8_t> got;
    auto                 take = [&] {
        std::vector<uint8_t> part = f.drain();
        got.insert(got.end(), part.begin(), part.end());
        return part.size();
    };
    f.at(1000 * kMs + 37'500'000);
    CHECK(f.dma->level());
    CHECK(take() == 16);
    CHECK_FALSE(f.dma->level());
    f.at(1075 * kMs);
    CHECK(take() == 16);
    f.at(1112 * kMs + 500'000);
    CHECK(take() == 16);
    // The last burst comes with the end of the frame, and the frame ends after it.
    CHECK(f.r(kStatus) == 1);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(1150 * kMs));
    f.at(1150 * kMs);
    CHECK(take() == 16);
    CHECK(f.r(kStatus) == 3);
    CHECK(got == frame);
    CHECK(f.jpeg.next_event_ns() == std::optional<uint64_t>(2000 * kMs));
}

TEST_CASE("jpeg burst_words after a late tick, and when the encoder is disabled") {
    Fixture f(picture(59), 4);
    f.w(kCtrl1, kEncEn | kByteRev | kEncSize);
    f.at(1300 * kMs);
    CHECK(f.r(kStatus) == 3);
    CHECK(f.drain().size() == 64);
    f.at(2020 * kMs); // the next frame has just begun
    CHECK(f.r(kState) == 1);
    f.at(2040 * kMs);
    CHECK(f.r(kState) == 0);
    f.w(kCtrl1, 0);
    CHECK(f.r(kState) == 1);
    CHECK_FALSE(f.jpeg.next_event_ns().has_value());
}

TEST_CASE("jpeg the default picture works and is a JPEG") {
    std::vector<uint8_t> pic(kDefaultPicture, kDefaultPicture + kDefaultPicture_size);
    REQUIRE(pic.size() > 1000);
    CHECK(pic[0] == 0xFF);
    CHECK(pic[1] == 0xD8);
    CHECK(pic[pic.size() - 2] == 0xFF);
    CHECK(pic[pic.size() - 1] == 0xD9);
    FakeClock clock;
    Jpeg      jpeg("JPEG", clock, {}, nullptr, 1000, 150, pic);
    jpeg.write("regs", kCtrl1, 4, kEncEn | kEncSize | kByteRev);
    clock.now = 1000 * kMs;
    jpeg.tick(clock.now);
    size_t words = 0;
    while (!(jpeg.read("regs", kState, 4) & 1)) {
        jpeg.read("regs", kFifo, 4);
        ++words;
    }
    // The whole picture plus the 5-byte trailer, in whole words (the picture is made to fit them).
    CHECK(words * 4 >= pic.size() + 5);
    CHECK(words * 4 <= pic.size() + 5 + 8);
}

TEST_CASE("jpeg a picture shorter than a marker is refused") {
    FakeClock clock;
    CHECK_THROWS_AS(Jpeg("JPEG", clock, {}, nullptr, 1000, 150, {}), std::invalid_argument);
    CHECK_THROWS_AS(Jpeg("JPEG", clock, {}, nullptr, 1000, 150, {0xFF}), std::invalid_argument);
    CHECK_NOTHROW(Jpeg("JPEG", clock, {}, nullptr, 1000, 150, {0xFF, 0xD8}));
}

TEST_CASE("jpeg works without interrupt and DMA lines") {
    FakeClock clock;
    Jpeg      jpeg("JPEG", clock, {}, nullptr, 10, 5, picture());
    jpeg.write("regs", kCtrl1, 4, kEncEn);
    clock.now = 10 * kMs;
    jpeg.tick(clock.now);
    CHECK(jpeg.read("regs", kStatus, 4) == 1);
}
