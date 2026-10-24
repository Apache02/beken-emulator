#include <doctest/doctest.h>

#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "net/packets.h"
#include "net/stack.h"

using namespace bk;
using namespace bk::packets;

namespace {

constexpr Mac kMe    = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr Mac kFw    = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
constexpr Mac kOther = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x02};

constexpr uint32_t kFwIp   = ip4(10, 0, 0, 1);
constexpr uint32_t kMeIp   = ip4(10, 0, 0, 2);
constexpr uint32_t kOther4 = ip4(10, 0, 0, 77);

constexpr uint64_t kSec = 1'000'000'000;

Bytes sub(std::span<const uint8_t> s) { return Bytes(s.begin(), s.end()); }

// What the stack sent, taken apart.
struct Sent {
    Ethernet eth;
    Ipv4     ip;
    Udp      udp;
    Dhcp     dhcp;
};

// A server message as the firmware's DHCP server would send it.
Bytes dhcp_reply(
    uint8_t type, uint32_t xid, const Mac &chaddr, uint32_t yiaddr, std::optional<uint32_t> server_id = kFwIp) {
    Bytes p(236, 0);
    p[0] = 2;
    p[1] = 1;
    p[2] = 6;
    for (int i = 0; i < 4; ++i) {
        p[4 + i]  = uint8_t(xid >> (24 - 8 * i));
        p[16 + i] = uint8_t(yiaddr >> (24 - 8 * i));
    }
    std::copy(chaddr.begin(), chaddr.end(), p.begin() + 28);
    p.insert(p.end(), {0x63, 0x82, 0x53, 0x63, 53, 1, type});
    if (server_id) {
        p.insert(p.end(), {54, 4});
        for (int i = 0; i < 4; ++i) p.push_back(uint8_t(*server_id >> (24 - 8 * i)));
    }
    p.push_back(255);
    return p;
}

Bytes udp_frame(
    const Mac &dst, const Mac &src, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, const Bytes &data) {
    return build_ethernet(dst, src, kEtherIpv4, build_ipv4(sip, dip, kProtoUdp, 1, build_udp(sip, dip, sp, dp, data)));
}

struct Fixture {
    std::vector<Bytes> out;
    Stack              stack;
    uint64_t           now = 0;

    Fixture() : stack(kMe, [this](Bytes f) { out.push_back(std::move(f)); }) {}

    // The last frame sent, as a DHCP message.
    std::pair<Ethernet, Dhcp> last_dhcp() {
        REQUIRE_FALSE(out.empty());
        auto eth = parse_ethernet(out.back());
        REQUIRE(eth);
        auto ip = parse_ipv4(eth->payload);
        REQUIRE(ip);
        auto udp = parse_udp(ip->src, ip->dst, ip->payload);
        REQUIRE(udp);
        CHECK(ip->src == 0);
        CHECK(ip->dst == 0xffffffff);
        CHECK(udp->src_port == 68);
        CHECK(udp->dst_port == 67);
        auto d = parse_dhcp(udp->payload);
        REQUIRE(d);
        return {*eth, *d};
    }

    void server(uint8_t                 type,
                uint32_t                xid,
                const Mac              &chaddr,
                uint32_t                yiaddr = kMeIp,
                std::optional<uint32_t> sid    = kFwIp,
                const Mac              &src    = kFw) {
        stack.receive(
            udp_frame(kBroadcastMac, src, kFwIp, 0xffffffff, 67, 68, dhcp_reply(type, xid, chaddr, yiaddr, sid)), now);
    }

    void bind() {
        stack.link_up(now);
        uint32_t xid = last_dhcp().second.xid;
        server(DhcpOffer, xid, kMe);
        server(DhcpAck, xid, kMe);
        REQUIRE(stack.dhcp_state() == Stack::Dhcp::Bound);
        out.clear();
    }

    void fw(const Bytes &f) { stack.receive(f, now); }
};

} // namespace

TEST_CASE("stack: link_up sends a broadcast DISCOVER and arms the retry") {
    Fixture f;
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::None);
    CHECK_FALSE(f.stack.next_event_ns());
    f.now = 5 * kSec;
    f.stack.link_up(f.now);
    REQUIRE(f.out.size() == 1);
    auto [eth, d] = f.last_dhcp();
    CHECK(eth.dst == kBroadcastMac);
    CHECK(eth.src == kMe);
    CHECK(d.op == 1);
    CHECK(d.type == DhcpDiscover);
    CHECK(d.xid == 1);
    CHECK(d.chaddr == kMe);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);
    CHECK(f.stack.next_event_ns() == 8 * kSec);
    // The whole frame is the exact client layout.
    auto ip  = parse_ipv4(eth.payload);
    auto udp = parse_udp(ip->src, ip->dst, ip->payload);
    CHECK(sub(udp->payload) == build_dhcp_client(DhcpDiscover, 1, kMe, std::nullopt, std::nullopt));
}

