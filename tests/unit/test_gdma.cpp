#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "periph/gdma.h"

using namespace bk;

namespace {

// Byte memory with holes: a read or write outside [0, size) is a bus error.
struct FakeMemory : BusMemory {
    std::vector<uint8_t> bytes;
    int                  reads = 0, writes = 0;
    explicit FakeMemory(size_t size = 0x1000) : bytes(size) {
        for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i * 3 + 1);
    }
    std::vector<uint8_t> read(uint32_t addr, uint32_t size) override {
        ++reads;
        if (uint64_t{addr} + size > bytes.size()) throw BusError(addr, "nothing readable there");
        return {bytes.begin() + addr, bytes.begin() + addr + size};
    }
    void write(uint32_t addr, std::span<const uint8_t> data) override {
        ++writes;
        if (uint64_t{addr} + data.size() > bytes.size()) throw BusError(addr, "nothing writable there");
        std::copy(data.begin(), data.end(), bytes.begin() + addr);
    }
};

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

constexpr uint32_t kEnable = 1, kFinInt = 2, kHalfInt = 4, kRepeat = 8, kSrcInc = 0x100, kDstInc = 0x200;
constexpr uint32_t kSrcLoop = 0x400, kDstLoop = 0x800;
constexpr uint32_t kIntStatus = 0xE0, kPrio = 0xDC;

constexpr uint32_t cfg(uint32_t length_bytes, uint32_t sdw, uint32_t ddw, uint32_t flags) {
    return (length_bytes - 1) << 16 | ddw << 6 | sdw << 4 | flags;
}

struct Fixture {
    std::ostringstream out;
    FakeClock          clock;
    FakeMemory         mem;
    std::vector<bool>  irq;
    Gdma               dma;

    Fixture() : dma("gdma", clock, [this](bool v) { irq.push_back(v); }, mem, 120'000'000) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    void     w(uint32_t off, uint32_t v) { dma.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return dma.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
    // Programs channel `ch` and starts it.
    void start(unsigned ch, uint32_t src, uint32_t dst, uint32_t config, uint32_t mux = 0) {
        w(ch * 0x20 + 0x04, dst);
        w(ch * 0x20 + 0x08, src);
        w(ch * 0x20 + 0x1C, mux);
        w(ch * 0x20, config | kEnable);
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

TEST_CASE("gdma has one range of 0x200 bytes and resets idle") {
    Fixture f;
    REQUIRE(f.dma.ranges().size() == 1);
    CHECK(f.dma.ranges()[0].name == "regs");
    CHECK(f.dma.ranges()[0].size == 0x200);
    CHECK(f.r(0x00) == 0);
    CHECK(f.r(kIntStatus) == 0);
    CHECK_FALSE(f.dma.next_event_ns().has_value());
    CHECK(f.r(0x1F0) == 0); // unknown offsets read 0
}

TEST_CASE("gdma channel registers: addresses, loops and mux are plain words per channel") {
    Fixture f;
    for (unsigned ch = 0; ch < 6; ++ch) {
        f.w(ch * 0x20 + 0x04, 0x1000 + ch);
        f.w(ch * 0x20 + 0x08, 0x2000 + ch);
        f.w(ch * 0x20 + 0x0C, 0x30 + ch);
        f.w(ch * 0x20 + 0x18, 0x40 + ch);
        f.w(ch * 0x20 + 0x1C, 0x50 + ch);
    }
    for (unsigned ch = 0; ch < 6; ++ch) {
        CHECK(f.r(ch * 0x20 + 0x04) == 0x1000 + ch);
        CHECK(f.r(ch * 0x20 + 0x08) == 0x2000 + ch);
        CHECK(f.r(ch * 0x20 + 0x0C) == 0x30 + ch);
        CHECK(f.r(ch * 0x20 + 0x18) == 0x40 + ch);
        CHECK(f.r(ch * 0x20 + 0x1C) == 0x50 + ch);
    }
    CHECK(f.r(0x10) == 0);
}

TEST_CASE("gdma prio_mode keeps bit 0; pause registers are plain words") {
    Fixture f;
    f.w(kPrio, 0xFF);
    CHECK(f.r(kPrio) == 1);
    f.w(0x100 + 4 * 2, 0xAA);
    f.w(0x120 + 4 * 5, 0xBB);
    CHECK(f.r(0x100 + 4 * 2) == 0xAA);
    CHECK(f.r(0x120 + 4 * 5) == 0xBB);
    CHECK(f.r(0x100 + 4 * 5) == 0);
}

TEST_CASE("gdma memory to memory moves its words as time passes") {
    Fixture f;
    // 64 bytes, 32-bit both sides: 16 writes * 7.2 cycles at 120 MHz = 960 ns.
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    CHECK(f.r(0x00) == (cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt) | 1));
    CHECK(f.dma.next_event_ns() == std::optional<uint64_t>(16ULL * 72 * 100'000'000 / 120'000'000));
    CHECK(f.mem.writes == 0);
    f.clock.now = 480;
    f.dma.tick(480);
    CHECK(f.mem.writes == 8);
    f.clock.now = 959;
    f.dma.tick(959);
    CHECK(f.mem.writes == 15);
    CHECK((f.r(0x00) & 1) == 1);
    CHECK(f.r(kIntStatus) == 0);
    f.clock.now = 960;
    f.dma.tick(960);
    for (uint32_t i = 0; i < 64; ++i) CHECK(f.mem.bytes[0x400 + i] == f.mem.bytes[0x100 + i]);
    CHECK((f.r(0x00) & 1) == 0);
    CHECK(f.r(kIntStatus) == 1);
    CHECK(f.line());
    CHECK_FALSE(f.dma.next_event_ns().has_value());
    f.w(kIntStatus, 1);
    CHECK_FALSE(f.line());
}

TEST_CASE("gdma without fin_int_en finishes silently") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc));
    f.run_to_end();
    CHECK(f.r(kIntStatus) == 0);
    CHECK_FALSE(f.line());
    CHECK(f.mem.bytes[0x400] == f.mem.bytes[0x100]);
}

