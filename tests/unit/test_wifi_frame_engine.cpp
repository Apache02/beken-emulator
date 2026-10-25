#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "periph/wifi/air.h"
#include "periph/wifi/mac.h"

using namespace bk;

namespace {

constexpr uint32_t kThdMarker = 0xCAFEBABE, kTbdMarker = 0xCAFEFADE, kRhdMarker = 0xBAADF00D, kRbdMarker = 0xC0DEDBAD;

// Platform bank registers.
constexpr uint32_t kTxrxStatus = 0x078, kTxrxEnable = 0x080, kDmaSet = 0x180, kDmaClear = 0x184, kDmaStatus = 0x188,
                   kRxHeaderHead = 0x1B8, kRxPayloadHead = 0x1BC;
constexpr uint32_t kMaster = 1u << 31;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

// Zeroed RAM at [0, size); everything else is a bus error. Writes at or above
// `write_limit` fail.
struct Memory : BusMemory {
    std::vector<uint8_t> bytes;
    uint32_t             write_limit = 0xFFFFFFFF;
    explicit Memory(size_t size = 0x10000) : bytes(size) {}
    std::vector<uint8_t> read(uint32_t addr, uint32_t size) override {
        if (uint64_t{addr} + size > bytes.size()) throw BusError(addr, "nothing readable there");
        return {bytes.begin() + addr, bytes.begin() + addr + size};
    }
    void write(uint32_t addr, std::span<const uint8_t> data) override {
        if (uint64_t{addr} + data.size() > bytes.size() || addr >= write_limit)
            throw BusError(addr, "nothing writable there");
        std::copy(data.begin(), data.end(), bytes.begin() + addr);
    }
    uint32_t word(uint32_t addr) const {
        uint32_t v;
        std::memcpy(&v, bytes.data() + addr, 4);
        return v;
    }
    void set(uint32_t addr, uint32_t value) { std::memcpy(bytes.data() + addr, &value, 4); }
    void fill(uint32_t addr, const std::vector<uint8_t> &data) {
        std::copy(data.begin(), data.end(), bytes.begin() + addr);
    }
};

struct FakeAir : WifiAir {
    Deliver                           deliver;
    std::vector<std::vector<uint8_t>> sent;
    std::vector<uint64_t>             ticks;
    std::optional<uint64_t>           event;
    int                               resets = 0, attaches = 0;

    void attach(Deliver d) override { deliver = std::move(d), ++attaches; }
    void transmit(std::span<const uint8_t> frame) override { sent.emplace_back(frame.begin(), frame.end()); }
    void tick(uint64_t now_ns) override { ticks.push_back(now_ns); }
    std::optional<uint64_t> next_event_ns() const override { return event; }
    void                    reset() override { ++resets; }
};

std::vector<uint8_t> pattern(uint8_t first, size_t size) {
    std::vector<uint8_t> v(size);
    for (size_t i = 0; i < size; ++i) v[i] = static_cast<uint8_t>(first + i);
    return v;
}

struct Fixture {
    std::ostringstream out;
    FakeClock          clock;
    Memory             mem;
    FakeAir            air;
    bool               tx_line = false, rx_line = false;
    WifiMac            mac;

    Fixture()
        : mac("MAC",
              clock,
              Signal(),
              WifiMac::Lines{{}, {}, [this](bool v) { rx_line = v; }, [this](bool v) { tx_line = v; }, {}, {}},
              nullptr,
              mem) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
        mac.attach(&air);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    uint32_t pl(uint32_t off) { return mac.read("pl", off, 4); }
    void     wpl(uint32_t off, uint32_t v) { mac.write("pl", off, 4, v); }
    void     activate() { mac.write("core", 0x038, 4, 3u << 4); }
    void     enable_txrx() { wpl(kTxrxEnable, kMaster | 0x7FFFFFFF); }
    void     at_us(uint64_t us) {
        clock.now = us * 1000;
        mac.tick(clock.now);
    }