TEST_CASE("stack: offer, request and ack bind the address") {
    Fixture f;
    f.stack.link_up(0);
    f.now = kSec;
    f.server(DhcpOffer, 1, kMe, kMeIp, kFwIp, kOther);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Requesting);
    REQUIRE(f.out.size() == 2);
    auto [eth, d] = f.last_dhcp();
    CHECK(eth.dst == kBroadcastMac);
    CHECK(d.type == DhcpRequest);
    CHECK(d.xid == 1);
    CHECK(d.server_id == kFwIp);
    auto ip  = parse_ipv4(eth.payload);
    auto udp = parse_udp(ip->src, ip->dst, ip->payload);
    CHECK(sub(udp->payload) == build_dhcp_client(DhcpRequest, 1, kMe, kMeIp, kFwIp));
    CHECK(f.stack.next_event_ns() == 4 * kSec);

    f.now = 2 * kSec;
    f.server(DhcpAck, 1, kMe, kMeIp, kFwIp, kFw);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Bound);
    CHECK(f.stack.ip() == kMeIp);
    CHECK(f.stack.guest_ip() == kFwIp);
    // The MAC remembered from the offer.
    CHECK(f.stack.guest_mac() == kOther);
    CHECK_FALSE(f.stack.next_event_ns());
}

TEST_CASE("stack: replies that are not for this exchange are ignored") {
    Fixture f;
    f.stack.link_up(0);
    f.out.clear();
    f.server(DhcpOffer, 2, kMe);                      // wrong xid
    f.server(DhcpOffer, 1, kOther);                   // wrong chaddr
    f.server(DhcpOffer, 1, kMe, kMeIp, std::nullopt); // no server id
    f.server(DhcpAck, 1, kMe);                        // an ACK while discovering
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);
    CHECK(f.out.empty());

    // op must be 2.
    Bytes req = dhcp_reply(DhcpOffer, 1, kMe, kMeIp);
    req[0]    = 1;
    f.fw(udp_frame(kBroadcastMac, kFw, kFwIp, 0xffffffff, 67, 68, req));
    // Wrong destination port.
    f.fw(udp_frame(kBroadcastMac, kFw, kFwIp, 0xffffffff, 67, 6868, dhcp_reply(DhcpOffer, 1, kMe, kMeIp)));
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);
    CHECK(f.out.empty());

    // Not addressed to us at the Ethernet level.
    f.stack.receive(udp_frame(kOther, kFw, kFwIp, 0xffffffff, 67, 68, dhcp_reply(DhcpOffer, 1, kMe, kMeIp)), 0);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);

    // Offers while requesting and ACKs once bound do nothing.
    f.server(DhcpOffer, 1, kMe);
    REQUIRE(f.stack.dhcp_state() == Stack::Dhcp::Requesting);
    f.out.clear();
    f.server(DhcpOffer, 1, kMe, ip4(10, 0, 0, 3));
    CHECK(f.out.empty());
    f.server(DhcpAck, 1, kMe);
    REQUIRE(f.stack.dhcp_state() == Stack::Dhcp::Bound);
    CHECK(f.stack.ip() == kMeIp);
    f.server(DhcpNak, 1, kMe);
    f.server(DhcpAck, 1, kMe, ip4(10, 0, 0, 3));
    CHECK(f.stack.ip() == kMeIp);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Bound);
}

TEST_CASE("stack: replies unicast to the offered address are accepted too") {
    Fixture f;
    f.stack.link_up(0);
    f.stack.receive(udp_frame(kMe, kFw, kFwIp, kMeIp, 67, 68, dhcp_reply(DhcpOffer, 1, kMe, kMeIp)), 0);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Requesting);
}

TEST_CASE("stack: a NAK starts again with a new xid") {
    Fixture f;
    f.stack.link_up(0);
    f.server(DhcpOffer, 1, kMe);
    f.now = kSec;
    f.server(DhcpNak, 1, kMe);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);
    auto [eth, d] = f.last_dhcp();
    CHECK(d.type == DhcpDiscover);
    CHECK(d.xid == 2);
    CHECK(f.stack.next_event_ns() == 4 * kSec);
    // A NAK while discovering restarts too.
    f.server(DhcpNak, 2, kMe);
    CHECK(f.last_dhcp().second.xid == 3);
}

