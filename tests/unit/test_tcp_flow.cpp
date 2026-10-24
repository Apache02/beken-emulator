#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <vector>

#include "net/packets.h"
#include "net/tcp_flow.h"

using namespace bk;
using namespace bk::packets;

namespace {

constexpr uint64_t kSec = 1'000'000'000;

constexpr uint16_t kLocal   = 40000;
constexpr uint16_t kPeer    = 80;
constexpr uint32_t kIss     = 1000;
constexpr uint32_t kPeerIss = 5000;

// A segment the flow sent, with its own copy of the payload.
struct Seg {
    uint16_t                src_port;
    uint16_t                dst_port;
    uint32_t                seq;
    uint32_t                ack;
    uint8_t                 flags;
    uint16_t                window;
    std::optional<uint16_t> mss;
    Bytes                   payload;
};

struct FakeLink : TcpLink {
    std::vector<Seg> out;
    uint64_t         now = 0;

    void send_segment(const Tcp &s) override {
        out.push_back({s.src_port,
                       s.dst_port,
                       s.seq,
                       s.ack,
                       s.flags,
                       s.window,
                       s.mss,
                       Bytes(s.payload.begin(), s.payload.end())});
    }
    uint64_t now_ns() const override { return now; }
};

Bytes bytes(size_t n, uint8_t first = 0) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = uint8_t(first + i);
    return b;
}

struct Fixture {
    FakeLink                 link;
    Bytes                    received;
    int                      peer_closed  = 0;
    int                      closed_calls = 0;
    bool                     closed_reset = false;
    std::unique_ptr<TcpFlow> flow;

    explicit Fixture(uint32_t iss = kIss) {
        flow = std::make_unique<TcpFlow>(
            link,
            kLocal,
            kPeer,
            iss,
            [this](std::span<const uint8_t> d) { received.insert(received.end(), d.begin(), d.end()); },
            [this] { ++peer_closed; },
            [this](bool reset) {
                ++closed_calls;
                closed_reset = reset;
            });
        flow->start();
    }

    void peer(uint8_t                 flags,
              uint32_t                seq,
              uint32_t                ack,
              const Bytes            &data   = {},
              uint16_t                window = 65535,
              std::optional<uint16_t> mss    = std::nullopt) {
        flow->receive(Tcp{kPeer, kLocal, seq, ack, flags, window, mss, data});
    }

    // The handshake, then a clean slate of sent segments.
    void establish(uint16_t window = 65535, std::optional<uint16_t> mss = std::nullopt) {
        peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 1, {}, window, mss);
        REQUIRE(flow->state() == TcpFlow::State::Open);
        link.out.clear();
    }

    // An ACK from the peer for everything up to `ack_seq` (a sequence number).
    void ack(uint32_t ack_seq, uint16_t window = 65535) { peer(kTcpAck, kPeerIss + 1, ack_seq, {}, window); }

    // Our stream position after n data bytes.
    static uint32_t at(uint32_t n) { return kIss + 1 + n; }
};

} // namespace

TEST_CASE("tcp flow: start sends a SYN with the MSS option and arms the timer") {
    Fixture f;
    f.link.now = 3 * kSec;
    f.flow     = std::make_unique<TcpFlow>(f.link, kLocal, kPeer, kIss, nullptr, nullptr, nullptr);
    f.link.out.clear();
    f.flow->start();
    REQUIRE(f.link.out.size() == 1);
    const Seg &s = f.link.out[0];
    CHECK(s.src_port == kLocal);
    CHECK(s.dst_port == kPeer);
    CHECK(s.seq == kIss);
    CHECK(s.ack == 0);
    CHECK(s.flags == kTcpSyn);
    CHECK(s.window == 16384);
    CHECK(s.mss == 1400);
    CHECK(s.payload.empty());
    CHECK(f.flow->state() == TcpFlow::State::SynSent);
    CHECK(f.flow->deadline() == 4 * kSec);
}

TEST_CASE("tcp flow: SYN+ACK opens, sends the ACK and clears the timer") {
    Fixture f;
    f.link.out.clear();
    f.peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 1, {}, 8000, 1200);
    CHECK(f.flow->state() == TcpFlow::State::Open);
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[0].seq == kIss + 1);
    CHECK(f.link.out[0].ack == kPeerIss + 1);
    CHECK(f.link.out[0].window == 16384);
    CHECK_FALSE(f.link.out[0].mss);
    CHECK_FALSE(f.flow->deadline());
    CHECK(f.flow->mss() == 1200);
}