TEST_CASE("gdma half finish flag comes at half time and is acknowledged separately") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | kHalfInt));
    CHECK(f.dma.next_event_ns() == std::optional<uint64_t>(480));
    f.clock.now = 480;
    f.dma.tick(480);
    CHECK(f.r(kIntStatus) == (1u << 8));
    CHECK(f.line());
    CHECK(f.mem.writes == 8);
    CHECK((f.r(0xC0) & 0x1FFFF) == 32); // half of the bytes left
    f.run_to_end();
    CHECK(f.r(kIntStatus) == (1u | 1u << 8));
    f.w(kIntStatus, 1u << 8);
    CHECK(f.r(kIntStatus) == 1);
    CHECK(f.line());
    f.w(kIntStatus, 1);
    CHECK_FALSE(f.line());
}

TEST_CASE("gdma byte widths: 8-bit source into 32-bit destination packs four reads per write") {
    Fixture f;
    for (int i = 0; i < 8; ++i) f.mem.bytes[0x100 + 4 * i] = static_cast<uint8_t>(0xA0 + i);
    // 8 bytes, source 8-bit incrementing by 4 per read, destination 32-bit.
    f.start(0, 0x100, 0x400, cfg(8, 0, 2, kSrcInc | kDstInc));
    f.run_to_end();
    // Each source read takes the low byte of the word at src, src += 4.
    CHECK(std::vector<uint8_t>(f.mem.bytes.begin() + 0x400, f.mem.bytes.begin() + 0x408) ==
          std::vector<uint8_t>{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7});
}

TEST_CASE("gdma 32-bit source into 8-bit destination spreads a word over four writes of the low lane") {
    Fixture f;
    f.mem.bytes[0x100] = 0x11;
    f.mem.bytes[0x101] = 0x22;
    // 4 bytes, source 32-bit, destination 8-bit, destination incrementing by 4 per write.
    f.start(0, 0x100, 0x400, cfg(4, 2, 0, kSrcInc | kDstInc));
    f.run_to_end();
    // The accumulator holds 4 bytes; each write drains 1 byte into the low lane of a 4-byte slot.
    CHECK(f.mem.bytes[0x400] == 0x11);
    CHECK(f.mem.bytes[0x404] == 0x22);
    CHECK(f.mem.bytes[0x408] == f.mem.bytes[0x102]);
    CHECK(f.mem.bytes[0x40C] == f.mem.bytes[0x103]);
}