TEST_CASE("stack: DHCP is retried every 3 seconds") {
    Fixture f;
    f.stack.link_up(0);
    f.stack.tick(3 * kSec - 1);
    CHECK(f.out.size() == 1);
    f.stack.tick(3 * kSec);
    REQUIRE(f.out.size() == 2);
    CHECK(f.last_dhcp().second.type == DhcpDiscover);
    CHECK(f.last_dhcp().second.xid == 2);
    CHECK(f.stack.next_event_ns() == 6 * kSec);

    // The reply to the first discover is stale now.
    f.now = 4 * kSec;
    f.server(DhcpOffer, 1, kMe);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);
    f.server(DhcpOffer, 2, kMe);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Requesting);

    // An unanswered request falls back to a discover.
    f.stack.tick(7 * kSec);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);
    CHECK(f.last_dhcp().second.type == DhcpDiscover);
    CHECK(f.last_dhcp().second.xid == 3);
}

TEST_CASE("stack: no ticking once bound") {
    Fixture f;
    f.bind();
    f.stack.tick(100 * kSec);
    CHECK(f.out.empty());
    CHECK_FALSE(f.stack.next_event_ns());
}

TEST_CASE("stack: ARP requests for our address are answered") {
    Fixture f;
    f.bind();
    Arp req{1, kFw, kFwIp, Mac{}, kMeIp};
    f.fw(build_ethernet(kBroadcastMac, kFw, kEtherArp, build_arp(req)));
    REQUIRE(f.out.size() == 1);
    auto eth = parse_ethernet(f.out[0]);
    REQUIRE(eth);
    CHECK(eth->dst == kFw);
    CHECK(eth->src == kMe);
    CHECK(eth->type == kEtherArp);
    auto a = parse_arp(eth->payload);
    REQUIRE(a);
    CHECK(a->op == 2);
    CHECK(a->sha == kMe);
    CHECK(a->spa == kMeIp);
    CHECK(a->tha == kFw);
    CHECK(a->tpa == kFwIp);
}

TEST_CASE("stack: ARP for other addresses, replies and other MACs get no answer") {
    Fixture f;
    f.bind();
    f.fw(build_ethernet(kBroadcastMac, kFw, kEtherArp, build_arp({1, kFw, kFwIp, Mac{}, kOther4})));
    f.fw(build_ethernet(kMe, kFw, kEtherArp, build_arp({2, kFw, kFwIp, kMe, kMeIp})));
    f.fw(build_ethernet(kOther, kFw, kEtherArp, build_arp({1, kFw, kFwIp, Mac{}, kMeIp})));
    CHECK(f.out.empty());
}

TEST_CASE("stack: ARP from the firmware's address teaches its MAC") {
    Fixture f;
    f.bind();
    CHECK(f.stack.guest_mac() == kFw);
    f.fw(build_ethernet(kBroadcastMac, kOther, kEtherArp, build_arp({1, kOther, kFwIp, Mac{}, kOther4})));
    CHECK(f.stack.guest_mac() == kOther);
    f.fw(build_ethernet(kMe, kFw, kEtherArp, build_arp({2, kFw, kFwIp, kMe, kMeIp})));
    CHECK(f.stack.guest_mac() == kFw);
    // Someone else's address does not.
    f.fw(build_ethernet(kBroadcastMac, kOther, kEtherArp, build_arp({1, kOther, kOther4, Mac{}, kOther4})));
    CHECK(f.stack.guest_mac() == kFw);
    // Traffic then goes to the learned MAC.
    f.fw(build_ethernet(kBroadcastMac, kOther, kEtherArp, build_arp({1, kOther, kFwIp, Mac{}, kOther4})));
    f.stack.send_udp(40000, 1, Bytes{1});
    REQUIRE(f.out.size() == 1);
    CHECK(parse_ethernet(f.out[0])->dst == kOther);
}

TEST_CASE("stack: no ARP answers before an address") {
    Fixture f;
    f.stack.link_up(0);
    f.out.clear();
    f.fw(build_ethernet(kBroadcastMac, kFw, kEtherArp, build_arp({1, kFw, kFwIp, Mac{}, 0})));
    CHECK(f.out.empty());
}

TEST_CASE("stack: ping is answered") {
    Fixture f;
    f.bind();
    const Bytes data = {1, 2, 3, 4};
    Bytes       echo = build_icmp_echo({false, 0xabcd, 5, data});
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, build_ipv4(kFwIp, kMeIp, kProtoIcmp, 9, echo)));
    REQUIRE(f.out.size() == 1);
    auto eth = parse_ethernet(f.out[0]);
    REQUIRE(eth);
    CHECK(eth->dst == kFw);
    auto ip = parse_ipv4(eth->payload);
    REQUIRE(ip);
    CHECK(ip->src == kMeIp);
    CHECK(ip->dst == kFwIp);
    CHECK(ip->protocol == kProtoIcmp);
    CHECK(ip->ttl == 64);
    auto r = parse_icmp_echo(ip->payload);
    REQUIRE(r);
    CHECK(r->reply);
    CHECK(r->id == 0xabcd);
    CHECK(r->seq == 5);
    CHECK(sub(r->data) == data);
}