TEST_CASE("tcp flow: the MSS is the smaller of ours and the peer's, 536 without an option") {
    {
        Fixture f;
        f.peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 1);
        CHECK(f.flow->mss() == 536);
    }
    {
        Fixture f;
        f.peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 1, {}, 100, 9000);
        CHECK(f.flow->mss() == 1400);
    }
}

TEST_CASE("tcp flow: a SYN+ACK with the wrong ack gets an RST and ends the flow as reset") {
    Fixture f;
    f.link.out.clear();
    f.peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 7);
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == kTcpRst);
    CHECK(f.link.out[0].seq == kIss + 7);
    CHECK(f.link.out[0].ack == 0);
    CHECK(f.flow->finished());
    CHECK(f.closed_calls == 1);
    CHECK(f.closed_reset);
    CHECK_FALSE(f.flow->deadline());
}

TEST_CASE("tcp flow: an RST that acks the SYN ends the flow, other segments in SYN-SENT do not") {
    Fixture f;
    f.link.out.clear();
    f.peer(kTcpRst, 0, 0);
    f.peer(kTcpRst | kTcpAck, 0, kIss + 5);
    f.peer(kTcpAck, kPeerIss, kIss + 1);
    f.peer(kTcpSyn, kPeerIss, 0);
    CHECK(f.flow->state() == TcpFlow::State::SynSent);
    CHECK(f.closed_calls == 0);
    CHECK(f.link.out.empty());
    f.peer(kTcpRst | kTcpAck, 0, kIss + 1);
    CHECK(f.flow->finished());
    CHECK(f.closed_calls == 1);
    CHECK(f.closed_reset);
    CHECK(f.link.out.empty());
}

TEST_CASE("tcp flow: an RST while open ends the flow as reset") {
    Fixture f;
    f.establish();
    f.peer(kTcpRst, kPeerIss + 1, 0);
    CHECK(f.flow->finished());
    CHECK(f.closed_calls == 1);
    CHECK(f.closed_reset);
    CHECK(f.link.out.empty());
    // Later segments and calls do nothing.
    f.peer(kTcpAck, kPeerIss + 1, kIss + 1, bytes(3));
    f.flow->write(bytes(3));
    f.flow->abort();
    CHECK(f.link.out.empty());
    CHECK(f.received.empty());
    CHECK(f.closed_calls == 1);
}

TEST_CASE("tcp flow: in order data is delivered and acked, overlap delivers the new tail") {
    Fixture f;
    f.establish();
    f.peer(kTcpAck | kTcpPsh, kPeerIss + 1, kIss + 1, bytes(10));
    CHECK(f.received == bytes(10));
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[0].seq == kIss + 1);
    CHECK(f.link.out[0].ack == kPeerIss + 11);
    CHECK(f.link.out[0].payload.empty());

    // 5 old bytes and 5 new ones.
    f.peer(kTcpAck, kPeerIss + 6, kIss + 1, bytes(10, 5));
    CHECK(f.received == bytes(15));
    CHECK(f.link.out.back().ack == kPeerIss + 16);

    // Entirely old data is acked again and not delivered.
    f.peer(kTcpAck, kPeerIss + 1, kIss + 1, bytes(10));
    CHECK(f.received == bytes(15));
    REQUIRE(f.link.out.size() == 3);
    CHECK(f.link.out.back().ack == kPeerIss + 16);
}

TEST_CASE("tcp flow: out of order data is dropped and answered with a duplicate ACK") {
    Fixture f;
    f.establish();
    f.peer(kTcpAck, kPeerIss + 1, kIss + 1, bytes(10));
    f.link.out.clear();
    f.peer(kTcpAck, kPeerIss + 21, kIss + 1, bytes(10, 20));
    CHECK(f.received == bytes(10));
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[0].ack == kPeerIss + 11);
    // The hole filled, the dropped part has to come again.
    f.peer(kTcpAck, kPeerIss + 11, kIss + 1, bytes(10, 10));
    f.peer(kTcpAck, kPeerIss + 21, kIss + 1, bytes(10, 20));
    CHECK(f.received == bytes(30));
    CHECK(f.link.out.back().ack == kPeerIss + 31);
}

TEST_CASE("tcp flow: a segment without data or FIN is not acked") {
    Fixture f;
    f.establish();
    f.ack(kIss + 1);
    f.peer(kTcpAck, kPeerIss + 1, kIss + 1);
    CHECK(f.link.out.empty());
}

