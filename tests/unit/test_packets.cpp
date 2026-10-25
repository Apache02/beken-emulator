#include <doctest/doctest.h>

#include <initializer_list>
#include <vector>

#include "net/packets.h"

using namespace bk::packets;

namespace {

constexpr Mac kA = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr Mac kB = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes r;
    for (const Bytes &p : parts) r.insert(r.end(), p.begin(), p.end());
    return r;
}

Bytes m(const Mac &x) { return Bytes(x.begin(), x.end()); }

} // namespace

TEST_CASE("checksum: the RFC 1071 example") {
    const Bytes data = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(checksum_sum(data) == 0xddf2);
    CHECK(checksum(data) == 0x220d);
}

TEST_CASE("checksum: odd length pads with a zero byte, empty is all ones") {
    CHECK(checksum(Bytes{0x12, 0x34, 0x56}) == uint16_t(~0x6834));
    CHECK(checksum(Bytes{}) == 0xffff);
}

TEST_CASE("checksum: a partial sum continues in the accumulator") {
    const Bytes a = {0x00, 0x01, 0xf2, 0x03}, b = {0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(checksum(b, checksum_sum(a)) == 0x220d);
}

TEST_CASE("ethernet: round trip and short frames") {
    const Bytes payload = {1, 2, 3};
    Bytes       f       = build_ethernet(kA, kB, kEtherIpv4, payload);
    CHECK(f == cat({
                   m(kA), m(kB), {0x08, 0x00},
                     payload
    }));
    auto e = parse_ethernet(f);
    REQUIRE(e);
    CHECK(e->dst == kA);
    CHECK(e->src == kB);
    CHECK(e->type == kEtherIpv4);
    CHECK(Bytes(e->payload.begin(), e->payload.end()) == payload);

    CHECK_FALSE(parse_ethernet(Bytes(13, 0)));
    CHECK(parse_ethernet(Bytes(14, 0)));
}

TEST_CASE("arp: round trip, exact bytes and rejections") {
    Arp   a{1, kA, ip4(10, 0, 0, 2), Mac{}, ip4(10, 0, 0, 1)};
    Bytes p = build_arp(a);
    CHECK(p == cat({
                   {0, 1, 8, 0, 6, 4, 0, 1},
                   m(kA), {10, 0, 0, 2},
                   Bytes(6, 0), {10, 0, 0, 1}
    }));
    auto r = parse_arp(p);
    REQUIRE(r);
    CHECK(r->op == 1);
    CHECK(r->sha == kA);
    CHECK(r->spa == ip4(10, 0, 0, 2));
    CHECK(r->tha == Mac{});
    CHECK(r->tpa == ip4(10, 0, 0, 1));

    CHECK_FALSE(parse_arp(Bytes(p.begin(), p.end() - 1)));
    for (size_t i : {1u, 3u, 4u, 5u}) {
        Bytes bad = p;
        bad[i] ^= 0x01;
        CHECK_FALSE(parse_arp(bad));
    }
}

TEST_CASE("ipv4: a known header and its checksum") {
    Bytes p = build_ipv4(ip4(192, 168, 0, 1), ip4(192, 168, 0, 199), kProtoUdp, 0, Bytes(95, 0));
    CHECK(Bytes(p.begin(), p.begin() + 20) == Bytes{0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                                                    0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7});
}

TEST_CASE("ipv4: round trip") {
    const Bytes payload = {9, 8, 7};
    Bytes       p       = build_ipv4(ip4(10, 0, 0, 1), ip4(10, 0, 0, 2), kProtoIcmp, 0x1234, payload);
    auto        r       = parse_ipv4(p);
    REQUIRE(r);
    CHECK(r->src == ip4(10, 0, 0, 1));
    CHECK(r->dst == ip4(10, 0, 0, 2));
    CHECK(r->protocol == kProtoIcmp);
    CHECK(r->ttl == 64);
    CHECK(r->ident == 0x1234);
    CHECK(Bytes(r->payload.begin(), r->payload.end()) == payload);
}

TEST_CASE("ipv4: padding behind the total length is cut off") {
    Bytes p = build_ipv4(1, 2, kProtoUdp, 0, Bytes{1, 2, 3});
    p.insert(p.end(), 20, 0);
    auto r = parse_ipv4(p);
    REQUIRE(r);
    CHECK(r->payload.size() == 3);
}

TEST_CASE("ipv4: short, other versions, bad checksum and fragments are rejected") {
    Bytes p = build_ipv4(1, 2, kProtoUdp, 0, Bytes{1, 2, 3, 4});
    CHECK_FALSE(parse_ipv4(Bytes(p.begin(), p.begin() + 19)));
    // Total length beyond the data.
    CHECK_FALSE(parse_ipv4(Bytes(p.begin(), p.end() - 1)));

    Bytes v6 = p;
    v6[0]    = 0x65;
    CHECK_FALSE(parse_ipv4(v6));

    Bytes sum = p;
    sum[8] ^= 1; // TTL changes, checksum stays
    CHECK_FALSE(parse_ipv4(sum));

    auto with_flags = [&](uint16_t flags) {
        Bytes f = p;
        f[6]    = uint8_t(flags >> 8);
        f[7]    = uint8_t(flags);
        f[10] = f[11] = 0;
        uint16_t c    = checksum(std::span<const uint8_t>(f).first(20));
        f[10]         = uint8_t(c >> 8);
        f[11]         = uint8_t(c);
        return f;
    };
    CHECK(parse_ipv4(with_flags(0x4000)));
    CHECK_FALSE(parse_ipv4(with_flags(0x2000))); // more fragments
    CHECK_FALSE(parse_ipv4(with_flags(0x0001))); // offset
}

TEST_CASE("udp: round trip and the pseudo header") {
    const Bytes payload = {'h', 'i', '!'};
    Bytes       d       = build_udp(ip4(10, 0, 0, 1), ip4(10, 0, 0, 2), 1234, 5678, payload);
    CHECK(d.size() == 11);
    CHECK(Bytes(d.begin(), d.begin() + 6) == Bytes{0x04, 0xd2, 0x16, 0x2e, 0x00, 0x0b});
    auto r = parse_udp(ip4(10, 0, 0, 1), ip4(10, 0, 0, 2), d);
    REQUIRE(r);
    CHECK(r->src_port == 1234);
    CHECK(r->dst_port == 5678);
    CHECK(Bytes(r->payload.begin(), r->payload.end()) == payload);
    // Other addresses change the pseudo header.
    CHECK_FALSE(parse_udp(ip4(10, 0, 0, 1), ip4(10, 0, 0, 3), d));
}

TEST_CASE("udp: a checksum that works out to zero is sent as 0xFFFF") {
    // Pseudo header 17 + 10, length 10 and the word 0xffda add up to 0xffff.
    Bytes d = build_udp(0, 0, 0, 0, Bytes{0xff, 0xda});
    CHECK(d[6] == 0xff);
    CHECK(d[7] == 0xff);
    CHECK(parse_udp(0, 0, d));
}

TEST_CASE("udp: bad checksum dropped, zero checksum accepted, short or lying length rejected") {
    Bytes d   = build_udp(1, 2, 10, 20, Bytes{1, 2, 3, 4});
    Bytes bad = d;
    bad[9] ^= 0x40;
    CHECK_FALSE(parse_udp(1, 2, bad));

    Bytes none = bad;
    none[6] = none[7] = 0;
    CHECK(parse_udp(1, 2, none));

    CHECK_FALSE(parse_udp(1, 2, Bytes(d.begin(), d.begin() + 7)));
    CHECK_FALSE(parse_udp(1, 2, Bytes(d.begin(), d.end() - 1))); // length field beyond the data
    Bytes tiny = d;
    tiny[5]    = 7; // length below the header
    CHECK_FALSE(parse_udp(1, 2, tiny));

    // Padding behind the datagram is ignored.
    Bytes padded = d;
    padded.insert(padded.end(), 6, 0);
    auto r = parse_udp(1, 2, padded);
    REQUIRE(r);
    CHECK(r->payload.size() == 4);
}

TEST_CASE("icmp: echo request and reply") {
    const Bytes data = {1, 2, 3, 4, 5};
    Bytes       m1   = build_icmp_echo({false, 0x1234, 7, data});
    CHECK(m1[0] == 8);
    CHECK(m1[1] == 0);
    CHECK(checksum(m1) == 0);
    auto r = parse_icmp_echo(m1);
    REQUIRE(r);
    CHECK_FALSE(r->reply);
    CHECK(r->id == 0x1234);
    CHECK(r->seq == 7);
    CHECK(Bytes(r->data.begin(), r->data.end()) == data);

    auto rep = parse_icmp_echo(build_icmp_echo({true, 1, 2, data}));
    REQUIRE(rep);
    CHECK(rep->reply);
}

TEST_CASE("icmp: short, bad checksum, other types and codes are rejected") {
    Bytes e = build_icmp_echo({false, 1, 2, Bytes{9}});
    CHECK_FALSE(parse_icmp_echo(Bytes(e.begin(), e.begin() + 7)));
    Bytes sum = e;
    sum[8] ^= 1;
    CHECK_FALSE(parse_icmp_echo(sum));
    Bytes type = e; // destination unreachable with a valid checksum
    type[0]    = 3;
    type[2] = type[3] = 0;
    uint16_t c        = checksum(type);
    type[2]           = uint8_t(c >> 8);
    type[3]           = uint8_t(c);
    CHECK_FALSE(parse_icmp_echo(type));
    Bytes code = e;
    code[1]    = 1;
    CHECK_FALSE(parse_icmp_echo(code));
}

TEST_CASE("dhcp: the exact bytes of a DISCOVER") {
    Bytes expected = cat({
        {1, 1, 6, 0},
        {0x00, 0x00, 0x00, 0x2a}, // xid
        {0, 0, 0x80, 0}, // secs, flags
        Bytes(16, 0), // ciaddr yiaddr siaddr giaddr
        m(kA),
        Bytes(10, 0), // chaddr padding
        Bytes(192, 0), // sname, file
        {0x63, 0x82, 0x53, 0x63},
        {53, 1, 1},
        {55, 3, 1, 3, 6},
        {255}
    });
    CHECK(expected.size() == 249);
    CHECK(build_dhcp_client(DhcpDiscover, 42, kA, std::nullopt, std::nullopt) == expected);
}

TEST_CASE("dhcp: the exact bytes of a REQUEST") {
    Bytes expected = cat({
        {1, 1, 6, 0},
        {0x12, 0x34, 0x56, 0x78},
        {0, 0, 0x80, 0},
        Bytes(16, 0),
        m(kA),
        Bytes(10 + 192, 0),
        {0x63, 0x82, 0x53, 0x63},
        {53, 1, 3},
        {50, 4, 10, 0, 0, 2},
        {54, 4, 10, 0, 0, 1},
        {55, 3, 1, 3, 6},
        {255}
    });
    CHECK(build_dhcp_client(DhcpRequest, 0x12345678, kA, ip4(10, 0, 0, 2), ip4(10, 0, 0, 1)) == expected);
}

TEST_CASE("dhcp: parse of a server reply") {
    Bytes p(236, 0);
    p[0]  = 2;
    p[4]  = 0xde;
    p[5]  = 0xad;
    p[6]  = 0xbe;
    p[7]  = 0xef;
    p[16] = 10;
    p[19] = 2; // yiaddr 10.0.0.2
    std::copy(kA.begin(), kA.end(), p.begin() + 28);
    p.insert(p.end(), {0x63, 0x82, 0x53, 0x63, 0, 53, 1, 5, 54, 4, 10, 0, 0, 1, 3, 4, 10, 0, 0, 9, 255, 99});
    auto d = parse_dhcp(p);
    REQUIRE(d);
    CHECK(d->op == 2);
    CHECK(d->xid == 0xdeadbeef);
    CHECK(d->yiaddr == ip4(10, 0, 0, 2));
    CHECK(d->chaddr == kA);
    CHECK(d->type == DhcpAck);
    CHECK(d->server_id == ip4(10, 0, 0, 1));
    CHECK(d->router == ip4(10, 0, 0, 9));
}

TEST_CASE("dhcp: a client message parses back, short or cookie-less messages are rejected") {
    Bytes p = build_dhcp_client(DhcpRequest, 7, kA, ip4(10, 0, 0, 2), ip4(10, 0, 0, 1));
    auto  d = parse_dhcp(p);
    REQUIRE(d);
    CHECK(d->op == 1);
    CHECK(d->xid == 7);
    CHECK(d->chaddr == kA);
    CHECK(d->type == DhcpRequest);
    CHECK(d->server_id == ip4(10, 0, 0, 1));
    CHECK_FALSE(d->router);

    CHECK_FALSE(parse_dhcp(Bytes(p.begin(), p.begin() + 239)));
    Bytes cookie = p;
    cookie[236]  = 0;
    CHECK_FALSE(parse_dhcp(cookie));
    // An option running past the end.
    Bytes cut(p.begin(), p.begin() + 242);
    cut[240] = 54;
    cut[241] = 4;
    CHECK_FALSE(parse_dhcp(cut));
}

TEST_CASE("dhcp: requested address and ciaddr are parsed") {
    Bytes p = build_dhcp_client(DhcpRequest, 7, kA, ip4(10, 0, 2, 15), std::nullopt);
    auto  d = parse_dhcp(p);
    REQUIRE(d);
    CHECK(d->requested == ip4(10, 0, 2, 15));
    CHECK(d->ciaddr == 0);

    Bytes renew = build_dhcp_client(DhcpRequest, 7, kA, std::nullopt, std::nullopt);
    renew[12]   = 10;
    renew[14]   = 2;
    renew[15]   = 15;
    d           = parse_dhcp(renew);
    REQUIRE(d);
    CHECK_FALSE(d->requested);
    CHECK(d->ciaddr == ip4(10, 0, 2, 15));
}

TEST_CASE("dhcp server message round-trips through parse_dhcp") {
    Bytes p = build_dhcp_server(DhcpOffer,
                                0x01020304,
                                kB,
                                ip4(10, 0, 2, 15),
                                ip4(10, 0, 2, 2),
                                ip4(255, 255, 255, 0),
                                ip4(10, 0, 2, 3),
                                ip4(10, 0, 2, 4),
                                86400);
    auto  d = parse_dhcp(p);
    REQUIRE(d);
    CHECK(d->op == 2);
    CHECK(d->xid == 0x01020304);
    CHECK(d->yiaddr == ip4(10, 0, 2, 15));
    CHECK(d->ciaddr == 0);
    CHECK(d->chaddr == kB);
    CHECK(d->type == DhcpOffer);
    CHECK(d->server_id == ip4(10, 0, 2, 2));
    CHECK(d->router == ip4(10, 0, 2, 3));
    CHECK_FALSE(d->requested);

    // The fixed part: BOOTREPLY, Ethernet, no flags, siaddr 0.
    REQUIRE(p.size() > 240);
    CHECK(p[1] == 1);
    CHECK(p[2] == 6);
    CHECK(p[10] == 0);
    CHECK(p[20] == 0);
    // The options, in order.
    Bytes options(p.begin() + 240, p.end());
    CHECK(options == Bytes{53,  1,   2,   54, 4, 10, 0,  2, 2, 51, 4, 0, 1,  0x51, 0x80, 1, 4,
                           255, 255, 255, 0,  3, 4,  10, 0, 2, 3,  6, 4, 10, 0,    2,    4, 255});
}

TEST_CASE("tcp: a SYN with the MSS option has the exact wire layout") {
    const Bytes data;
    Bytes syn = build_tcp(ip4(10, 0, 0, 2), ip4(10, 0, 0, 1), {40000, 80, 0x01020304, 0, kTcpSyn, 16384, 1400, data});
    CHECK(syn == Bytes{0x9c, 0x40, 0x00, 0x50, 0x01, 0x02, 0x03, 0x04, 0,    0,    0,    0,
                       0x60, 0x02, 0x40, 0x00, 0xa3, 0xc9, 0,    0,    0x02, 0x04, 0x05, 0x78});
    auto t = parse_tcp(ip4(10, 0, 0, 2), ip4(10, 0, 0, 1), syn);
    REQUIRE(t);
    CHECK(t->src_port == 40000);
    CHECK(t->dst_port == 80);
    CHECK(t->seq == 0x01020304);
    CHECK(t->ack == 0);
    CHECK(t->flags == kTcpSyn);
    CHECK(t->window == 16384);
    CHECK(t->mss == 1400);
    CHECK(t->payload.empty());
}

TEST_CASE("tcp: a data segment has a 20 byte header and no MSS") {
    const Bytes data = {'h', 'i', '!'};
    Bytes       seg  = build_tcp(
        ip4(10, 0, 0, 1), ip4(10, 0, 0, 2), {80, 40000, 100, 0x01020305, kTcpAck | kTcpPsh, 1000, std::nullopt, data});
    CHECK(seg.size() == 23);
    CHECK(seg[12] == 0x50);
    CHECK(seg[16] == 0x6d);
    CHECK(seg[17] == 0x7a);
    auto t = parse_tcp(ip4(10, 0, 0, 1), ip4(10, 0, 0, 2), seg);
    REQUIRE(t);
    CHECK(t->flags == (kTcpAck | kTcpPsh));
    CHECK(t->ack == 0x01020305);
    CHECK_FALSE(t->mss);
    CHECK(Bytes(t->payload.begin(), t->payload.end()) == data);
}

TEST_CASE("tcp: short, mis-offset and wrongly summed segments are rejected") {
    const Bytes data;
    const auto  a = ip4(10, 0, 0, 2), b = ip4(10, 0, 0, 1);
    Bytes       seg = build_tcp(a, b, {1, 2, 3, 4, kTcpAck, 5, std::nullopt, data});
    CHECK(parse_tcp(a, b, seg));
    CHECK_FALSE(parse_tcp(a, b, std::span(seg).first(19)));
    // The pseudo header counts.
    CHECK_FALSE(parse_tcp(a, ip4(10, 0, 0, 3), seg));
    Bytes bad = seg;
    bad[4] ^= 1;
    CHECK_FALSE(parse_tcp(a, b, bad));
    // A header length below 20 or beyond the segment, with a valid sum.
    for (uint8_t offset : {0x40, 0x60}) {
        Bytes s = seg;
        s[12]   = offset;
        s[16] = s[17]   = 0;
        uint32_t pseudo = checksum_sum(Bytes{10, 0, 0, 2, 10, 0, 0, 1, 0, 6, 0, uint8_t(s.size())});
        uint16_t sum    = checksum(s, pseudo);
        s[16]           = uint8_t(sum >> 8);
        s[17]           = uint8_t(sum);
        CHECK_FALSE(parse_tcp(a, b, s));
    }
}

TEST_CASE("tcp: options are skipped, a malformed list ends the scan") {
    const auto a = ip4(10, 0, 0, 2), b = ip4(10, 0, 0, 1);
    auto       with_options = [&](const Bytes &opts) {
        Bytes s = {0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t((5 + opts.size() / 4) << 4), kTcpSyn, 0, 9, 0, 0, 0, 0};
        s.insert(s.end(), opts.begin(), opts.end());
        uint32_t pseudo = checksum_sum(Bytes{10, 0, 0, 2, 10, 0, 0, 1, 0, 6, 0, uint8_t(s.size())});
        uint16_t sum    = checksum(s, pseudo);
        s[16]           = uint8_t(sum >> 8);
        s[17]           = uint8_t(sum);
        return parse_tcp(a, b, s);
    };
    // NOP, window scale (skipped), MSS, end.
    auto t = with_options({1, 3, 3, 7, 2, 4, 0x02, 0x18, 0, 0, 0, 0});
    REQUIRE(t);
    CHECK(t->mss == 0x218);
    // An option claiming to run past the header: no MSS, segment still valid.
    t = with_options({1, 1, 2, 8});
    REQUIRE(t);
    CHECK_FALSE(t->mss);
    // A length below 2 would loop forever.
    t = with_options({2, 0, 0, 0});
    REQUIRE(t);
    CHECK_FALSE(t->mss);
}