TEST_CASE("stack: ping for other addresses, replies and corrupt echoes are not answered") {
    Fixture f;
    f.bind();
    Bytes echo = build_icmp_echo({false, 1, 1, Bytes{}});
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, build_ipv4(kFwIp, kOther4, kProtoIcmp, 9, echo)));
    f.fw(build_ethernet(
        kMe, kFw, kEtherIpv4, build_ipv4(kFwIp, kMeIp, kProtoIcmp, 9, build_icmp_echo({true, 1, 1, Bytes{}}))));
    Bytes bad = echo;
    bad[4] ^= 1;
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, build_ipv4(kFwIp, kMeIp, kProtoIcmp, 9, bad)));
    // Wrong destination MAC.
    f.fw(build_ethernet(kOther, kFw, kEtherIpv4, build_ipv4(kFwIp, kMeIp, kProtoIcmp, 9, echo)));
    // Corrupt IP header.
    Bytes ip = build_ipv4(kFwIp, kMeIp, kProtoIcmp, 9, echo);
    ip[8] ^= 1;
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, ip));
    CHECK(f.out.empty());
}

TEST_CASE("stack: nothing is answered while unbound") {
    Fixture f;
    f.stack.link_up(0);
    f.out.clear();
    Bytes echo = build_icmp_echo({false, 1, 1, Bytes{}});
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, build_ipv4(kFwIp, 0, kProtoIcmp, 9, echo)));
    CHECK(f.out.empty());
}

TEST_CASE("stack: UDP is delivered to the bound handler") {
    Fixture f;
    f.bind();
    std::vector<std::pair<uint16_t, Bytes>> got;
    f.stack.bind_udp(5000, [&](uint16_t sp, std::span<const uint8_t> d) { got.emplace_back(sp, sub(d)); });
    f.fw(udp_frame(kMe, kFw, kFwIp, kMeIp, 7777, 5000, {1, 2, 3}));
    REQUIRE(got.size() == 1);
    CHECK(got[0].first == 7777);
    CHECK(got[0].second == Bytes{1, 2, 3});

    // Other port, other address, bad checksum: dropped.
    f.fw(udp_frame(kMe, kFw, kFwIp, kMeIp, 7777, 5001, {1}));
    f.fw(udp_frame(kMe, kFw, kFwIp, kOther4, 7777, 5000, {1}));
    Bytes d = build_udp(kFwIp, kMeIp, 7777, 5000, Bytes{1, 2});
    d[8] ^= 1;
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, build_ipv4(kFwIp, kMeIp, kProtoUdp, 1, d)));
    CHECK(got.size() == 1);

    // A zero checksum is fine.
    Bytes z = build_udp(kFwIp, kMeIp, 7777, 5000, Bytes{4});
    z[6] = z[7] = 0;
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, build_ipv4(kFwIp, kMeIp, kProtoUdp, 1, z)));
    CHECK(got.size() == 2);

    f.stack.unbind_udp(5000);
    f.fw(udp_frame(kMe, kFw, kFwIp, kMeIp, 7777, 5000, {1}));
    CHECK(got.size() == 2);
}

TEST_CASE("stack: a handler may unbind itself") {
    Fixture f;
    f.bind();
    int n = 0;
    f.stack.bind_udp(5000, [&](uint16_t, std::span<const uint8_t>) {
        ++n;
        f.stack.unbind_udp(5000);
    });
    f.fw(udp_frame(kMe, kFw, kFwIp, kMeIp, 1, 5000, {1}));
    f.fw(udp_frame(kMe, kFw, kFwIp, kMeIp, 1, 5000, {1}));
    CHECK(n == 1);
}

TEST_CASE("stack: send_udp is false until bound, then goes to the firmware") {
    Fixture f;
    CHECK_FALSE(f.stack.send_udp(40000, 53, Bytes{1}));
    f.stack.link_up(0);
    f.out.clear();
    CHECK_FALSE(f.stack.send_udp(40000, 53, Bytes{1}));
    CHECK(f.out.empty());

    f.bind();
    const Bytes data = {'a', 'b', 'c'};
    CHECK(f.stack.send_udp(40001, 53, data));
    REQUIRE(f.out.size() == 1);
    auto eth = parse_ethernet(f.out[0]);
    REQUIRE(eth);
    CHECK(eth->dst == kFw);
    CHECK(eth->src == kMe);
    auto ip = parse_ipv4(eth->payload);
    REQUIRE(ip);
    CHECK(ip->src == kMeIp);
    CHECK(ip->dst == kFwIp);
    CHECK(ip->protocol == kProtoUdp);
    // DF set, header checksum good (parse), identification counts per packet.
    CHECK(eth->payload[6] == 0x40);
    CHECK(eth->payload[7] == 0x00);
    auto udp = parse_udp(ip->src, ip->dst, ip->payload);
    REQUIRE(udp);
    CHECK(udp->src_port == 40001);
    CHECK(udp->dst_port == 53);
    CHECK(sub(udp->payload) == data);

    f.stack.send_udp(40001, 53, data);
    REQUIRE(f.out.size() == 2);
    CHECK(parse_ipv4(parse_ethernet(f.out[1])->payload)->ident == uint16_t(ip->ident + 1));
}