TEST_CASE("tcp flow: the peer's FIN is acked, reported once, and writing still works") {
    Fixture f;
    f.establish();
    f.peer(kTcpFin | kTcpAck, kPeerIss + 1, kIss + 1);
    CHECK(f.peer_closed == 1);
    CHECK(f.flow->peer_closed());
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[0].ack == kPeerIss + 2);
    CHECK_FALSE(f.flow->finished());

    // A repeated FIN is acked again but not reported again.
    f.peer(kTcpFin | kTcpAck, kPeerIss + 1, kIss + 1);
    CHECK(f.peer_closed == 1);
    REQUIRE(f.link.out.size() == 2);
    CHECK(f.link.out[1].ack == kPeerIss + 2);

    f.flow->write(bytes(3));
    REQUIRE(f.link.out.size() == 3);
    CHECK(f.link.out[2].payload == bytes(3));
    CHECK(f.link.out[2].ack == kPeerIss + 2);
}

TEST_CASE("tcp flow: data and FIN in one segment") {
    Fixture f;
    f.establish();
    f.peer(kTcpFin | kTcpAck, kPeerIss + 1, kIss + 1, bytes(4));
    CHECK(f.received == bytes(4));
    CHECK(f.peer_closed == 1);
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].ack == kPeerIss + 6);
}

TEST_CASE("tcp flow: a FIN beyond rcv_nxt waits") {
    Fixture f;
    f.establish();
    f.peer(kTcpFin | kTcpAck, kPeerIss + 5, kIss + 1);
    CHECK(f.peer_closed == 0);
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].ack == kPeerIss + 1);
}

TEST_CASE("tcp flow: peer FIN first, then our FIN acked ends the flow cleanly") {
    Fixture f;
    f.establish();
    f.peer(kTcpFin | kTcpAck, kPeerIss + 1, kIss + 1);
    f.link.out.clear();
    f.flow->close();
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == (kTcpFin | kTcpAck));
    CHECK(f.link.out[0].seq == kIss + 1);
    CHECK(f.link.out[0].ack == kPeerIss + 2);
    CHECK(f.flow->deadline() == kSec);
    CHECK_FALSE(f.flow->finished());
    f.peer(kTcpAck, kPeerIss + 2, kIss + 2);
    CHECK(f.flow->finished());
    CHECK(f.closed_calls == 1);
    CHECK_FALSE(f.closed_reset);
    CHECK_FALSE(f.flow->deadline());
}

TEST_CASE("tcp flow: our FIN first, acked, then the peer's FIN ends the flow after its ACK") {
    Fixture f;
    f.establish();
    f.flow->close();
    f.ack(kIss + 2);
    CHECK_FALSE(f.flow->finished());
    CHECK_FALSE(f.flow->deadline());
    f.link.out.clear();
    f.peer(kTcpFin | kTcpAck, kPeerIss + 1, kIss + 2);
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[0].seq == kIss + 2);
    CHECK(f.link.out[0].ack == kPeerIss + 2);
    CHECK(f.peer_closed == 1);
    CHECK(f.flow->finished());
    CHECK(f.closed_calls == 1);
    CHECK_FALSE(f.closed_reset);
}

TEST_CASE("tcp flow: FIN and its ACK in one segment from the peer") {
    Fixture f;
    f.establish();
    f.flow->close();
    f.link.out.clear();
    f.peer(kTcpFin | kTcpAck, kPeerIss + 1, kIss + 2);
    CHECK(f.flow->finished());
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].ack == kPeerIss + 2);
    CHECK(f.peer_closed == 1);
    CHECK_FALSE(f.closed_reset);
}

TEST_CASE("tcp flow: segments are cut at the MSS, PSH only on the last one of the buffer") {
    Fixture f;
    f.establish();
    f.flow->write(bytes(1500));
    REQUIRE(f.link.out.size() == 3);
    CHECK(f.link.out[0].payload == bytes(536));
    CHECK(f.link.out[1].payload == bytes(536, uint8_t(536)));
    CHECK(f.link.out[2].payload == bytes(428, uint8_t(1072)));
    CHECK(f.link.out[0].seq == Fixture::at(0));
    CHECK(f.link.out[1].seq == Fixture::at(536));
    CHECK(f.link.out[2].seq == Fixture::at(1072));
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[1].flags == kTcpAck);
    CHECK(f.link.out[2].flags == (kTcpAck | kTcpPsh));
    CHECK(f.link.out[0].ack == kPeerIss + 1);
    CHECK(f.flow->deadline() == kSec);
}