    // A transmit header descriptor with its header buffer at `buf` (empty when 0).
    void
    thd(uint32_t addr, uint32_t next, uint32_t tbd, uint32_t buf, uint32_t size, uint32_t length, uint32_t control) {
        mem.set(addr, kThdMarker);
        mem.set(addr + 4, next);
        mem.set(addr + 12, tbd);
        mem.set(addr + 16, buf);
        mem.set(addr + 20, buf ? buf + size - 1 : 0);
        mem.set(addr + 24, length);
        mem.set(addr + 56, control);
    }
    void tbd(uint32_t addr, uint32_t next, uint32_t buf, uint32_t size) {
        mem.set(addr, kTbdMarker);
        mem.set(addr + 4, next);
        mem.set(addr + 8, buf);
        mem.set(addr + 12, buf + size - 1);
    }
    void rhd(uint32_t addr, uint32_t next) {
        mem.set(addr, kRhdMarker);
        mem.set(addr + 4, next);
    }
    void rbd(uint32_t addr, uint32_t next, uint32_t buf, uint32_t size) {
        mem.set(addr, kRbdMarker);
        mem.set(addr + 4, next);
        mem.set(addr + 8, buf);
        mem.set(addr + 12, buf + size - 1);
    }
    void set_rx_heads(uint32_t header, uint32_t payload) {
        wpl(kRxHeaderHead, header);
        wpl(kRxPayloadHead, payload);
        wpl(kDmaSet, 3u << 26);
    }
    // A receive chain of `count` frames' worth of descriptors: RHDs at 0x4000 +
    // 0x80 i, RBDs at 0x6000 + 0x20 i with 32-byte buffers at 0x8000 + 0x20 i.
    void rx_chain(uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const bool last = i + 1 == count;
            rhd(0x4000 + 0x80 * i, last ? 0 : 0x4000 + 0x80 * (i + 1));
            rbd(0x6000 + 0x20 * i, last ? 0 : 0x6000 + 0x20 * (i + 1), 0x8000 + 0x20 * i, 32);
        }
    }
};

// A frame (at least 16 bytes) whose address 2 (bytes 10..15) is `transmitter`.
std::vector<uint8_t> frame_from(uint8_t transmitter, uint8_t first = 0, size_t size = 24) {
    std::vector<uint8_t> f = pattern(first, size);
    f[4]                   = 0; // not a group address
    std::fill(f.begin() + 10, f.begin() + 16, transmitter);
    return f;
}

} // namespace

TEST_CASE("dma control registers read 0") {
    Fixture f;
    f.wpl(kDmaSet, 0);
    f.wpl(kDmaClear, 0xFFFFFFFF);
    f.wpl(kDmaStatus, 0x1234);
    CHECK(f.pl(kDmaSet) == 0);
    CHECK(f.pl(kDmaClear) == 0);
    CHECK(f.pl(kDmaStatus) == 0);
}

TEST_CASE("transmit gathers the header buffer and the buffer descriptors") {
    Fixture f;
    f.clock.now = 1'000'000;
    f.mem.fill(0x1000, pattern(0x10, 10));
    f.mem.fill(0x1100, pattern(0x40, 6));
    f.mem.fill(0x1200, pattern(0x70, 4));
    f.thd(0x100, 0, 0x300, 0x1000, 10, 24, 0);
    f.tbd(0x300, 0x320, 0x1100, 6);
    f.tbd(0x320, 0, 0x1200, 4);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);

    REQUIRE(f.air.sent.size() == 1);
    std::vector<uint8_t> expected = pattern(0x10, 10);
    for (uint8_t b : pattern(0x40, 6)) expected.push_back(b);
    for (uint8_t b : pattern(0x70, 4)) expected.push_back(b);
    CHECK(f.air.sent[0] == expected); // 24 - 4 FCS bytes = 20
    CHECK(f.mem.word(0x300 + 16) == 0x80000000);
    CHECK(f.mem.word(0x320 + 16) == 0x80000000);
}