TEST_CASE("gdma an under-filled last write is zero padded") {
    Fixture f;
    std::fill(f.mem.bytes.begin() + 0x400, f.mem.bytes.begin() + 0x410, 0xEE);
    // 6 bytes: a 32-bit destination gets two writes, the second holds two bytes plus padding.
    f.start(0, 0x100, 0x400, cfg(6, 2, 2, kSrcInc | kDstInc));
    f.run_to_end();
    CHECK(f.mem.bytes[0x404] == f.mem.bytes[0x104]);
    CHECK(f.mem.bytes[0x405] == f.mem.bytes[0x105]);
    CHECK(f.mem.bytes[0x406] == 0);
    CHECK(f.mem.bytes[0x407] == 0);
}

TEST_CASE("gdma fixed addresses read and write the same place") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(12, 2, 2, 0));
    f.run_to_end();
    CHECK(f.mem.reads == 3);
    CHECK(f.mem.writes == 3);
    CHECK(f.mem.bytes[0x400] == f.mem.bytes[0x100]);
}

TEST_CASE("gdma registers written while a channel runs do not change its transfer") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc));
    f.w(0x04, 0x600); // destination changed while running
    f.run_to_end();
    CHECK(f.mem.bytes[0x400] == f.mem.bytes[0x100]);
    CHECK(f.mem.bytes[0x600] == static_cast<uint8_t>(0x600 * 3 + 1));
}

TEST_CASE("gdma destination loop wraps at the exclusive end to the window start") {
    Fixture f;
    // 64 words through a 16-word window: the window holds the last 16 words.
    f.w(0x0C, 0x440); // dst loop end
    f.w(0x10, 0x400); // dst loop start
    f.start(0, 0x100, 0x400, cfg(256, 2, 2, kSrcInc | kDstInc | kDstLoop));
    f.run_to_end();
    for (uint32_t i = 0; i < 64; ++i) CHECK(f.mem.bytes[0x400 + i] == f.mem.bytes[0x100 + 192 + i]);
    CHECK(f.mem.bytes[0x440] == static_cast<uint8_t>(0x440 * 3 + 1));
    CHECK(f.r(0x160) == 0x400); // a whole number of turns
}

TEST_CASE("gdma source loop repeats its window; a start inside the window wraps to the window start") {
    Fixture f;
    f.w(0x14, 0x110); // src loop end: a 4-word window
    f.w(0x18, 0x100); // src loop start
    f.start(0, 0x108, 0x400, cfg(32, 2, 2, kSrcInc | kDstInc | kSrcLoop));
    f.run_to_end();
    // Words 2, 3, then 0, 1, 2, 3, 0, 1 of the window.
    const uint32_t order[] = {2, 3, 0, 1, 2, 3, 0, 1};
    for (uint32_t w = 0; w < 8; ++w)
        for (uint32_t b = 0; b < 4; ++b) CHECK(f.mem.bytes[0x400 + 4 * w + b] == f.mem.bytes[0x100 + 4 * order[w] + b]);
}

TEST_CASE("gdma a loop with its end register at 0 is off") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kDstLoop));
    f.run_to_end();
    CHECK(f.mem.bytes[0x43C] == f.mem.bytes[0x13C]);
}

TEST_CASE("gdma a stop keeps what was written, remain_len and dst_wr_addr; enable starts over") {
    Fixture f;
    std::fill(f.mem.bytes.begin() + 0x400, f.mem.bytes.begin() + 0x500, 0xEE);
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    f.clock.now = 300; // 5 words at 60 ns each
    f.w(0x00, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    CHECK((f.r(0x00) & 1) == 0);
    CHECK(f.mem.writes == 5);
    CHECK(f.r(0xC0) == 64 - 20);
    CHECK(f.r(0x160) == 0x414);
    CHECK(f.mem.bytes[0x414] == 0xEE);
    CHECK(f.r(kIntStatus) == 0);
    f.clock.now = 10'000;
    f.dma.tick(f.clock.now);
    CHECK(f.mem.writes == 5);
    // A bare enable copies the whole transfer again from the start addresses.
    std::fill(f.mem.bytes.begin() + 0x400, f.mem.bytes.begin() + 0x500, 0xEE);
    f.w(0x00, f.r(0x00) | kEnable);
    f.run_to_end();
    for (uint32_t i = 0; i < 64; ++i) CHECK(f.mem.bytes[0x400 + i] == f.mem.bytes[0x100 + i]);
    CHECK(f.mem.bytes[0x440] == 0xEE);
}

TEST_CASE("gdma remain_len counts bytes for narrow widths") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(64, 0, 0, kSrcInc | kDstInc)); // 64 one-byte writes
    f.clock.now = 600;                                          // 10 of them
    CHECK(f.r(0xC0) == 54);
}

