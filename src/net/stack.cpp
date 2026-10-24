#include "net/stack.h"

#include <algorithm>
#include <format>

#include "core/log.h"

namespace bk {

namespace {

using namespace packets;

Logger &log() { return Logger::get("net.stack"); }

std::string dotted(uint32_t ip) {
    return std::format("{}.{}.{}.{}", ip >> 24, ip >> 16 & 0xff, ip >> 8 & 0xff, ip & 0xff);
}

} // namespace

Stack::Stack(Mac mac, Output output) : mac_(mac), output_(std::move(output)) { reset(); }

void Stack::reset() {
    ip_        = 0;
    guest_ip_  = 0;
    guest_mac_ = {};
    dhcp_      = Dhcp::None;
    xid_       = 0;
    deadline_.reset();
    offered_ip_ = server_id_ = 0;
    server_mac_              = {};
    udp_.clear();
    // Handles outlive the table; make them inert.
    for (auto &[port, flow] : flows_) flow->discard();
    flows_.clear();
    next_port_ = FIRST_PORT;
    ident_     = 0;
    next_iss_  = 0;
}

void Stack::link_up(uint64_t now) {
    now_ = now;
    reset();
    send_discover(now);
}

void Stack::send_discover(uint64_t now) {
    ++xid_;
    dhcp_     = Dhcp::Discovering;
    deadline_ = now + DHCP_RETRY_NS;
    send_dhcp(DhcpDiscover, std::nullopt, std::nullopt);
}

void Stack::send_request(uint64_t now) {
    dhcp_     = Dhcp::Requesting;
    deadline_ = now + DHCP_RETRY_NS;
    send_dhcp(DhcpRequest, offered_ip_, server_id_);
}

void Stack::send_dhcp(uint8_t type, std::optional<uint32_t> requested, std::optional<uint32_t> server_id) {
    Bytes msg = build_dhcp_client(type, xid_, mac_, requested, server_id);
    Bytes udp = build_udp(0, 0xffffffff, kDhcpClientPort, kDhcpServerPort, msg);
    Bytes ip  = build_ipv4(0, 0xffffffff, kProtoUdp, ++ident_, udp);
    output_(build_ethernet(kBroadcastMac, mac_, kEtherIpv4, ip));
}

void Stack::tick(uint64_t now) {
    now_ = now;
    if (deadline_ && now >= *deadline_) {
        log().log(Level::Debug, "DHCP timed out, starting again");
        send_discover(now);
    }
    // A callback may open or close flows, so walk a copy.
    std::vector<TcpHandle> flows;
    for (auto &[port, flow] : flows_) flows.push_back(flow);
    for (auto &flow : flows) flow->tick();
    std::erase_if(flows_, [](const auto &entry) { return entry.second->finished(); });
}

std::optional<uint64_t> Stack::next_event_ns() const {
    std::optional<uint64_t> next = deadline_;
    for (auto &[port, flow] : flows_) {
        auto d = flow->deadline();
        if (d && (!next || *d < *next)) next = d;
    }
    return next;
}

void Stack::receive(std::span<const uint8_t> frame, uint64_t now) {
    now_     = now;
    auto eth = parse_ethernet(frame);
    if (!eth || (eth->dst != mac_ && eth->dst != kBroadcastMac)) return;
    if (eth->type == kEtherArp)
        on_arp(*eth);
    else if (eth->type == kEtherIpv4)
        on_ipv4(*eth, now);
}

void Stack::on_arp(const Ethernet &eth) {
    auto arp = parse_arp(eth.payload);
    if (!arp) return;
    if (guest_ip_ != 0 && arp->spa == guest_ip_) guest_mac_ = arp->sha;
    if (arp->op != 1 || ip_ == 0 || arp->tpa != ip_) return;
    Arp reply{2, mac_, ip_, arp->sha, arp->spa};
    output_(build_ethernet(arp->sha, mac_, kEtherArp, build_arp(reply)));
}

void Stack::on_ipv4(const Ethernet &eth, uint64_t now) {
    auto ip = parse_ipv4(eth.payload);
    if (!ip) return;

    if (ip->protocol == kProtoUdp) {
        auto udp = parse_udp(ip->src, ip->dst, ip->payload);
        if (!udp) return;
        // A reply is addressed to 0.0.0.0, the broadcast address or the
        // offered address, whichever the server picked: we have none yet.
        if (udp->dst_port == kDhcpClientPort) {
            on_dhcp(eth, *udp, now);
            return;
        }
        if (ip_ == 0 || ip->dst != ip_) return;
        auto it = udp_.find(udp->dst_port);
        if (it == udp_.end()) return;
        // The handler may unbind itself.
        UdpHandler handler = it->second;
        handler(udp->src_port, udp->payload);
        return;
    }

    if (ip_ == 0 || ip->dst != ip_) return;
    if (ip->protocol == kProtoIcmp) {
        auto echo = parse_icmp_echo(ip->payload);
        if (!echo || echo->reply) return;
        send_ip(kProtoIcmp, ip->src, build_icmp_echo({true, echo->id, echo->seq, echo->data}));
        return;
    }
    if (ip->protocol == kProtoTcp) on_tcp(*ip);
}

void Stack::on_tcp(const Ipv4 &ip) {
    auto tcp = parse_tcp(ip.src, ip.dst, ip.payload);
    if (!tcp) return;
    auto it = flows_.find(tcp->dst_port);
    if (it != flows_.end()) {
        // A callback may reset the stack and with it the table.
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
    send_ip(kProtoTcp, ip.src, build_tcp(ip_, ip.src, rst));
}

void Stack::Link::send_segment(const Tcp &segment) {
    stack_.send_ip(kProtoTcp, stack_.guest_ip_, build_tcp(stack_.ip_, stack_.guest_ip_, segment));
}

void Stack::on_dhcp(const Ethernet &eth, const Udp &udp, uint64_t now) {
    if (dhcp_ == Dhcp::None || dhcp_ == Dhcp::Bound) return;
    auto msg = parse_dhcp(udp.payload);
    if (!msg || msg->op != 2 || msg->xid != xid_ || msg->chaddr != mac_) return;

    switch (msg->type) {
        case DhcpOffer:
            if (dhcp_ != Dhcp::Discovering || !msg->server_id) return;
            offered_ip_ = msg->yiaddr;
            server_id_  = *msg->server_id;
            server_mac_ = eth.src;
            send_request(now);
            break;
        case DhcpAck:
            if (dhcp_ != Dhcp::Requesting) return;
            if (msg->router && *msg->router != server_id_)
                log().log(
                    Level::Debug,
                    std::format("router {} differs from the server {}", dotted(*msg->router), dotted(server_id_)));
            ip_        = offered_ip_;
            guest_ip_  = server_id_;
            guest_mac_ = server_mac_;
            dhcp_      = Dhcp::Bound;
            deadline_.reset();
            log().log(Level::Debug, std::format("DHCP bound: {} (firmware {})", dotted(ip_), dotted(guest_ip_)));
            break;
        case DhcpNak:
            log().log(Level::Debug, "DHCP NAK, starting again");
            send_discover(now);
            break;
        default: break;
    }
}

bool Stack::send_ip(uint8_t protocol, uint32_t dst, std::span<const uint8_t> payload) {
    if (dhcp_ != Dhcp::Bound) return false;
    Bytes ip = build_ipv4(ip_, dst, protocol, ++ident_, payload);
    output_(build_ethernet(guest_mac_, mac_, kEtherIpv4, ip));
    return true;
}

bool Stack::is_free(uint16_t port) const { return !udp_.contains(port) && !flows_.contains(port); }

uint16_t Stack::allocate_port() {
    for (unsigned tries = 0; tries <= 0xffff - FIRST_PORT; ++tries) {
        uint16_t port = next_port_;
        next_port_    = next_port_ == 0xffff ? FIRST_PORT : uint16_t(next_port_ + 1);
        if (is_free(port)) return port;
    }
    return 0;
}

void Stack::bind_udp(uint16_t port, UdpHandler handler) { udp_[port] = std::move(handler); }

void Stack::unbind_udp(uint16_t port) { udp_.erase(port); }

bool Stack::send_udp(uint16_t local_port, uint16_t guest_port, std::span<const uint8_t> data) {
    if (dhcp_ != Dhcp::Bound) return false;
    return send_ip(kProtoUdp, guest_ip_, build_udp(ip_, guest_ip_, local_port, guest_port, data));
}

Stack::TcpHandle Stack::open_tcp(uint16_t            guest_port,
                                 TcpFlow::Deliver    deliver,
                                 TcpFlow::PeerClosed on_peer_closed,
                                 TcpFlow::Closed     on_closed,
                                 uint64_t            now) {
    now_ = now;
    if (dhcp_ != Dhcp::Bound) return nullptr;
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