TEST_CASE("transmit drops the FCS bytes and keeps short frames whole") {
    Fixture f;
    f.mem.fill(0x1000, pattern(1, 30));
    f.thd(0x100, 0, 0, 0x1000, 30, 34, 0);
    f.thd(0x200, 0, 0, 0x1000, 3, 4, 0);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    f.wpl(0x19C, 0x200);
    f.wpl(kDmaSet, 1u << 9);
    REQUIRE(f.air.sent.size() == 2);
    CHECK(f.air.sent[0].size() == 30);
    CHECK(f.air.sent[1].size() == 3);
}

TEST_CASE("transmit completes 150 us later with status and air time") {
    Fixture f;
    f.clock.now = 2'000'000;
    f.mem.fill(0x1000, pattern(1, 20));
    f.thd(0x100, 0, 0, 0x1000, 20, 24, 1u << 8);
    f.mem.set(0x100 + 60, 0x00000001);
    f.enable_txrx();
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);

    CHECK(f.mem.word(0x100 + 60) == 1);
    CHECK(f.mac.next_event_ns() == std::optional<uint64_t>(2'150'000));
    f.at_us(2149);
    CHECK(f.mem.word(0x100 + 60) == 1);
    CHECK(f.pl(kTxrxStatus) == 0);
    CHECK_FALSE(f.tx_line);
    f.at_us(2150);
    CHECK(f.mem.word(0x100 + 60) == (0x80000000u | 0x00800000u | 1u));
    CHECK(f.mem.word(0x100 + 64) == 150);
    CHECK(f.pl(kTxrxStatus) == (1u << 6));
    CHECK(f.tx_line);
    CHECK_FALSE(f.mac.next_event_ns().has_value());
}

TEST_CASE("the trigger bit is raised only when the descriptor asks for it") {
    Fixture f;
    f.mem.fill(0x1000, pattern(1, 20));
    f.thd(0x100, 0, 0, 0x1000, 20, 24, 0);
    f.enable_txrx();
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    f.at_us(1000);
    CHECK(f.mem.word(0x100 + 60) == 0x80800000u);
    CHECK(f.pl(kTxrxStatus) == 0);
}

TEST_CASE("every queue has its new head bit, head register, new tail bit and trigger bit") {
    struct Q {
        uint32_t head_bit, head_reg, tail_bit, trigger_bit;
    };
    const Q queues[] = {
        {8,  0x198, 0, 10},
        {9,  0x19C, 1, 6 },
        {10, 0x1A0, 2, 7 },
        {11, 0x1A4, 3, 8 },
        {12, 0x1A8, 4, 9 },
    };
    for (const Q &q : queues) {
        CAPTURE(q.head_bit);
        Fixture f;
        f.mem.fill(0x1000, pattern(0x20, 20));
        f.thd(0x100, 0x200, 0, 0x1000, 20, 24, 1u << 8);
        f.thd(0x200, 0, 0, 0x1000, 8, 12, 1u << 8);
        f.enable_txrx();
        f.wpl(q.head_reg, 0x100);
        f.wpl(kDmaSet, 1u << q.head_bit);
        // The head chain reaches the second descriptor by itself.
        REQUIRE(f.air.sent.size() == 2);
        CHECK(f.air.sent[0].size() == 20);
        CHECK(f.air.sent[1].size() == 8);
        f.at_us(200);
        CHECK(f.pl(kTxrxStatus) == (1u << q.trigger_bit));

        // A new tail continues after the last descriptor sent on the queue.
        f.thd(0x300, 0, 0, 0x1000, 6, 10, 0);
        f.mem.set(0x200 + 4, 0x300);
        f.wpl(kDmaSet, 1u << q.tail_bit);
        REQUIRE(f.air.sent.size() == 3);
        CHECK(f.air.sent[2].size() == 6);
    }
}