TEST_CASE("stack: ports are handed out from 40000, skipping bound ones") {
    Fixture f;
    CHECK(f.stack.allocate_port() == 40000);
    CHECK(f.stack.allocate_port() == 40001);
    f.stack.bind_udp(40002, [](uint16_t, std::span<const uint8_t>) {});
    CHECK(f.stack.allocate_port() == 40003);
    f.stack.unbind_udp(40002);
    CHECK(f.stack.allocate_port() == 40004);
}

TEST_CASE("stack: reset forgets the address, bindings, ports and timers") {
    Fixture f;
    f.bind();
    f.stack.bind_udp(5000, [](uint16_t, std::span<const uint8_t>) {});
    f.stack.allocate_port();
    f.stack.reset();
    CHECK(f.stack.ip() == 0);
    CHECK(f.stack.guest_ip() == 0);
    CHECK(f.stack.guest_mac() == Mac{});
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::None);
    CHECK_FALSE(f.stack.next_event_ns());
    CHECK(f.stack.allocate_port() == 40000);
    CHECK_FALSE(f.stack.send_udp(40000, 1, Bytes{1}));
    f.out.clear();
    f.stack.tick(100 * kSec);
    CHECK(f.out.empty());
}

TEST_CASE("stack: link_up starts over, also when bound") {
    Fixture f;
    f.bind();
    f.stack.bind_udp(5000, [](uint16_t, std::span<const uint8_t>) {});
    f.now = 10 * kSec;
    f.stack.link_up(f.now);
    CHECK(f.stack.ip() == 0);
    CHECK(f.stack.dhcp_state() == Stack::Dhcp::Discovering);
    CHECK(f.stack.next_event_ns() == 13 * kSec);
    CHECK(f.stack.allocate_port() == 40000);
    CHECK(f.last_dhcp().second.type == DhcpDiscover);
}

TEST_CASE("stack: runt and garbage frames are ignored") {
    Fixture f;
    f.bind();
    f.fw(Bytes{});
    f.fw(Bytes(13, 0xff));
    f.fw(build_ethernet(kMe, kFw, 0x86dd, Bytes(40, 0)));
    f.fw(build_ethernet(kMe, kFw, kEtherIpv4, Bytes(5, 0)));
    CHECK(f.out.empty());
}

namespace {

Bytes tcp_frame(const Mac &dst, const Mac &src, uint32_t sip, uint32_t dip, const Tcp &tcp) {
    return build_ethernet(dst, src, kEtherIpv4, build_ipv4(sip, dip, kProtoTcp, 1, build_tcp(sip, dip, tcp)));
}

// A TCP segment from the firmware to the stack.
Bytes from_fw(uint16_t                src_port,
              uint16_t                dst_port,
              uint32_t                seq,
              uint32_t                ack,
              uint8_t                 flags,
              const Bytes            &data = {},
              std::optional<uint16_t> mss  = std::nullopt) {
    return tcp_frame(kMe, kFw, kFwIp, kMeIp, Tcp{src_port, dst_port, seq, ack, flags, 4096, mss, data});
}

// A frame the stack sent, as a TCP segment (the spans point into the frame).
struct SentTcp {
    Ethernet eth;
    Ipv4     ip;
    Tcp      tcp;
};
SentTcp parse_sent(const Bytes &frame) {
    auto eth = parse_ethernet(frame);
    REQUIRE(eth);
    auto ip = parse_ipv4(eth->payload);
    REQUIRE(ip);
    REQUIRE(ip->protocol == kProtoTcp);
    auto tcp = parse_tcp(ip->src, ip->dst, ip->payload);
    REQUIRE(tcp);
    return {*eth, *ip, *tcp};
}

// The firmware's end of a connection: accepts, stores what arrives in order,
// answers the first FIN with data and its own FIN.
struct FakePeer {
    explicit FakePeer(Fixture &fixture) : f(fixture) {}

    Fixture &f;
    size_t   seen    = 0;
    uint32_t iss     = 7000;
    uint32_t rcv_nxt = 0;
    uint32_t snd_nxt = 0;
    Bytes    got;
    bool     got_fin = false;
    int      dropped = 0; // frames to swallow, simulating loss