TEST_CASE("gdma half finish flag holds the channel at the half point within a tick") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(600, 2, 2, kSrcInc | kDstInc | kHalfInt));
    f.clock.now = 100'000; // long after the whole transfer would have ended
    f.dma.tick(f.clock.now);
    CHECK(f.r(kIntStatus) == (1u << 8));
    CHECK((f.r(0xC0) & 0x1FFFF) == 300);
    CHECK((f.r(0x00) & 1) == 1);
    f.clock.now = 200'000;
    f.dma.tick(f.clock.now);
    CHECK((f.r(0x00) & 1) == 0);
    CHECK((f.r(0xC0) & 0x1FFFF) == 0);
}

TEST_CASE("gdma repeat mode continues the addresses and flags every cycle") {
    Fixture f;
    f.w(0x0C, 0x440); // dst window: 16 words
    f.w(0x10, 0x400);
    f.w(0x14, 0x140); // src window: 16 words
    f.w(0x18, 0x100);
    // 10 words per cycle: if each cycle restarted at the start, words 10..15 would never be written.
    f.start(0, 0x100, 0x400, cfg(40, 2, 2, kSrcInc | kDstInc | kSrcLoop | kDstLoop | kRepeat | kFinInt));
    for (int cycle = 1; cycle <= 3; ++cycle) {
        auto next = f.dma.next_event_ns();
        REQUIRE(next);
        f.clock.now = *next;
        f.dma.tick(f.clock.now);
        CHECK(f.r(kIntStatus) == 1);
        CHECK(((f.r(0xC0) >> 24) & 0xF) == static_cast<uint32_t>(cycle));
        CHECK((f.r(0x00) & 1) == 1);
        f.w(kIntStatus, 1);
    }
    for (uint32_t i = 0; i < 64; ++i) CHECK(f.mem.bytes[0x400 + i] == f.mem.bytes[0x100 + i]);
    CHECK(f.r(0x160) == 0x400 + 4 * (30 % 16));
    f.w(0x00, 0);
    CHECK((f.r(0x00) & 1) == 0);
    CHECK_FALSE(f.dma.next_event_ns().has_value());
}

TEST_CASE("gdma repeat mode without a finish interrupt keeps rewriting fixed addresses") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(4, 2, 2, kRepeat));
    CHECK_FALSE(f.dma.next_event_ns().has_value()); // no flag to wait for
    f.clock.now = 6000;                             // 100 cycles of 60 ns
    f.dma.tick(f.clock.now);
    CHECK(f.mem.writes == 100);
    f.mem.bytes[0x100] = 0x5A;
    f.clock.now        = 6060;
    f.dma.tick(f.clock.now);
    CHECK(f.mem.bytes[0x400] == 0x5A);
    CHECK((f.r(0x00) & 1) == 1);
}

TEST_CASE("gdma round-robin splits the bus evenly") {
    Fixture f;
    // Alone 64 bytes take 960 ns; two channels each get one word per 10 cycles: 1600 tenths = 1333 ns.
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    f.start(1, 0x200, 0x500, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    CHECK(f.dma.next_event_ns() == std::optional<uint64_t>(1334));
    f.clock.now = 1300;
    f.dma.tick(f.clock.now);
    CHECK(f.r(kIntStatus) == 0);
    f.clock.now = 1334;
    f.dma.tick(f.clock.now);
    CHECK(f.r(kIntStatus) == 3);
}

TEST_CASE("gdma fixed priority serves the higher priority, then the higher channel, first") {
    Fixture f;
    f.w(kPrio, 1);
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | 3u << 12));
    f.start(1, 0x200, 0x500, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | 1u << 12));
    f.start(2, 0x300, 0x600, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | 1u << 12));
    // Channel 0 runs at its full speed and finishes first.
    f.clock.now = *f.dma.next_event_ns();
    f.dma.tick(f.clock.now);
    CHECK(f.clock.now == 960);
    CHECK(f.r(kIntStatus) == 1);
    f.run_to_end();
    CHECK(f.r(kIntStatus) == 7);
    // Channel 2 got the rest of the bus before channel 1.
    Fixture g;
    g.w(kPrio, 1);
    g.start(1, 0x200, 0x500, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | 1u << 12));
    g.start(2, 0x300, 0x600, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | 1u << 12));
    g.clock.now = *g.dma.next_event_ns();
    g.dma.tick(g.clock.now);
    CHECK(g.r(kIntStatus) == 4);
}