TEST_CASE("a new head wins over a new tail and queues run beacon first") {
    Fixture f;
    for (uint32_t i = 0; i < 5; ++i) {
        f.mem.fill(0x1000 + 0x40 * i, pattern(static_cast<uint8_t>(0x10 * (i + 1)), 8));
        f.thd(0x100 + 0x80 * i, 0, 0, 0x1000 + 0x40 * i, 8, 12, 0);
    }
    // thd i belongs to queue: 0 -> 0, 1 -> 1, 2 -> 2, 3 -> 3, 4 -> 4 (beacon).
    f.wpl(0x19C, 0x100);
    f.wpl(0x1A0, 0x180);
    f.wpl(0x1A4, 0x200);
    f.wpl(0x1A8, 0x280);
    f.wpl(0x198, 0x300);
    f.wpl(kDmaSet, (1u << 12) | (1u << 11) | (1u << 10) | (1u << 9) | (1u << 8) | (1u << 1) | (1u << 0));
    REQUIRE(f.air.sent.size() == 5);
    CHECK(f.air.sent[0][0] == 0x50); // queue 4
    CHECK(f.air.sent[1][0] == 0x10); // queue 0
    CHECK(f.air.sent[2][0] == 0x20);
    CHECK(f.air.sent[3][0] == 0x30);
    CHECK(f.air.sent[4][0] == 0x40);
}

TEST_CASE("a new tail with nothing linked sends nothing") {
    Fixture f;
    f.wpl(kDmaSet, 1u << 1); // no descriptor was ever sent on queue 0
    f.mem.fill(0x1000, pattern(1, 8));
    f.thd(0x100, 0, 0, 0x1000, 8, 12, 0);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    REQUIRE(f.air.sent.size() == 1);
    f.wpl(kDmaSet, 1u << 1); // next is still 0
    CHECK(f.air.sent.size() == 1);
}

TEST_CASE("a bad marker stops the chain") {
    Fixture f;
    f.mem.fill(0x1000, pattern(1, 8));
    f.thd(0x100, 0x200, 0, 0x1000, 8, 12, 0);
    f.thd(0x200, 0, 0, 0x1000, 8, 12, 0);
    f.mem.set(0x200, 0xDEADBEEF);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    CHECK(f.air.sent.size() == 1);

    f.wpl(0x19C, 0x20000); // outside memory
    f.wpl(kDmaSet, 1u << 9);
    CHECK(f.air.sent.size() == 1);
}

TEST_CASE("a descriptor without data waits for the firmware to kick again") {
    Fixture f;
    f.thd(0x100, 0, 0, 0, 0, 12, 1u << 8);
    f.enable_txrx();
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    f.at_us(1000);
    CHECK(f.air.sent.empty());
    CHECK(f.mem.word(0x100 + 60) == 0);
    CHECK_FALSE(f.mac.next_event_ns().has_value());

    f.mem.fill(0x1000, pattern(1, 12));
    f.thd(0x100, 0, 0, 0x1000, 12, 16, 1u << 8);
    f.wpl(kDmaSet, 1u << 9);
    CHECK(f.air.sent.size() == 1);
}

TEST_CASE("buffers with a bad extent are skipped") {
    Fixture f;
    f.mem.fill(0x1000, pattern(1, 8));
    f.mem.fill(0x1100, pattern(0x50, 8));
    f.thd(0x100, 0, 0x300, 0x1000, 8, 40, 0);
    f.mem.set(0x100 + 20, 0x1000 - 1); // end < start: no header buffer
    f.tbd(0x300, 0x320, 0x1100, 8);
    f.mem.set(0x300 + 12, 0x1100 + 4096); // too long
    f.tbd(0x320, 0, 0x1100, 8);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    REQUIRE(f.air.sent.size() == 1);
    CHECK(f.air.sent[0] == pattern(0x50, 8));
    CHECK(f.mem.word(0x300 + 16) == 0); // skipped
    CHECK(f.mem.word(0x320 + 16) == 0x80000000);
}