    void pump() {
        while (seen < f.out.size()) {
            Bytes frame = f.out[seen++];
            if (dropped > 0) {
                --dropped;
                continue;
            }
            handle(frame);
        }
    }

    void reply(const SentTcp          &s,
               uint32_t                seq,
               uint8_t                 flags,
               const Bytes            &data = {},
               std::optional<uint16_t> mss  = std::nullopt) {
        f.fw(from_fw(s.tcp.dst_port, s.tcp.src_port, seq, rcv_nxt, flags, data, mss));
    }

    void handle(const Bytes &frame) {
        SentTcp s = parse_sent(frame);
        if (s.tcp.flags & kTcpSyn) {
            rcv_nxt = s.tcp.seq + 1;
            snd_nxt = iss + 1;
            reply(s, iss, kTcpSyn | kTcpAck, {}, 1460);
            return;
        }
        bool fin = s.tcp.flags & kTcpFin;
        if (s.tcp.payload.empty() && !fin) return;
        if (s.tcp.seq != rcv_nxt) {
            reply(s, snd_nxt, kTcpAck);
            return;
        }
        got.insert(got.end(), s.tcp.payload.begin(), s.tcp.payload.end());
        rcv_nxt += uint32_t(s.tcp.payload.size());
        if (!fin) {
            reply(s, snd_nxt, kTcpAck);
            return;
        }
        got_fin = true;
        ++rcv_nxt;
        const Bytes bye = {'b', 'y', 'e'};
        reply(s, snd_nxt, kTcpFin | kTcpAck | kTcpPsh, bye);
        snd_nxt += 4;
    }
};

} // namespace

TEST_CASE("stack tcp: open_tcp is null until the address is bound and sends nothing") {
    Fixture f;
    CHECK_FALSE(f.stack.open_tcp(80, nullptr, nullptr, nullptr, 0));
    f.stack.link_up(0);
    f.out.clear();
    CHECK_FALSE(f.stack.open_tcp(80, nullptr, nullptr, nullptr, 0));
    CHECK(f.out.empty());
    // The refused attempt took neither a port nor a sequence number.
    f.bind();
    auto flow = f.stack.open_tcp(80, nullptr, nullptr, nullptr, 0);
    REQUIRE(flow);
    CHECK(flow->local_port() == 40000);
    CHECK(parse_sent(f.out.back()).tcp.seq == 0);
}

TEST_CASE("stack tcp: open_tcp sends a SYN to the firmware, the sequence number grows by 100000") {
    Fixture f;
    f.bind();
    f.now  = 5 * kSec;
    auto a = f.stack.open_tcp(8080, nullptr, nullptr, nullptr, f.now);
    REQUIRE(a);
    REQUIRE(f.out.size() == 1);
    SentTcp s = parse_sent(f.out[0]);
    CHECK(s.eth.dst == kFw);
    CHECK(s.eth.src == kMe);
    CHECK(s.ip.src == kMeIp);
    CHECK(s.ip.dst == kFwIp);
    CHECK(s.tcp.src_port == 40000);
    CHECK(s.tcp.dst_port == 8080);
    CHECK(s.tcp.seq == 0);
    CHECK(s.tcp.ack == 0);
    CHECK(s.tcp.flags == kTcpSyn);
    CHECK(s.tcp.window == 16384);
    CHECK(s.tcp.mss == 1400);
    CHECK(s.tcp.payload.empty());

    auto b = f.stack.open_tcp(8081, nullptr, nullptr, nullptr, f.now);
    REQUIRE(b);
    s = parse_sent(f.out[1]);
    CHECK(s.tcp.src_port == 40001);
    CHECK(s.tcp.dst_port == 8081);
    CHECK(s.tcp.seq == 100000);
    CHECK(f.stack.open_tcp(8082, nullptr, nullptr, nullptr, f.now));
    CHECK(parse_sent(f.out[2]).tcp.seq == 200000);
    CHECK(a != b);
}

TEST_CASE("stack tcp: allocate_port skips ports of flows") {
    Fixture f;
    f.bind();
    auto flow = f.stack.open_tcp(80, nullptr, nullptr, nullptr, 0);
    REQUIRE(flow);
    CHECK(flow->local_port() == 40000);
    // The counter has moved on, but a wrap-around must not land on the flow.
    for (int i = 1; i < 0x10000 - 40000; ++i) f.stack.allocate_port();
    CHECK(f.stack.allocate_port() == 40001);
}

