#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "net/packets.h"

namespace bk::testing {

// A tiny firmware network stack at the far end of a `Stack`: it hands out a
// DHCP lease, accepts TCP connections on chosen ports and records UDP
// datagrams. Frames it sends are queued, so a test decides when they arrive
// and the stack is never called back from inside its own output.
class FakeGuest {
  public:
    using Bytes = packets::Bytes;
    using Mac   = packets::Mac;

    static constexpr Mac      kMac   = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
    static constexpr uint32_t kIp    = packets::ip4(10, 0, 0, 1);
    static constexpr uint32_t kLease = packets::ip4(10, 0, 0, 2);

    struct Connection {
        uint16_t guest_port = 0;
        uint16_t peer_port  = 0;
        uint32_t snd_nxt    = 0;
        uint32_t rcv_nxt    = 0;
        Bytes    received;
        bool     got_fin = false;
        bool     got_rst = false;
    };

    struct Datagram {
        uint16_t from_port; // the stack's local port
        uint16_t to_port;
        Bytes    data;
    };

    FakeGuest() = default;

    // Where the frames for the stack go (queued).
    std::deque<Bytes> outbox;

    bool                   answer_dhcp = true;
    std::set<uint16_t>     listening;
    std::deque<Connection> connections;
    std::vector<Datagram>  datagrams;
    uint32_t               syns = 0;

    void on_frame(const Bytes &frame) {
        using namespace packets;
        auto eth = parse_ethernet(frame);
        if (!eth || eth->type != kEtherIpv4) return;
        auto ip = parse_ipv4(eth->payload);
        if (!ip) return;
        if (ip->protocol == kProtoUdp) {
            auto udp = parse_udp(ip->src, ip->dst, ip->payload);
            if (!udp) return;
            if (udp->dst_port == kDhcpServerPort) return on_dhcp(*udp);
            datagrams.push_back({udp->src_port, udp->dst_port, Bytes(udp->payload.begin(), udp->payload.end())});
        } else if (ip->protocol == kProtoTcp) {
            auto tcp = parse_tcp(ip->src, ip->dst, ip->payload);
            if (tcp) on_tcp(ip->src, *tcp);
        }
    }

    Connection *connection(size_t i = 0) { return i < connections.size() ? &connections[i] : nullptr; }

    // Data from the firmware, in segments the stack's flow takes as they are.
    void send(Connection &c, const Bytes &data) {
        for (size_t off = 0; off < data.size(); off += 1000) {
            size_t n = std::min<size_t>(1000, data.size() - off);
            tcp(c, packets::kTcpAck | packets::kTcpPsh, c.snd_nxt, {data.data() + off, n});
            c.snd_nxt += uint32_t(n);
        }
    }
    void send_fin(Connection &c) {
        tcp(c, packets::kTcpAck | packets::kTcpFin, c.snd_nxt, {});
        c.snd_nxt += 1;
    }
    void send_rst(Connection &c) { tcp(c, packets::kTcpRst, c.snd_nxt, {}); }

    // A datagram from the firmware to a local port of the stack.
    void send_udp(uint16_t from_port, uint16_t to_port, const Bytes &data) {
        using namespace packets;
        ip_frame(kProtoUdp, build_udp(kIp, lease_, from_port, to_port, data));
    }

  private:
    void ip_frame(uint8_t protocol, const packets::Bytes &payload, bool broadcast = false, uint32_t dst = 0) {
        using namespace packets;
        Bytes ip = build_ipv4(kIp, dst ? dst : lease_, protocol, ++ident_, payload);
        outbox.push_back(build_ethernet(broadcast ? kBroadcastMac : stack_mac_, kMac, kEtherIpv4, ip));
    }

    void on_dhcp(const packets::Udp &udp) {
        using namespace packets;
        auto d = parse_dhcp(udp.payload);
        if (!d || !answer_dhcp || d->op != 1) return;
        stack_mac_   = d->chaddr;
        uint8_t type = d->type == DhcpDiscover ? DhcpOffer : d->type == DhcpRequest ? DhcpAck : 0;
        if (type == 0) return;
        Bytes p(236, 0);
        p[0] = 2;
        p[1] = 1;
        p[2] = 6;
        for (int i = 0; i < 4; ++i) {
            p[4 + i]  = uint8_t(d->xid >> (24 - 8 * i));
            p[16 + i] = uint8_t(kLease >> (24 - 8 * i));
        }
        std::copy(d->chaddr.begin(), d->chaddr.end(), p.begin() + 28);
        p.insert(p.end(), {0x63, 0x82, 0x53, 0x63, 53, 1, type, 54, 4});
        for (int i = 0; i < 4; ++i) p.push_back(uint8_t(kIp >> (24 - 8 * i)));
        p.push_back(255);
        ip_frame(kProtoUdp, build_udp(kIp, 0xffffffff, kDhcpServerPort, kDhcpClientPort, p), true, 0xffffffff);
    }

    void on_tcp(uint32_t src, const packets::Tcp &seg) {
        using namespace packets;
        lease_        = src;
        Connection *c = nullptr;
        for (Connection &x : connections)
            if (x.peer_port == seg.src_port && x.guest_port == seg.dst_port) c = &x;
        if (seg.flags & kTcpSyn) {
            ++syns;
            if (!listening.contains(seg.dst_port)) {
                Tcp rst{seg.dst_port, seg.src_port, 0, seg.seq + 1, uint8_t(kTcpRst | kTcpAck), 0, std::nullopt, {}};
                ip_frame(kProtoTcp, build_tcp(kIp, lease_, rst));
                return;
            }
            Connection &n = connections.emplace_back();
            n.guest_port  = seg.dst_port;
            n.peer_port   = seg.src_port;
            n.rcv_nxt     = seg.seq + 1;
            n.snd_nxt     = 5000 + 100 * uint32_t(connections.size());
            tcp(n, kTcpSyn | kTcpAck, n.snd_nxt, {}, 1400);
            n.snd_nxt += 1;
            return;
        }
        if (!c) return;
        if (seg.flags & kTcpRst) {
            c->got_rst = true;
            return;
        }
        bool wants_ack = false;
        if (seg.seq == c->rcv_nxt) {
            if (!seg.payload.empty()) {
                c->received.insert(c->received.end(), seg.payload.begin(), seg.payload.end());
                c->rcv_nxt += uint32_t(seg.payload.size());
                wants_ack = true;
            }
            if (seg.flags & kTcpFin) {
                c->got_fin = true;
                c->rcv_nxt += 1;
                wants_ack = true;
            }
        } else if (!seg.payload.empty()) {
            wants_ack = true;
        }
        if (wants_ack) tcp(*c, kTcpAck, c->snd_nxt, {});
    }

    void
    tcp(Connection &c, uint8_t flags, uint32_t seq, std::span<const uint8_t> data, std::optional<uint16_t> mss = {}) {
        using namespace packets;
        Tcp t{c.guest_port, c.peer_port, seq, c.rcv_nxt, flags, 65535, mss, data};
        ip_frame(kProtoTcp, build_tcp(kIp, lease_, t));
    }

    Mac      stack_mac_ = packets::kBroadcastMac;
    uint32_t lease_     = kLease;
    uint16_t ident_     = 0;
};

} // namespace bk::testing
