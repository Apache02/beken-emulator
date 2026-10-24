#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bk::packets {

using Mac   = std::array<uint8_t, 6>;
using Bytes = std::vector<uint8_t>;

// IPv4 addresses are plain host-order integers; 0 is "no address".
constexpr uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | d;
}

constexpr Mac kBroadcastMac = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

constexpr uint16_t kEtherIpv4 = 0x0800;
constexpr uint16_t kEtherArp  = 0x0806;

constexpr uint8_t kProtoIcmp = 1;
constexpr uint8_t kProtoTcp  = 6;
constexpr uint8_t kProtoUdp  = 17;

constexpr uint16_t kDhcpServerPort = 67;
constexpr uint16_t kDhcpClientPort = 68;

enum DhcpType : uint8_t { DhcpDiscover = 1, DhcpOffer = 2, DhcpRequest = 3, DhcpAck = 5, DhcpNak = 6 };

// The 16-bit one's complement sum of `data` (not inverted). `acc` carries a
// previous partial sum, which is how a pseudo header is prepended.
uint32_t checksum_sum(std::span<const uint8_t> data, uint32_t acc = 0);
// The Internet checksum: the inverted sum.
uint16_t checksum(std::span<const uint8_t> data, uint32_t acc = 0);

// The parse functions return spans into their input, valid while it is.
struct Ethernet {
    Mac                      dst;
    Mac                      src;
    uint16_t                 type;
    std::span<const uint8_t> payload;
};
std::optional<Ethernet> parse_ethernet(std::span<const uint8_t> frame);
Bytes                   build_ethernet(const Mac &dst, const Mac &src, uint16_t type, std::span<const uint8_t> payload);

struct Arp {
    uint16_t op; // 1 request, 2 reply
    Mac      sha;
    uint32_t spa;
    Mac      tha;
    uint32_t tpa;
};
std::optional<Arp> parse_arp(std::span<const uint8_t> payload);
Bytes              build_arp(const Arp &arp);

struct Ipv4 {
    uint32_t                 src;
    uint32_t                 dst;
    uint8_t                  protocol;
    uint8_t                  ttl;
    uint16_t                 ident;
    std::span<const uint8_t> payload;
};
// Rejects short packets, other versions, bad header checksums and fragments.
// Link layer padding behind the total length is cut off.
std::optional<Ipv4> parse_ipv4(std::span<const uint8_t> packet);
// Header length 20, TTL 64, DF set.
Bytes build_ipv4(uint32_t src, uint32_t dst, uint8_t protocol, uint16_t ident, std::span<const uint8_t> payload);

struct Udp {
    uint16_t                 src_port;
    uint16_t                 dst_port;
    std::span<const uint8_t> payload;
};
// A zero checksum is accepted (the sender did not compute one), a non-zero
// wrong one is not.
std::optional<Udp> parse_udp(uint32_t src, uint32_t dst, std::span<const uint8_t> datagram);
Bytes build_udp(uint32_t src, uint32_t dst, uint16_t src_port, uint16_t dst_port, std::span<const uint8_t> payload);

constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpPsh = 0x08;
constexpr uint8_t kTcpAck = 0x10;

struct Tcp {
    uint16_t                 src_port;
    uint16_t                 dst_port;
    uint32_t                 seq;
    uint32_t                 ack;
    uint8_t                  flags; // kTcp* bits
    uint16_t                 window;
    std::optional<uint16_t>  mss; // the option, in SYN segments
    std::span<const uint8_t> payload;
};
// Rejects short segments, a bad data offset and a wrong checksum. Options
// other than MSS are skipped; a malformed option list just ends the scan.
std::optional<Tcp> parse_tcp(uint32_t src, uint32_t dst, std::span<const uint8_t> segment);
// Header length 20, or 24 with the MSS option.
Bytes build_tcp(uint32_t src, uint32_t dst, const Tcp &tcp);

struct IcmpEcho {
    bool                     reply; // false: request
    uint16_t                 id;
    uint16_t                 seq;
    std::span<const uint8_t> data;
};
std::optional<IcmpEcho> parse_icmp_echo(std::span<const uint8_t> message);
Bytes                   build_icmp_echo(const IcmpEcho &echo);

struct Dhcp {
    uint8_t                 op;
    uint32_t                xid;
    uint32_t                yiaddr;
    Mac                     chaddr;
    uint8_t                 type; // option 53, 0 when absent
    std::optional<uint32_t> server_id;
    std::optional<uint32_t> router;
};
std::optional<Dhcp> parse_dhcp(std::span<const uint8_t> payload);
// A client message (op 1) with the parameter request list for subnet mask,
// router and DNS.
Bytes build_dhcp_client(
    uint8_t type, uint32_t xid, const Mac &mac, std::optional<uint32_t> requested, std::optional<uint32_t> server_id);

} // namespace bk::packets