TEST_CASE("stack tcp: a segment for a flow reaches it by destination port") {
    Fixture f;
    f.bind();
    Bytes               got;
    bool                peer_closed = false;
    std::optional<bool> closed;
    auto                flow = f.stack.open_tcp(
        80,
        [&](std::span<const uint8_t> d) { got.insert(got.end(), d.begin(), d.end()); },
        [&] { peer_closed = true; },
        [&](bool reset) { closed = reset; },
        0);
    REQUIRE(flow);
    f.out.clear();

    // Wrong checksum and a foreign destination address are dropped.
    Bytes bad = from_fw(80, 40000, 7000, 1, kTcpSyn | kTcpAck, {}, 1460);
    bad[bad.size() - 5] ^= 0xff;
    f.fw(bad);
    f.fw(tcp_frame(kMe, kFw, kFwIp, kOther4, Tcp{80, 40000, 7000, 1, kTcpSyn | kTcpAck, 4096, std::nullopt, {}}));
    CHECK(flow->state() == TcpFlow::State::SynSent);
    CHECK(f.out.empty());

    f.fw(from_fw(80, 40000, 7000, 1, kTcpSyn | kTcpAck, {}, 1460));
    CHECK(flow->state() == TcpFlow::State::Open);
    REQUIRE(f.out.size() == 1);
    SentTcp ack = parse_sent(f.out[0]);
    CHECK(ack.tcp.flags == kTcpAck);
    CHECK(ack.tcp.seq == 1);
    CHECK(ack.tcp.ack == 7001);

    f.fw(from_fw(80, 40000, 7001, 1, kTcpAck | kTcpFin, Bytes{'o', 'k'}));
    CHECK(got == Bytes{'o', 'k'});
    CHECK(peer_closed);
    CHECK_FALSE(closed);
}

TEST_CASE("stack tcp: a segment without a flow gets an RST, with ACK: seq = its ack") {
    Fixture f;
    f.bind();
    f.fw(from_fw(1234, 5555, 100, 555, kTcpAck, Bytes{1, 2, 3}));
    REQUIRE(f.out.size() == 1);
    SentTcp r = parse_sent(f.out[0]);
    CHECK(r.eth.dst == kFw);
    CHECK(r.ip.src == kMeIp);
    CHECK(r.ip.dst == kFwIp);
    CHECK(r.tcp.src_port == 5555);
    CHECK(r.tcp.dst_port == 1234);
    CHECK(r.tcp.flags == kTcpRst);
    CHECK(r.tcp.seq == 555);
    CHECK(r.tcp.ack == 0);
    CHECK(r.tcp.payload.empty());
}

TEST_CASE("stack tcp: a segment without a flow and without ACK gets RST+ACK acking its length") {
    Fixture f;
    f.bind();
    f.fw(from_fw(1234, 5555, 100, 0, kTcpSyn, {}, 1460));
    REQUIRE(f.out.size() == 1);
    SentTcp r = parse_sent(f.out[0]);
    CHECK(r.tcp.flags == (kTcpRst | kTcpAck));
    CHECK(r.tcp.seq == 0);
    CHECK(r.tcp.ack == 101);

    f.fw(from_fw(1234, 5555, 200, 0, kTcpPsh, Bytes(5, 9)));
    REQUIRE(f.out.size() == 2);
    r = parse_sent(f.out[1]);
    CHECK(r.tcp.flags == (kTcpRst | kTcpAck));
    CHECK(r.tcp.seq == 0);
    CHECK(r.tcp.ack == 205);
}

TEST_CASE("stack tcp: an RST without a flow is not answered, nor is anything before the address") {
    Fixture f;
    f.stack.link_up(0);
    f.out.clear();
    f.fw(from_fw(1234, 5555, 100, 0, kTcpSyn));
    CHECK(f.out.empty());
    f.bind();
    f.fw(from_fw(1234, 5555, 100, 0, kTcpRst));
    f.fw(from_fw(1234, 5555, 100, 9, kTcpRst | kTcpAck));
    CHECK(f.out.empty());
    // Bad checksum: nothing either.
    Bytes bad = from_fw(1234, 5555, 100, 0, kTcpSyn);
    bad.back() ^= 0xff;
    f.fw(bad);
    CHECK(f.out.empty());
}

TEST_CASE("stack tcp: tick runs the flow timers and next_event_ns shows the earliest deadline") {
    Fixture f;
    f.bind();
    CHECK_FALSE(f.stack.next_event_ns());
    f.now     = 5 * kSec;
    auto flow = f.stack.open_tcp(80, nullptr, nullptr, nullptr, f.now);
    REQUIRE(flow);
    CHECK(f.stack.next_event_ns() == 6 * kSec);
    f.out.clear();
    f.stack.tick(6 * kSec - 1);
    CHECK(f.out.empty());
    f.stack.tick(6 * kSec);
    REQUIRE(f.out.size() == 1);
    CHECK(parse_sent(f.out[0]).tcp.flags == kTcpSyn);
    CHECK(f.stack.next_event_ns() == 8 * kSec);

    // A second flow with an earlier deadline wins.
    f.now = 6 * kSec;
    REQUIRE(f.stack.open_tcp(81, nullptr, nullptr, nullptr, f.now));
    CHECK(f.stack.next_event_ns() == 7 * kSec);
}