TEST_CASE("gdma fixed priority never grants priority 0, not even after round-robin") {
    Fixture f;
    f.w(kPrio, 1);
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    CHECK_FALSE(f.dma.next_event_ns().has_value());
    f.clock.now = 100'000;
    f.dma.tick(f.clock.now);
    CHECK(f.mem.writes == 0);
    CHECK((f.r(0xC0) & 0x1FFFF) == 64);
    CHECK(f.r(kIntStatus) == 0x00050000);
    f.w(kPrio, 0);
    f.clock.now = 200'000;
    f.dma.tick(f.clock.now);
    CHECK(f.mem.writes == 0);
    CHECK((f.r(0x00) & 1) == 1);
    f.w(0x00, 0);
    CHECK(f.r(kIntStatus) == 0);
    // Priority 1 alone runs normally.
    f.w(kPrio, 1);
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt | 1u << 12));
    f.run_to_end();
    CHECK(f.r(kIntStatus) == 1);
}

TEST_CASE("gdma timing grows with the pacing interval") {
    Fixture f;
    // Interval 10: (10 + 2) * 10 = 120 tenths of a cycle per word. 4 writes at 120 MHz = 4 * 120 * 1e8 / 120e6.
    f.start(0, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc), 10u << 12);
    CHECK(f.dma.next_event_ns() == std::optional<uint64_t>(4ULL * 120 * 100'000'000 / 120'000'000));
    // Interval 5 still costs 7.2 cycles per word.
    Fixture g;
    g.start(0, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc), 5u << 16);
    CHECK(g.dma.next_event_ns() == std::optional<uint64_t>(4ULL * 72 * 100'000'000 / 120'000'000));
    // Reads count too: 8-bit source needs 16 reads for 16 bytes, more than 4 writes.
    Fixture h;
    h.start(0, 0x100, 0x400, cfg(16, 0, 2, kSrcInc | kDstInc));
    CHECK(h.dma.next_event_ns() == std::optional<uint64_t>(16ULL * 72 * 100'000'000 / 120'000'000));
}

TEST_CASE("gdma writing enable clear while running stops the channel silently") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    f.w(0x00, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt));
    CHECK((f.r(0x00) & 1) == 0);
    CHECK_FALSE(f.dma.next_event_ns().has_value());
    f.clock.now = 100000;
    f.dma.tick(100000);
    CHECK(f.mem.writes == 0);
    CHECK(f.r(kIntStatus) == 0);
}

TEST_CASE("gdma enable written while running does not restart") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc));
    uint64_t end = *f.dma.next_event_ns();
    f.clock.now  = 100;
    f.w(0x00, cfg(64, 2, 2, kSrcInc | kDstInc) | kEnable);
    CHECK(f.dma.next_event_ns() == std::optional<uint64_t>(end));
}

TEST_CASE("gdma bus error aborts the channel without flags") {
    Fixture f;
    f.start(0, 0x0FFC, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc | kFinInt)); // the second read leaves memory
    f.run_to_end();
    CHECK((f.r(0x00) & 1) == 0);
    CHECK(f.r(kIntStatus) == 0);
}

TEST_CASE("gdma other request lines are not emulated and stall") {
    Fixture f;
    f.start(1, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc), 4); // source UART1, no line registered
    CHECK((f.r(0x20) & 1) == 1);
    CHECK_FALSE(f.dma.next_event_ns().has_value());
    f.clock.now = 1'000'000'000;
    f.dma.tick(f.clock.now);
    CHECK((f.r(0x20) & 1) == 1);
    // Software can still stop it.
    f.w(0x20, 0);
    CHECK((f.r(0x20) & 1) == 0);
}