TEST_CASE("a chain is limited to 64 descriptors and 32 buffer descriptors") {
    Fixture f;
    f.mem.fill(0x1000, pattern(1, 4));
    for (uint32_t i = 0; i < 70; ++i)
        f.thd(0x2000 + 0x80 * i, i == 69 ? 0 : 0x2000 + 0x80 * (i + 1), 0, 0x1000, 4, 8, 0);
    f.wpl(0x19C, 0x2000);
    f.wpl(kDmaSet, 1u << 9);
    CHECK(f.air.sent.size() == 64);

    Fixture g;
    g.mem.fill(0x1100, pattern(1, 2));
    g.thd(0x100, 0, 0x400, 0, 0, 100, 0);
    for (uint32_t i = 0; i < 40; ++i) g.tbd(0x400 + 0x20 * i, i == 39 ? 0 : 0x400 + 0x20 * (i + 1), 0x1100, 2);
    g.wpl(0x19C, 0x100);
    g.wpl(kDmaSet, 1u << 9);
    REQUIRE(g.air.sent.size() == 1);
    CHECK(g.air.sent[0].size() == 64);
    CHECK(g.mem.word(0x400 + 0x20 * 31 + 16) == 0x80000000);
    CHECK(g.mem.word(0x400 + 0x20 * 32 + 16) == 0);
}

TEST_CASE("transmit tolerates a completion write error and unreadable buffers") {
    Fixture f;
    f.mem.fill(0x1000, pattern(1, 8));
    f.thd(0x100, 0, 0x300, 0x1000, 8, 12, 0);
    f.tbd(0x300, 0, 0x20000, 8); // unreadable payload
    f.mem.write_limit = 0x100;   // descriptors are read-only
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    REQUIRE(f.air.sent.size() == 1);
    CHECK(f.air.sent[0].size() == 8);
    f.at_us(500);
}

TEST_CASE("frames are dropped silently without an air") {
    Fixture f;
    f.mac.attach(nullptr);
    f.mem.fill(0x1000, pattern(1, 8));
    f.thd(0x100, 0, 0, 0x1000, 8, 12, 1u << 8);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    f.at_us(500);
    CHECK(f.air.sent.empty());
    CHECK(f.mem.word(0x100 + 60) == 0x80800000u);
    f.activate();
    f.mac.receive(frame_from(1));
}

TEST_CASE("reset drops pending completions and resets the air") {
    Fixture f;
    f.mem.fill(0x1000, pattern(1, 8));
    f.thd(0x100, 0, 0, 0x1000, 8, 12, 0);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    const int before = f.air.resets;
    f.mac.reset();
    CHECK(f.air.resets == before + 1);
    f.at_us(500);
    CHECK(f.mem.word(0x100 + 60) == 0);
    // The last-sent pointers are gone too.
    f.wpl(kDmaSet, 1u << 1);
    CHECK(f.air.sent.size() == 1);
}

