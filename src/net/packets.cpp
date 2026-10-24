#include "net/packets.h"

#include <algorithm>

namespace bk::packets {

namespace {

constexpr size_t kEthernetHeader = 14;
constexpr size_t kArpLength      = 28;
constexpr size_t kIpv4Header     = 20;
constexpr size_t kUdpHeader      = 8;
constexpr size_t kTcpHeader      = 20;
constexpr size_t kIcmpHeader     = 8;

constexpr uint8_t kTcpOptEnd = 0, kTcpOptNop = 1, kTcpOptMss = 2;
constexpr size_t  kDhcpFixed   = 236;
constexpr size_t  kDhcpMinimum = kDhcpFixed + 4;

constexpr uint8_t kCookie[] = {0x63, 0x82, 0x53, 0x63};

constexpr uint8_t kIcmpEchoReply = 0, kIcmpEchoRequest = 8;

constexpr uint8_t kOptPad = 0, kOptRouter = 3, kOptRequested = 50, kOptType = 53, kOptServer = 54, kOptParameters = 55,
                  kOptEnd = 255;

uint16_t be16(std::span<const uint8_t> b, size_t off) { return uint16_t(b[off] << 8 | b[off + 1]); }
uint32_t be32(std::span<const uint8_t> b, size_t off) {
    return uint32_t(b[off]) << 24 | uint32_t(b[off + 1]) << 16 | uint32_t(b[off + 2]) << 8 | b[off + 3];
}

void put16(Bytes &v, uint16_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
void put32(Bytes &v, uint32_t x) {
    put16(v, uint16_t(x >> 16));
    put16(v, uint16_t(x));
}
void put_mac(Bytes &v, const Mac &m) { v.insert(v.end(), m.begin(), m.end()); }

void patch16(Bytes &v, size_t off, uint16_t x) {
    v[off]     = uint8_t(x >> 8);
    v[off + 1] = uint8_t(x);
}

Mac mac_at(std::span<const uint8_t> b, size_t off) {
    Mac m;
    std::copy_n(b.begin() + off, 6, m.begin());
    return m;
}

// The pseudo header of UDP and TCP, as a partial sum.
uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t protocol, size_t length) {
    Bytes p;
    put32(p, src);
    put32(p, dst);
    p.push_back(0);
    p.push_back(protocol);
    put16(p, uint16_t(length));
    return checksum_sum(p);
}

} // namespace

uint32_t checksum_sum(std::span<const uint8_t> data, uint32_t acc) {
    uint64_t sum = acc;
    size_t   i   = 0;
    for (; i + 1 < data.size(); i += 2) sum += uint32_t(data[i] << 8 | data[i + 1]);
    if (i < data.size()) sum += uint32_t(data[i] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return uint32_t(sum);
}

uint16_t checksum(std::span<const uint8_t> data, uint32_t acc) { return uint16_t(~checksum_sum(data, acc)); }

std::optional<Ethernet> parse_ethernet(std::span<const uint8_t> frame) {
    if (frame.size() < kEthernetHeader) return std::nullopt;
    return Ethernet{mac_at(frame, 0), mac_at(frame, 6), be16(frame, 12), frame.subspan(kEthernetHeader)};
}

Bytes build_ethernet(const Mac &dst, const Mac &src, uint16_t type, std::span<const uint8_t> payload) {
    Bytes f;
    f.reserve(kEthernetHeader + payload.size());
    put_mac(f, dst);
    put_mac(f, src);
    put16(f, type);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::optional<Arp> parse_arp(std::span<const uint8_t> p) {
    if (p.size() < kArpLength) return std::nullopt;
    if (be16(p, 0) != 1 || be16(p, 2) != kEtherIpv4 || p[4] != 6 || p[5] != 4) return std::nullopt;
    return Arp{be16(p, 6), mac_at(p, 8), be32(p, 14), mac_at(p, 18), be32(p, 24)};
}

Bytes build_arp(const Arp &a) {
    Bytes p;
    put16(p, 1);
    put16(p, kEtherIpv4);
    p.push_back(6);
    p.push_back(4);
    put16(p, a.op);
    put_mac(p, a.sha);
    put32(p, a.spa);
    put_mac(p, a.tha);
    put32(p, a.tpa);
    return p;
}

std::optional<Ipv4> parse_ipv4(std::span<const uint8_t> p) {
    if (p.size() < kIpv4Header || p[0] >> 4 != 4) return std::nullopt;
    size_t header = size_t(p[0] & 0x0f) * 4;
    size_t total  = be16(p, 2);
    if (header < kIpv4Header || header > p.size() || total < header || total > p.size()) return std::nullopt;
    if (checksum(p.first(header)) != 0) return std::nullopt;
    // More-fragments flag or a fragment offset.
    if (be16(p, 6) & 0x3fff) return std::nullopt;
    return Ipv4{be32(p, 12), be32(p, 16), p[9], p[8], be16(p, 4), p.subspan(header, total - header)};
}

Bytes build_ipv4(uint32_t src, uint32_t dst, uint8_t protocol, uint16_t ident, std::span<const uint8_t> payload) {
    Bytes p;
    p.reserve(kIpv4Header + payload.size());
    p.push_back(0x45);
    p.push_back(0);
    put16(p, uint16_t(kIpv4Header + payload.size()));
    put16(p, ident);
    put16(p, 0x4000);
    p.push_back(64);
    p.push_back(protocol);
    put16(p, 0);
    put32(p, src);
    put32(p, dst);
    patch16(p, 10, checksum(p));
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

std::optional<Udp> parse_udp(uint32_t src, uint32_t dst, std::span<const uint8_t> d) {
    if (d.size() < kUdpHeader) return std::nullopt;
    size_t length = be16(d, 4);
    if (length < kUdpHeader || length > d.size()) return std::nullopt;
    d = d.first(length);
    if (be16(d, 6) != 0 && checksum(d, pseudo_sum(src, dst, kProtoUdp, length)) != 0) return std::nullopt;
    return Udp{be16(d, 0), be16(d, 2), d.subspan(kUdpHeader)};
}

Bytes build_udp(uint32_t src, uint32_t dst, uint16_t src_port, uint16_t dst_port, std::span<const uint8_t> payload) {
    Bytes d;
    d.reserve(kUdpHeader + payload.size());
    put16(d, src_port);
    put16(d, dst_port);
    put16(d, uint16_t(kUdpHeader + payload.size()));
    put16(d, 0);
    d.insert(d.end(), payload.begin(), payload.end());
    uint16_t sum = checksum(d, pseudo_sum(src, dst, kProtoUdp, d.size()));
    patch16(d, 6, sum ? sum : 0xffff);
    return d;
}

std::optional<Tcp> parse_tcp(uint32_t src, uint32_t dst, std::span<const uint8_t> s) {
    if (s.size() < kTcpHeader) return std::nullopt;
    size_t header = size_t(s[12] >> 4) * 4;
    if (header < kTcpHeader || header > s.size()) return std::nullopt;
    if (checksum(s, pseudo_sum(src, dst, kProtoTcp, s.size())) != 0) return std::nullopt;
    Tcp t{be16(s, 0),
          be16(s, 2),
          be32(s, 4),
          be32(s, 8),
          uint8_t(s[13] & 0x3f),
          be16(s, 14),
          std::nullopt,
          s.subspan(header)};
    for (size_t i = kTcpHeader; i < header;) {
        uint8_t kind = s[i++];
        if (kind == kTcpOptEnd) break;
        if (kind == kTcpOptNop) continue;
        if (i >= header) break;
        size_t len = s[i++];
        if (len < 2 || i - 2 + len > header) break;
        if (kind == kTcpOptMss && len == 4) t.mss = be16(s, i);
        i += len - 2;
    }
    return t;
}

Bytes build_tcp(uint32_t src, uint32_t dst, const Tcp &t) {
    Bytes  s;
    size_t header = kTcpHeader + (t.mss ? 4 : 0);
    s.reserve(header + t.payload.size());
    put16(s, t.src_port);
    put16(s, t.dst_port);
    put32(s, t.seq);
    put32(s, t.ack);
    s.push_back(uint8_t(header / 4 << 4));
    s.push_back(t.flags);
    put16(s, t.window);
    put16(s, 0); // checksum
    put16(s, 0); // urgent pointer
    if (t.mss) {
        s.insert(s.end(), {kTcpOptMss, 4});
        put16(s, *t.mss);
    }
    s.insert(s.end(), t.payload.begin(), t.payload.end());
    patch16(s, 16, checksum(s, pseudo_sum(src, dst, kProtoTcp, s.size())));
    return s;
}

std::optional<IcmpEcho> parse_icmp_echo(std::span<const uint8_t> m) {
    if (m.size() < kIcmpHeader || m[1] != 0 || checksum(m) != 0) return std::nullopt;
    if (m[0] != kIcmpEchoRequest && m[0] != kIcmpEchoReply) return std::nullopt;
    return IcmpEcho{m[0] == kIcmpEchoReply, be16(m, 4), be16(m, 6), m.subspan(kIcmpHeader)};
}

Bytes build_icmp_echo(const IcmpEcho &e) {
    Bytes m;
    m.reserve(kIcmpHeader + e.data.size());
    m.push_back(e.reply ? kIcmpEchoReply : kIcmpEchoRequest);
    m.push_back(0);
    put16(m, 0);
    put16(m, e.id);
    put16(m, e.seq);
    m.insert(m.end(), e.data.begin(), e.data.end());
    patch16(m, 2, checksum(m));
    return m;
}

std::optional<Dhcp> parse_dhcp(std::span<const uint8_t> p) {
    if (p.size() < kDhcpMinimum || !std::equal(std::begin(kCookie), std::end(kCookie), p.begin() + kDhcpFixed))
        return std::nullopt;
    Dhcp d{p[0], be32(p, 4), be32(p, 16), mac_at(p, 28), 0, std::nullopt, std::nullopt};
    for (size_t i = kDhcpMinimum; i < p.size();) {
        uint8_t code = p[i++];
        if (code == kOptPad) continue;
        if (code == kOptEnd) break;
        if (i >= p.size()) return std::nullopt;
        size_t len = p[i++];
        if (i + len > p.size()) return std::nullopt;
        auto value = p.subspan(i, len);
        if (code == kOptType && len == 1) d.type = value[0];
        if (code == kOptServer && len >= 4) d.server_id = be32(value, 0);
        if (code == kOptRouter && len >= 4) d.router = be32(value, 0);
        i += len;
    }
    return d;
}

Bytes build_dhcp_client(
    uint8_t type, uint32_t xid, const Mac &mac, std::optional<uint32_t> requested, std::optional<uint32_t> server_id) {
    Bytes p;
    p.push_back(1); // BOOTREQUEST
    p.push_back(1); // Ethernet
    p.push_back(6);
    p.push_back(0); // hops
    put32(p, xid);
    put16(p, 0);              // secs
    put16(p, 0x8000);         // ask for broadcast replies: we have no address yet
    p.insert(p.end(), 16, 0); // ciaddr, yiaddr, siaddr, giaddr
    put_mac(p, mac);
    p.insert(p.end(), 10 + 192, 0); // chaddr padding, sname, file
    p.insert(p.end(), std::begin(kCookie), std::end(kCookie));
    p.insert(p.end(), {kOptType, 1, type});
    if (requested) {
        p.insert(p.end(), {kOptRequested, 4});
        put32(p, *requested);
    }
    if (server_id) {
        p.insert(p.end(), {kOptServer, 4});
        put32(p, *server_id);
    }
    p.insert(p.end(), {kOptParameters, 3, 1, 3, 6, kOptEnd});
    return p;
}

} // namespace bk::packets