TEST_CASE("tcp flow: the peer's window limits what is in flight") {
    Fixture f;
    f.establish(1000, 1400);
    f.flow->write(bytes(1500));
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].payload.size() == 1000);
    CHECK(f.link.out[0].flags == kTcpAck);

    // The ACK opens the window again.
    f.ack(Fixture::at(1000), 1000);
    REQUIRE(f.link.out.size() == 2);
    CHECK(f.link.out[1].seq == Fixture::at(1000));
    CHECK(f.link.out[1].payload == bytes(500, uint8_t(1000)));
    CHECK(f.link.out[1].flags == (kTcpAck | kTcpPsh));
}

TEST_CASE("tcp flow: a duplicate ACK with a larger window releases more data") {
    Fixture f;
    f.establish(100, 1400);
    f.flow->write(bytes(300));
    REQUIRE(f.link.out.size() == 1);
    f.ack(Fixture::at(0), 250);
    REQUIRE(f.link.out.size() == 2);
    CHECK(f.link.out[1].seq == Fixture::at(100));
    CHECK(f.link.out[1].payload.size() == 150);
}

TEST_CASE("tcp flow: writing before the handshake is held back") {
    Fixture f;
    f.flow->write(bytes(10));
    CHECK(f.link.out.size() == 1);
    f.peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 1);
    REQUIRE(f.link.out.size() == 3);
    CHECK(f.link.out[1].flags == kTcpAck);
    CHECK(f.link.out[2].payload == bytes(10));
    CHECK(f.link.out[2].flags == (kTcpAck | kTcpPsh));
}

TEST_CASE("tcp flow: ACKs release the buffer and move the deadline") {
    Fixture f;
    f.establish();
    f.flow->write(bytes(100));
    CHECK(f.flow->buffered() == 100);
    CHECK(f.flow->deadline() == kSec);
    f.link.now = kSec * 4 / 10;
    f.ack(Fixture::at(40));
    CHECK(f.flow->buffered() == 60);
    CHECK(f.flow->deadline() == kSec * 14 / 10);
    // An old ACK and one beyond what was sent change nothing.
    f.ack(Fixture::at(40));
    f.ack(Fixture::at(101));
    CHECK(f.flow->buffered() == 60);
    CHECK(f.flow->deadline() == kSec * 14 / 10);
    f.ack(Fixture::at(100));
    CHECK(f.flow->buffered() == 0);
    CHECK_FALSE(f.flow->deadline());
}

TEST_CASE("tcp flow: the send buffer fills up at its limit") {
    Fixture f;
    f.establish(1000, 1400);
    CHECK(f.flow->can_write());
    f.flow->write(bytes(65535));
    CHECK(f.flow->can_write());
    f.flow->write(bytes(1));
    CHECK_FALSE(f.flow->can_write());
    CHECK(f.flow->buffered() == 65536);
    f.ack(Fixture::at(1000), 1000);
    CHECK(f.flow->can_write());
    CHECK(f.flow->buffered() == 64536);
}

TEST_CASE("tcp flow: nothing can be written after close") {
    Fixture f;
    f.establish();
    f.flow->close();
    CHECK_FALSE(f.flow->can_write());
    f.link.out.clear();
    f.flow->write(bytes(5));
    CHECK(f.link.out.empty());
    CHECK(f.flow->buffered() == 0);
}

TEST_CASE("tcp flow: retransmission backs off 1, 2, 4, 8, 16, 32 s and the sixth try ends the flow") {
    Fixture f;
    f.establish();
    f.flow->write(bytes(10));
    REQUIRE(f.link.out.size() == 1);

    const uint64_t deadlines[] = {1, 3, 7, 15, 31, 63};
    for (int i = 0; i < 6; ++i) {
        REQUIRE(f.flow->deadline() == deadlines[i] * kSec);
        // Early ticks do nothing.
        f.link.now = deadlines[i] * kSec - 1;
        f.flow->tick();
        CHECK(f.link.out.size() == size_t(i + 1));
        f.link.now = deadlines[i] * kSec;
        f.flow->tick();
        if (i < 5) {
            REQUIRE(f.link.out.size() == size_t(i + 2));
            CHECK(f.link.out.back().seq == Fixture::at(0));
            CHECK(f.link.out.back().payload == bytes(10));
            CHECK_FALSE(f.flow->finished());
        }
    }
    CHECK(f.link.out.size() == 6);
    CHECK(f.flow->finished());
    CHECK(f.closed_calls == 1);
    CHECK(f.closed_reset);
    CHECK_FALSE(f.flow->deadline());
}