TEST_CASE("the air is attached, ticked and merged into next_event_ns") {
    Fixture f;
    CHECK(f.air.attaches == 1);
    f.at_us(10);
    REQUIRE(f.air.ticks.size() == 1);
    CHECK(f.air.ticks[0] == 10'000);

    CHECK_FALSE(f.mac.next_event_ns().has_value());
    f.air.event = 5'000'000;
    CHECK(f.mac.next_event_ns() == std::optional<uint64_t>(5'000'000));
    f.mem.fill(0x1000, pattern(1, 8));
    f.thd(0x100, 0, 0, 0x1000, 8, 12, 0);
    f.wpl(0x19C, 0x100);
    f.wpl(kDmaSet, 1u << 9);
    CHECK(f.mac.next_event_ns() == std::optional<uint64_t>(10'000 + 150'000));
    f.air.event = 20'000;
    CHECK(f.mac.next_event_ns() == std::optional<uint64_t>(20'000));
}

TEST_CASE("receive delivers a frame into the descriptors") {
    Fixture f;
    f.activate();
    f.enable_txrx();
    f.clock.now = 7'000'000;
    f.rhd(0x400, 0x440);
    f.mem.set(0x400 + 12, 0x11111111);
    f.mem.set(0x400 + 24, 0x22222222);
    f.rbd(0x500, 0x520, 0x2000, 8);
    f.rbd(0x520, 0, 0x2100, 8);
    f.mem.set(0x500 + 16, 0xABCD0000);
    f.set_rx_heads(0x400, 0x500);
    CHECK(f.pl(kTxrxStatus) == 0);

    const std::vector<uint8_t> frame = frame_from(0x77, 0x30, 16);
    f.air.deliver(frame);

    CHECK(std::equal(frame.begin(), frame.begin() + 8, f.mem.bytes.begin() + 0x2000));
    CHECK(std::equal(frame.begin() + 8, frame.end(), f.mem.bytes.begin() + 0x2100));
    CHECK(f.mem.word(0x500 + 16) == 0xABCD0002);
    CHECK(f.mem.word(0x520 + 16) == 0x00000003);
    CHECK(f.mem.word(0x400) == kRhdMarker);
    CHECK(f.mem.word(0x400 + 4) == 0x440);
    CHECK(f.mem.word(0x400 + 8) == 0x500);
    CHECK(f.mem.word(0x400 + 12) == 0x11111111);
    CHECK(f.mem.word(0x400 + 24) == 0x22222222);
    CHECK(f.mem.word(0x400 + 28) == 16);
    CHECK(f.mem.word(0x400 + 32) == 7000); // TSF low (the MAC started at 0)
    CHECK(f.mem.word(0x400 + 36) == 0);
    CHECK(f.mem.word(0x400 + 40) == 20);
    CHECK(f.mem.word(0x400 + 48) == 0xD30000D3u);
    CHECK(f.mem.word(0x400 + 64) == 0x6000);
    CHECK(f.pl(kTxrxStatus) == ((1u << 16) | (1u << 17) | (1u << 19)));
    CHECK(f.rx_line);
}

TEST_CASE("receive marks group frames and valid keys") {
    Fixture f;
    f.activate();
    // Key entry 30 for the transmitter 0x77...: address registers, then write.
    f.mac.write("core", 0x0BC, 4, 0x77777777);
    f.mac.write("core", 0x0C0, 4, 0x7777);
    f.mac.write("core", 0x0C4, 4, (1u << 30) | 30u << 16);

    f.rx_chain(3);
    f.set_rx_heads(0x4000, 0x6000);

    std::vector<uint8_t> group = frame_from(0x11, 0, 24);
    group[4] |= 1;
    f.mac.receive(group);
    CHECK(f.mem.word(0x4000 + 64) == (0x4000u | 0x2000u | 0x0400u));

    f.mac.receive(frame_from(0x77, 0, 24));
    CHECK(f.mem.word(0x4080 + 64) == (0x4000u | 0x2000u | 0x02000000u | 30u << 15));

    f.mac.receive(frame_from(0x55, 0, 24));
    CHECK(f.mem.word(0x4100 + 64) == 0x6000u);
}

TEST_CASE("receive is ignored unless the MAC is active") {
    Fixture f;
    f.rx_chain(2);
    f.set_rx_heads(0x4000, 0x6000);
    f.mac.receive(frame_from(1));
    f.activate();
    f.set_rx_heads(0x4000, 0x6000);
    CHECK(f.mem.word(0x4000 + 64) == 0); // nothing was queued
    f.mac.receive(frame_from(1));
    CHECK(f.mem.word(0x4000 + 64) == 0x6000u);
}

TEST_CASE("frames fill consecutive descriptors") {
    Fixture f;
    f.activate();
    f.rx_chain(3);
    f.set_rx_heads(0x4000, 0x6000);
    for (uint8_t i = 0; i < 3; ++i) f.mac.receive(frame_from(1, static_cast<uint8_t>(0x10 * (i + 1)), 20));
    for (uint32_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(f.mem.word(0x4000 + 0x80 * i + 64) == 0x6000u);
        CHECK(f.mem.word(0x4000 + 0x80 * i + 8) == 0x6000 + 0x20 * i);
        CHECK(f.mem.bytes[0x8000 + 0x20 * i] == 0x10 * (i + 1));
        CHECK(f.mem.word(0x6000 + 0x20 * i + 16) == 3);
    }
    // The chain is used up: the fourth frame waits.
    f.mac.receive(frame_from(1, 0x99, 20));
    CHECK(f.mem.bytes[0x8000 + 0x20 * 2] == 0x30);
}

TEST_CASE("a frame waits until there is enough buffer space") {
    Fixture f;
    f.activate();
    f.enable_txrx();
    f.rhd(0x400, 0);
    f.rbd(0x500, 0, 0x2000, 8);
    f.set_rx_heads(0x400, 0x500);
    f.mac.receive(frame_from(1, 0x40, 16));
    CHECK(f.mem.word(0x400 + 64) == 0);
    CHECK(f.mem.word(0x500 + 16) == 0);
    CHECK(f.mem.bytes[0x2000] == 0);
    CHECK(f.pl(kTxrxStatus) == 0);

    f.rbd(0x520, 0, 0x2100, 8);
    f.mem.set(0x500 + 4, 0x520);
    f.wpl(kDmaSet, 1u << 27); // new payload head
    CHECK(f.mem.word(0x400 + 64) == 0x6000u);
    CHECK(f.mem.word(0x500 + 16) == 2);
    CHECK(f.mem.word(0x520 + 16) == 3);
    CHECK(f.pl(kTxrxStatus) == ((1u << 16) | (1u << 17) | (1u << 19)));
}

TEST_CASE("a header descriptor that is done or invalid is not used") {
    Fixture f;
    f.activate();
    f.rhd(0x400, 0);
    f.mem.set(0x400 + 64, 0x4000);
    f.rbd(0x500, 0, 0x2000, 32);
    f.set_rx_heads(0x400, 0x500);
    f.mac.receive(frame_from(1));
    CHECK(f.mem.word(0x500 + 16) == 0);

    f.mem.set(0x400 + 64, 0);
    f.mem.set(0x500, 0); // bad RBD marker
    f.wpl(kDmaSet, 1u << 26);
    CHECK(f.mem.word(0x400 + 64) == 0);
    f.mem.set(0x500, kRbdMarker);
    f.mem.set(0x500 + 12, 0x2000 - 1); // end < start
    f.wpl(kDmaSet, 1u << 26);
    CHECK(f.mem.word(0x400 + 64) == 0);
    f.mem.set(0x500 + 12, 0x2000 + 31);
    f.wpl(kDmaSet, 1u << 26);
    CHECK(f.mem.word(0x400 + 64) == 0x6000u);
}

TEST_CASE("only the newest 64 queued frames are kept") {
    Fixture f;
    f.activate();
    for (uint8_t i = 0; i < 70; ++i) f.mac.receive(frame_from(1, i, 20));
    f.rx_chain(64);
    f.set_rx_heads(0x4000, 0x6000);
    for (uint32_t i = 0; i < 64; ++i) {
        CAPTURE(i);
        CHECK(f.mem.word(0x4000 + 0x80 * i + 64) == 0x6000u);
        CHECK(f.mem.bytes[0x8000 + 0x20 * i] == i + 6);
    }
}

TEST_CASE("a receive buffer write error still counts the frame as delivered") {
    Fixture f;
    f.activate();
    f.rhd(0x400, 0);
    f.rbd(0x500, 0, 0x2000, 32);
    f.mem.write_limit = 0x2000;
    f.set_rx_heads(0x400, 0x500);
    f.mac.receive(frame_from(1));
    CHECK(f.mem.word(0x400 + 64) == 0x6000u);
}

TEST_CASE("set_rx_heads with one head keeps the other and restarts the chain") {
    Fixture f;
    f.activate();
    f.rx_chain(2);
    f.set_rx_heads(0x4000, 0x6000);
    f.mac.receive(frame_from(1, 0x10, 20));
    // Restart the receive side from the first descriptors again.
    f.mem.set(0x4000 + 64, 0);
    f.wpl(kRxHeaderHead, 0x4000 | 3); // low bits are dropped
    f.wpl(kDmaSet, 1u << 26);
    f.mac.receive(frame_from(1, 0x20, 20));
    CHECK(f.mem.bytes[0x8000] == 0x20); // RBD head 0x6000 was kept; RHD head restarted
}
