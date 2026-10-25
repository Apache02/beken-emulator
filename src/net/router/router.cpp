#include "net/router/router.h"

#include <format>

#include "net/router/mac_text.h"
#include "net/router/nat.h"

namespace bk {

namespace {

using namespace packets;

constexpr uint32_t kBroadcastIp = 0xffffffff;

std::string dotted(uint32_t ip) {
    return std::format("{}.{}.{}.{}", ip >> 24, ip >> 16 & 0xff, ip >> 8 & 0xff, ip & 0xff);
}

} // namespace

Router::Router(RouterConfig config, Output output, Logger &log)
    : config_(config), output_(std::move(output)), log_(log) {}

void Router::reset() {
    guest_mac_.reset();
    udp_.clear();
    // Handles outlive the table; make them inert.
    for (auto &[port, flow] : flows_) flow->discard();
    flows_.clear();
    // The port and ISS counters keep going, so a connection after a new join
    // does not meet the old one's 4-tuple still in TIME_WAIT on the firmware.
}

void Router::tick(uint64_t now) {
    now_ = now;
    // A callback may open or close flows, so walk a copy.
    std::vector<TcpHandle> flows;
    for (auto &[port, flow] : flows_) flows.push_back(flow);
    for (auto &flow : flows) flow->tick();
    std::erase_if(flows_, [](const auto &entry) { return entry.second->finished(); });
}

std::optional<uint64_t> Router::next_event_ns() const {
    std::optional<uint64_t> next;
    for (auto &[port, flow] : flows_) {
        auto d = flow->deadline();
        if (d && (!next || *d < *next)) next = d;
    }
    return next;
}

void Router::receive(std::span<const uint8_t> frame, uint64_t now) {
    now_     = now;
    auto eth = parse_ethernet(frame);
    if (!eth || (eth->dst != config_.mac && eth->dst != kBroadcastMac)) return;
    if (eth->type == kEtherArp)
        on_arp(*eth);
    else if (eth->type == kEtherIpv4)
        on_ipv4(*eth);
}

void Router::on_arp(const Ethernet &eth) {
    auto arp = parse_arp(eth.payload);
    if (!arp) return;
    if (guest_ip_ != 0 && arp->spa == guest_ip_) guest_mac_ = arp->sha;
    if (arp->op != 1 || arp->tpa != config_.gateway) return;
    Arp reply{2, config_.mac, config_.gateway, arp->sha, arp->spa};
    output_(build_ethernet(arp->sha, config_.mac, kEtherArp, build_arp(reply)));
}

void Router::on_ipv4(const Ethernet &eth) {
    auto ip = parse_ipv4(eth.payload);
    if (!ip) return;

    if (ip->protocol == kProtoUdp) {
        auto udp = parse_udp(ip->src, ip->dst, ip->payload);
        if (!udp) return;
        // A client without an address sends from 0.0.0.0 to the broadcast
        // address; a renewing one from its address to ours.
        if (udp->dst_port == kDhcpServerPort && (ip->dst == kBroadcastIp || ip->dst == config_.gateway)) {
            on_dhcp(*udp);
            return;
        }
    }

    if (guest_ip_ == 0 || ip->src != guest_ip_) return;
    if (ip->dst != config_.gateway) {
        // Broadcast, multicast, 0.0.0.0/8 and the rest of the LAN (nobody else
        // is on it) stay here; only unicast beyond the LAN is the NAT's business.
        // 127.0.0.0/8 too: the host's loopback services are not the firmware's.
        const bool lan       = (ip->dst & config_.netmask) == (config_.gateway & config_.netmask);
        const bool multicast = (ip->dst >> 28) == 0xe;
        const bool loopback  = (ip->dst >> 24) == 127;
        if (lan || multicast || loopback || ip->dst == kBroadcastIp || (ip->dst >> 24) == 0) return;
        if (nat_ && ip->protocol == kProtoUdp) {
            auto udp = parse_udp(ip->src, ip->dst, ip->payload);
            if (!udp) return;
            nat_->udp_out(guest_ip_, udp->src_port, ip->dst, udp->dst_port, udp->payload, now_);
            return;
        }
        if (nat_ && ip->protocol == kProtoTcp) {
            auto tcp = parse_tcp(ip->src, ip->dst, ip->payload);
            if (!tcp) return;
            nat_->tcp_out(guest_ip_, ip->dst, *tcp, now_);
            return;
        }
        if (nat_ && ip->protocol == kProtoIcmp) {
            auto echo = parse_icmp_echo(ip->payload);
            if (echo) nat_->icmp_out(guest_ip_, ip->dst, *echo, now_);
            return;
        }
        log_.log(Level::Debug, std::format("no route to {}, dropped", dotted(ip->dst)));
        return;
    }

    switch (ip->protocol) {
        case kProtoIcmp: {
            auto echo = parse_icmp_echo(ip->payload);
            if (!echo || echo->reply) return;
            send_ip(config_.gateway, kProtoIcmp, build_icmp_echo({true, echo->id, echo->seq, echo->data}));
            break;
        }
        case kProtoUdp: {
            auto udp = parse_udp(ip->src, ip->dst, ip->payload);
            if (!udp) return;
            if (udp->dst_port == Nat::kDnsPort) {
                if (nat_) nat_->dns_query(guest_ip_, udp->src_port, udp->payload, now_);
                return;
            }
            // Only the forwarder's ports; the gateway is no door to the host.
            auto it = udp_.find(udp->dst_port);
            if (it == udp_.end()) return;
            // The handler may unbind itself.
            UdpHandler handler = it->second;
            handler(udp->src_port, udp->payload);
            break;
        }
        case kProtoTcp: on_tcp(*ip); break;
        default: break;
    }
}

void Router::on_tcp(const Ipv4 &ip) {
    auto tcp = parse_tcp(ip.src, ip.dst, ip.payload);
    if (!tcp) return;
    auto it = flows_.find(tcp->dst_port);
    if (it != flows_.end()) {
        // A callback may reset the router and with it the table.
        TcpHandle flow = it->second;
        flow->receive(*tcp);
        return;
    }
    if (tcp->flags & kTcpRst) return;
    Tcp rst{tcp->dst_port, tcp->src_port, 0, 0, kTcpRst, 0, std::nullopt, {}};
    if (tcp->flags & kTcpAck) {
        rst.seq = tcp->ack;
    } else {
        rst.flags |= kTcpAck;
        rst.ack = tcp->seq + uint32_t(tcp->payload.size()) + ((tcp->flags & kTcpSyn) ? 1 : 0);
    }
    send_ip(config_.gateway, kProtoTcp, build_tcp(config_.gateway, ip.src, rst));
}

void Router::on_dhcp(const Udp &udp) {
    auto msg = parse_dhcp(udp.payload);
    if (!msg || msg->op != 1) return;

    switch (msg->type) {
        case DhcpDiscover: send_dhcp(DhcpOffer, *msg, config_.guest); break;
        case DhcpRequest: {
            // The client took another server's offer.
            if (msg->server_id && *msg->server_id != config_.gateway) return;
            uint32_t wanted = msg->requested.value_or(msg->ciaddr);
            if (wanted != config_.guest) {
                log_.log(Level::Debug,
                         std::format("DHCP request for {} from {} refused", dotted(wanted), mac_text(msg->chaddr)));
                send_dhcp(DhcpNak, *msg, 0);
                return;
            }
            guest_ip_  = config_.guest;
            guest_mac_ = msg->chaddr;
            send_dhcp(DhcpAck, *msg, config_.guest);
            log_.log(Level::Info, std::format("lease {} to {}", dotted(guest_ip_), mac_text(msg->chaddr)));
            break;
        }
        default: break;
    }
}

void Router::send_dhcp(uint8_t type, const Dhcp &request, uint32_t yiaddr) {
    Bytes msg = build_dhcp_server(type,
                                  request.xid,
                                  request.chaddr,
                                  yiaddr,
                                  config_.gateway,
                                  config_.netmask,
                                  config_.gateway,
                                  config_.gateway,
                                  config_.lease_s);
    // Broadcast: the client may not have its address yet.
    Bytes udp = build_udp(config_.gateway, kBroadcastIp, kDhcpServerPort, kDhcpClientPort, msg);
    Bytes ip  = build_ipv4(config_.gateway, kBroadcastIp, kProtoUdp, ++ident_, udp);
    output_(build_ethernet(kBroadcastMac, config_.mac, kEtherIpv4, ip));
}

bool Router::send_ip(uint32_t src, uint8_t protocol, std::span<const uint8_t> payload) {
    if (!reachable()) return false;
    Bytes ip = build_ipv4(src, guest_ip_, protocol, ++ident_, payload);
    output_(build_ethernet(*guest_mac_, config_.mac, kEtherIpv4, ip));
    return true;
}

void Router::Link::send_segment(const Tcp &segment) {
    router_.send_ip(router_.config_.gateway, kProtoTcp, build_tcp(router_.config_.gateway, router_.guest_ip_, segment));
}

bool Router::is_free(uint16_t port) const { return !udp_.contains(port) && !flows_.contains(port); }

uint16_t Router::allocate_port() {
    for (unsigned tries = 0; tries <= 0xffff - FIRST_PORT; ++tries) {
        uint16_t port = next_port_;
        next_port_    = next_port_ == 0xffff ? FIRST_PORT : uint16_t(next_port_ + 1);
        if (is_free(port)) return port;
    }
    return 0;
}

void Router::bind_udp(uint16_t port, UdpHandler handler) { udp_[port] = std::move(handler); }

void Router::unbind_udp(uint16_t port) { udp_.erase(port); }

bool Router::send_udp(uint16_t local_port, uint16_t guest_port, std::span<const uint8_t> data) {
    if (!reachable()) return false;
    return send_ip(config_.gateway, kProtoUdp, build_udp(config_.gateway, guest_ip_, local_port, guest_port, data));
}

Router::TcpHandle Router::open_tcp(uint16_t            guest_port,
                                   TcpFlow::Deliver    deliver,
                                   TcpFlow::PeerClosed on_peer_closed,
                                   TcpFlow::Closed     on_closed,
                                   uint64_t            now) {
    now_ = now;
    if (!reachable()) return nullptr;
    uint16_t port = allocate_port();
    if (port == 0) return nullptr;
    auto flow = std::make_shared<TcpFlow>(
        link_, port, guest_port, next_iss_, std::move(deliver), std::move(on_peer_closed), std::move(on_closed));
    next_iss_ += TCP_ISS_STEP;
    flows_[port] = flow;
    flow->start();
    return flow;
}

} // namespace bk
