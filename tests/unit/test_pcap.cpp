#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "net/pcap.h"

using namespace bk;

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t le32(const Bytes &b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
uint16_t le16(const Bytes &b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); }

struct TempFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "bkemu_test_pcap.cap";
    ~TempFile() { std::filesystem::remove(path); }
    Bytes contents() const {
        std::ifstream in(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(in), {});
    }
};

// An air that remembers what it was given and hands a frame back on request.
struct StubAir : WifiAir {
    Deliver deliver;
    Bytes   transmitted;
    int     ticks = 0, resets = 0;

    void                    attach(Deliver d) override { deliver = std::move(d); }
    void                    transmit(std::span<const uint8_t> f) override { transmitted.assign(f.begin(), f.end()); }
    void                    tick(uint64_t) override { ++ticks; }
    std::optional<uint64_t> next_event_ns() const override { return 42; }
    void                    reset() override { ++resets; }
};

} // namespace

TEST_CASE("pcap: the file header names radiotap and a record holds the frame") {
    TempFile file;
    {
        PcapWriter w(file.path.string());
        CHECK(w.write(3'000'002'000, Bytes{0xaa, 0xbb, 0xcc}, 6));
        CHECK(w.write(4'000'000'000, Bytes{0xdd}, std::nullopt));
    }
    Bytes b = file.contents();
    REQUIRE(b.size() == 24 + (16 + 12 + 3) + (16 + 8 + 1));
    CHECK(le32(b, 0) == 0xa1b2c3d4);
    CHECK(le16(b, 4) == 2);
    CHECK(le16(b, 6) == 4);
    CHECK(le32(b, 20) == 127);

    // 3.000002 s, radiotap with the channel field, then the frame.
    CHECK(le32(b, 24) == 3);
    CHECK(le32(b, 28) == 2);
    CHECK(le32(b, 32) == 15);
    CHECK(le32(b, 36) == 15);
    CHECK(b[40] == 0); // radiotap version
    CHECK(le16(b, 42) == 12);
    CHECK(le32(b, 44) == 1u << 3);
    CHECK(le16(b, 48) == 2437); // channel 6
    CHECK(le16(b, 50) == 0x0080);
    CHECK(Bytes(b.begin() + 52, b.begin() + 55) == Bytes{0xaa, 0xbb, 0xcc});

    // A frame without a known channel gets the bare radiotap header.
    const size_t second = 24 + 16 + 12 + 3;
    CHECK(le32(b, second) == 4);
    CHECK(le32(b, second + 8) == 9);
    CHECK(le16(b, second + 16 + 2) == 8);
    CHECK(le32(b, second + 16 + 4) == 0);
    CHECK(b.back() == 0xdd);
}

TEST_CASE("pcap: a file that cannot be opened is an error") {
    CHECK_THROWS_AS(PcapWriter("/nonexistent-dir/x.cap"), std::runtime_error);
}

TEST_CASE("pcap: the capturing air records both directions and passes everything on") {
    std::ostringstream log;
    Logger::reset_for_tests();
    Logger::configure(log, false);
    TempFile file;
    {
        PcapWriter         w(file.path.string());
        StubAir            inner;
        CapturingAir       air(inner, w, Logger::get("net.pcap"), [] { return std::optional<unsigned>(1); });
        std::vector<Bytes> delivered;
        air.attach([&](Bytes f) { delivered.push_back(std::move(f)); });

        air.tick(5'000'000'000);
        CHECK(inner.ticks == 1);
        CHECK(air.next_event_ns() == 42);
        air.transmit(Bytes{1, 2});
        CHECK(inner.transmitted == Bytes{1, 2});
        inner.deliver(Bytes{3, 4, 5});
        REQUIRE(delivered.size() == 1);
        CHECK(delivered[0] == Bytes{3, 4, 5});
        air.reset();
        CHECK(inner.resets == 1);
    }
    Bytes b = file.contents();
    REQUIRE(b.size() == 24 + (16 + 12 + 2) + (16 + 12 + 3));
    CHECK(le32(b, 24) == 5);
    CHECK(Bytes(b.begin() + 52, b.begin() + 54) == Bytes{1, 2});
    CHECK(Bytes(b.begin() + 24 + 30 + 28, b.end()) == Bytes{3, 4, 5});
    Logger::reset_for_tests();
}

TEST_CASE("pcap: a target that refuses the file header is an error") {
    // /dev/full accepts the open and refuses every write.
    CHECK_THROWS_AS(PcapWriter("/dev/full"), std::runtime_error);
}