TEST_CASE("tcp flow: new data acked resets the tries and the back-off") {
    Fixture f;
    f.establish();
    f.flow->write(bytes(100));
    f.link.now = kSec;
    f.flow->tick();
    CHECK(f.flow->deadline() == 3 * kSec);
    f.link.now = kSec * 15 / 10;
    f.ack(Fixture::at(50));
    CHECK(f.flow->deadline() == kSec * 25 / 10);
    f.link.now = kSec * 25 / 10;
    f.flow->tick();
    // One try used again: 1 s, not 4 s.
    CHECK(f.flow->deadline() == kSec * 45 / 10);
}

TEST_CASE("tcp flow: the SYN is repeated with the same back-off and the flow ends reset") {
    Fixture        f;
    const uint64_t deadlines[] = {1, 3, 7, 15, 31, 63};
    for (int i = 0; i < 6; ++i) {
        REQUIRE(f.flow->deadline() == deadlines[i] * kSec);
        f.link.now = deadlines[i] * kSec;
        f.flow->tick();
        if (i < 5) {
            REQUIRE(f.link.out.size() == size_t(i + 2));
            CHECK(f.link.out.back().flags == kTcpSyn);
            CHECK(f.link.out.back().seq == kIss);
            CHECK(f.link.out.back().mss == 1400);
        }
    }
    CHECK(f.link.out.size() == 6);
    CHECK(f.flow->finished());
    CHECK(f.closed_reset);
    CHECK(f.closed_calls == 1);
}

TEST_CASE("tcp flow: after a timeout the sender goes back to the oldest unacked byte") {
    Fixture f;
    f.establish(65535, 1000);
    f.flow->write(bytes(3000));
    REQUIRE(f.link.out.size() == 3);
    f.ack(Fixture::at(1000));
    f.link.out.clear();
    f.link.now = kSec;
    CHECK(f.flow->deadline() == kSec);
    f.flow->tick();
    REQUIRE(f.link.out.size() == 2);
    CHECK(f.link.out[0].seq == Fixture::at(1000));
    CHECK(f.link.out[0].payload == bytes(1000, uint8_t(1000 & 0xff)));
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[1].seq == Fixture::at(2000));
    CHECK(f.link.out[1].flags == (kTcpAck | kTcpPsh));
    // An ACK covering the resent range is accepted.
    f.ack(Fixture::at(3000));
    CHECK(f.flow->buffered() == 0);
    CHECK_FALSE(f.flow->deadline());
}

TEST_CASE("tcp flow: an ACK beyond the go-back-N restart point moves the restart point forward") {
    Fixture f;
    f.establish(2000, 1000);
    f.flow->write(bytes(3000));
    REQUIRE(f.link.out.size() == 2);
    // The window shrinks to 500 with the first partial ACK.
    f.ack(Fixture::at(500), 500);
    f.link.out.clear();
    f.link.now = kSec;
    f.flow->tick();
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].seq == Fixture::at(500));
    CHECK(f.link.out[0].payload.size() == 500);
    // The ACK of everything first sent covers more than was resent.
    f.link.out.clear();
    f.ack(Fixture::at(2000), 1000);
    CHECK(f.flow->buffered() == 1000);
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].seq == Fixture::at(2000));
    CHECK(f.link.out[0].payload.size() == 1000);
    CHECK(f.link.out[0].flags == (kTcpAck | kTcpPsh));
}

TEST_CASE("tcp flow: an unacked FIN is sent again") {
    Fixture f;
    f.establish();
    f.flow->write(bytes(5));
    f.flow->close();
    REQUIRE(f.link.out.size() == 2);
    CHECK(f.link.out[1].flags == (kTcpFin | kTcpAck));
    CHECK(f.link.out[1].seq == Fixture::at(5));
    CHECK(f.link.out[1].payload.empty());
    f.ack(Fixture::at(5));
    CHECK(f.flow->deadline());
    f.link.out.clear();
    f.link.now = 2 * kSec;
    f.flow->tick();
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == (kTcpFin | kTcpAck));
    CHECK(f.link.out[0].seq == Fixture::at(5));
    f.ack(Fixture::at(5) + 1);
    CHECK_FALSE(f.flow->deadline());
    f.link.now = 100 * kSec;
    f.flow->tick();
    CHECK(f.link.out.size() == 1);
}