TEST_CASE("stack tcp: an aborted flow no longer holds a deadline") {
    Fixture f;
    f.bind();
    auto flow = f.stack.open_tcp(80, nullptr, nullptr, nullptr, 0);
    REQUIRE(flow);
    CHECK(f.stack.next_event_ns() == kSec);
    flow->abort();
    CHECK_FALSE(f.stack.next_event_ns());
}

TEST_CASE("stack tcp: finished flows leave on the next tick") {
    Fixture f;
    f.bind();
    std::optional<bool> closed;
    auto                flow = f.stack.open_tcp(80, nullptr, nullptr, [&](bool r) { closed = r; }, 0);
    REQUIRE(flow);
    f.fw(from_fw(80, 40000, 0, 1, kTcpRst | kTcpAck));
    CHECK(flow->finished());
    CHECK(closed == true);
    f.out.clear();
    f.stack.tick(1);
    // The port is no longer a flow's: a late segment gets an RST again.
    f.fw(from_fw(80, 40000, 9, 1, kTcpAck, Bytes{1}));
    REQUIRE(f.out.size() == 1);
    CHECK(parse_sent(f.out[0]).tcp.flags == kTcpRst);
    // The handle is inert.
    f.out.clear();
    flow->write(Bytes{1});
    flow->abort();
    CHECK(f.out.empty());
}

TEST_CASE("stack tcp: reset and link_up drop the flows silently and restart ports and sequence numbers") {
    for (bool by_link_up : {false, true}) {
        Fixture f;
        f.bind();
        int  closed = 0;
        auto flow   = f.stack.open_tcp(80, nullptr, nullptr, [&](bool) { ++closed; }, 0);
        REQUIRE(flow);
        f.stack.open_tcp(81, nullptr, nullptr, nullptr, 0);
        f.out.clear();
        if (by_link_up)
            f.stack.link_up(0);
        else
            f.stack.reset();
        CHECK(closed == 0);
        CHECK(flow->finished());
        CHECK(f.stack.allocate_port() == 40000);
        flow->write(Bytes{1});
        flow->abort();
        if (!by_link_up) {
            CHECK(f.out.empty());
            CHECK_FALSE(f.stack.next_event_ns());
        }
        f.bind();
        auto again = f.stack.open_tcp(80, nullptr, nullptr, nullptr, 0);
        REQUIRE(again);
        // bind() restarts the link, so the first flow of the new run is port 40000 / ISS 0.
        CHECK(again->local_port() == 40000);
        CHECK(parse_sent(f.out.back()).tcp.seq == 0);
    }
}

TEST_CASE("stack tcp: a whole connection with a tiny peer: handshake, data, close, FIN back") {
    Fixture f;
    f.bind();
    FakePeer            peer{f};
    Bytes               got;
    int                 peer_closed = 0;
    std::optional<bool> closed;
    auto                flow = f.stack.open_tcp(
        80,
        [&](std::span<const uint8_t> d) { got.insert(got.end(), d.begin(), d.end()); },
        [&] { ++peer_closed; },
        [&](bool reset) { closed = reset; },
        f.now);
    REQUIRE(flow);
    peer.pump();
    REQUIRE(flow->state() == TcpFlow::State::Open);
    CHECK(flow->mss() == 1400);

    Bytes data(3000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7);
    flow->write(data);
    peer.pump();
    CHECK(peer.got == data);
    CHECK(flow->buffered() == 0);
    CHECK_FALSE(flow->deadline());

    flow->close();
    peer.pump();
    CHECK(peer.got_fin);
    CHECK(got == Bytes{'b', 'y', 'e'});
    CHECK(peer_closed == 1);
    CHECK(closed == false);
    CHECK(flow->finished());

    f.stack.tick(f.now);
    CHECK_FALSE(f.stack.next_event_ns());
}

TEST_CASE("stack tcp: a lost segment is recovered by the retransmission timer") {
    Fixture f;
    f.bind();
    FakePeer peer{f};
    auto     flow = f.stack.open_tcp(80, nullptr, nullptr, nullptr, f.now);
    REQUIRE(flow);
    peer.pump();
    REQUIRE(flow->state() == TcpFlow::State::Open);

    const Bytes data(2000, 0x5a);
    peer.dropped = 1; // the first data segment is lost
    flow->write(data);
    peer.pump();
    // The second segment arrived out of order and was answered with a dup ACK.
    CHECK(peer.got.empty());

    f.now += kSec;
    f.stack.tick(f.now);
    peer.pump();
    CHECK(peer.got == data);
    CHECK(flow->buffered() == 0);
}