TEST_CASE("gdma paced channel moves words only while the line is up") {
    Fixture f;
    auto    line = f.dma.request_line(8);
    int     next = 0;
    line->bind([&] {
        ++next;
        return std::array<uint8_t, 4>{static_cast<uint8_t>(next), 0, 0, 0};
    });
    // 16 bytes = 4 writes; duration 4 * 72 * 1e8 / 120e6 = 240 ns, word_ns = 60.
    f.start(0, 0, 0x400, cfg(16, 2, 2, kDstInc | kFinInt | kHalfInt), 8);
    CHECK_FALSE(f.dma.next_event_ns().has_value()); // line down
    f.clock.now = 600;
    f.dma.tick(600);
    CHECK(f.mem.writes == 0);
    line->set(true);
    CHECK(f.dma.next_event_ns() == std::optional<uint64_t>(600 + 60));
    f.clock.now = 600 + 130; // two words allowed
    f.dma.tick(f.clock.now);
    CHECK(f.mem.writes == 2);
    CHECK(f.mem.bytes[0x400] == 1);
    CHECK(f.mem.bytes[0x404] == 2);
    CHECK(f.r(kIntStatus) == (1u << 8)); // half done
    CHECK(f.r(0xC0) == (8u | 1u << 28)); // two writes (8 bytes) left, one half flag counted
    CHECK(f.r(0x160) == 0x408);
    f.run_to_end();
    CHECK(f.mem.writes == 4);
    CHECK((f.r(0x00) & 1) == 0);
    CHECK(f.r(kIntStatus) == (1u | 1u << 8));
    CHECK(f.r(0xC0) == (0u | 1u << 24 | 1u << 28));
    CHECK(f.r(0x160) == 0x410); // kept after the stop
}

TEST_CASE("gdma paced channel drops the lag when the line was down") {
    Fixture f;
    auto    line = f.dma.request_line(8);
    line->bind([] { return std::array<uint8_t, 4>{7, 0, 0, 0}; });
    f.start(0, 0, 0x400, cfg(16, 2, 2, kDstInc), 8);
    line->set(true);
    f.clock.now = 60;
    f.dma.tick(60);
    CHECK(f.mem.writes == 1);
    line->set(false);
    f.clock.now = 5000;
    f.dma.tick(5000); // line down: `last` jumps to now
    line->set(true);
    CHECK(f.dma.next_event_ns() == std::optional<uint64_t>(5060));
    f.clock.now = 5059;
    f.dma.tick(5059);
    CHECK(f.mem.writes == 1);
}

TEST_CASE("gdma paced channel bus error stops it") {
    Fixture f;
    auto    line = f.dma.request_line(8);
    line->bind([] { return std::array<uint8_t, 4>{7, 0, 0, 0}; });
    line->set(true);
    f.start(0, 0, 0x0FFC, cfg(16, 2, 2, kDstInc | kFinInt), 8);
    f.clock.now = 10000;
    f.dma.tick(10000);
    CHECK((f.r(0x00) & 1) == 0);
    CHECK(f.r(kIntStatus) == 0);
}

TEST_CASE("gdma request line is replaced by a later request, survives reset") {
    Fixture f;
    auto    first  = f.dma.request_line(8);
    auto    second = f.dma.request_line(8);
    CHECK(first != second);
    second->bind([] { return std::array<uint8_t, 4>{5, 0, 0, 0}; });
    second->set(true);
    first->set(true);
    f.dma.reset();
    f.start(0, 0, 0x400, cfg(4, 2, 2, kDstInc | kFinInt), 8);
    f.run_to_end();
    CHECK(f.mem.bytes[0x400] == 5);
}

TEST_CASE("gdma status and dst_wr_addr follow the words written") {
    Fixture f;
    f.start(0, 0x100, 0x400, cfg(64, 2, 2, kSrcInc | kDstInc | kFinInt)); // 16 writes over 960 ns
    f.clock.now = 480;
    CHECK(f.r(0xC0) == 32);
    CHECK(f.r(0x160) == 0x400 + 4 * 8);
    f.clock.now = 5000;
    CHECK(f.r(0xC0) == (0u | 1u << 24));
    CHECK(f.r(0x160) == 0x400 + 4 * 16); // kept after the finish
}

TEST_CASE("gdma counts wrap at 4 bits") {
    Fixture f;
    for (int i = 0; i < 17; ++i) {
        f.start(0, 0x100, 0x400, cfg(4, 2, 2, kSrcInc | kDstInc | kFinInt));
        f.run_to_end();
    }
    CHECK((f.r(0xC0) >> 24) == 1);
}

TEST_CASE("gdma reset clears channels and flags") {
    Fixture f;
    f.w(0x04, 0x1234);
    f.start(1, 0x100, 0x400, cfg(16, 2, 2, kSrcInc | kDstInc | kFinInt));
    f.run_to_end();
    REQUIRE(f.line());
    f.w(kPrio, 1);
    f.dma.reset();
    CHECK_FALSE(f.line());
    CHECK(f.r(kIntStatus) == 0);
    CHECK(f.r(0x04) == 0);
    CHECK(f.r(0x20 + 0x04) == 0);
    CHECK(f.r(kPrio) == 0);
    CHECK(f.r(0xC4) == 0);
}