TEST_CASE("tcp flow: close sends the FIN only after all buffered bytes went out") {
    Fixture f;
    f.establish(10, 1400);
    f.flow->write(bytes(25));
    f.flow->close();
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].payload.size() == 10);
    f.ack(Fixture::at(10), 10);
    REQUIRE(f.link.out.size() == 2);
    CHECK(f.link.out[1].payload.size() == 10);
    f.ack(Fixture::at(20), 10);
    REQUIRE(f.link.out.size() == 4);
    CHECK(f.link.out[2].payload.size() == 5);
    CHECK(f.link.out[2].flags == (kTcpAck | kTcpPsh));
    CHECK(f.link.out[3].flags == (kTcpFin | kTcpAck));
    CHECK(f.link.out[3].seq == Fixture::at(25));
}

TEST_CASE("tcp flow: close during the handshake sends the FIN once open") {
    Fixture f;
    f.flow->close();
    CHECK(f.link.out.size() == 1);
    f.peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 1);
    REQUIRE(f.link.out.size() == 3);
    CHECK(f.link.out[2].flags == (kTcpFin | kTcpAck));
}

TEST_CASE("tcp flow: a repeated SYN+ACK is acked again") {
    Fixture f;
    f.establish();
    f.peer(kTcpSyn | kTcpAck, kPeerIss, kIss + 1);
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == kTcpAck);
    CHECK(f.link.out[0].ack == kPeerIss + 1);
    CHECK(f.received.empty());
}

TEST_CASE("tcp flow: abort sends RST+ACK and ends without a callback") {
    Fixture f;
    f.establish();
    f.peer(kTcpAck, kPeerIss + 1, kIss + 1, bytes(7));
    f.flow->write(bytes(5));
    f.link.out.clear();
    f.flow->abort();
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].flags == (kTcpRst | kTcpAck));
    CHECK(f.link.out[0].seq == Fixture::at(5));
    CHECK(f.link.out[0].ack == kPeerIss + 8);
    CHECK(f.flow->finished());
    CHECK(f.closed_calls == 0);
    CHECK_FALSE(f.flow->deadline());
    f.flow->abort();
    CHECK(f.link.out.size() == 1);
}

TEST_CASE("tcp flow: abort during the handshake is silent, discard too") {
    {
        Fixture f;
        f.link.out.clear();
        f.flow->abort();
        CHECK(f.link.out.empty());
        CHECK(f.flow->finished());
        CHECK(f.closed_calls == 0);
    }
    {
        Fixture f;
        f.establish();
        f.flow->discard();
        CHECK(f.link.out.empty());
        CHECK(f.flow->finished());
        CHECK(f.closed_calls == 0);
    }
}

TEST_CASE("tcp flow: on_closed fires once, also when a callback aborts from inside delivery") {
    FakeLink link;
    int      closed = 0;
    TcpFlow *flow   = nullptr;
    TcpFlow  f(
        link, kLocal, kPeer, kIss, [&](std::span<const uint8_t>) { flow->abort(); }, nullptr, [&](bool) { ++closed; });
    flow = &f;
    f.start();
    f.receive(Tcp{kPeer, kLocal, kPeerIss, kIss + 1, kTcpSyn | kTcpAck, 1000, std::nullopt, {}});
    link.out.clear();
    const Bytes data = bytes(3);
    f.receive(Tcp{kPeer, kLocal, kPeerIss + 1, kIss + 1, kTcpAck, 1000, std::nullopt, data});
    CHECK(f.finished());
    CHECK(closed == 0);
    // Only the RST: the data segment's ACK is not sent after the abort.
    REQUIRE(link.out.size() == 1);
    CHECK(link.out[0].flags == (kTcpRst | kTcpAck));
}

TEST_CASE("tcp flow: sequence numbers wrap around") {
    Fixture f(0xfffffff0);
    f.link.out.clear();
    f.peer(kTcpSyn | kTcpAck, 0xffffffff, 0xfffffff1, {}, 65535, 1400);
    REQUIRE(f.flow->state() == TcpFlow::State::Open);
    f.link.out.clear();
    f.flow->write(bytes(40));
    REQUIRE(f.link.out.size() == 1);
    CHECK(f.link.out[0].seq == 0xfffffff1);
    CHECK(f.link.out[0].ack == 0);
    f.peer(kTcpAck, 0, 0xfffffff1 + 40, {}, 65535);
    CHECK(f.flow->buffered() == 0);
    f.peer(kTcpAck, 0, 0xfffffff1 + 40, bytes(4));
    CHECK(f.received == bytes(4));
}
