#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "core/log.h"
#include "net/packets.h"
#include "net/router/nat.h"
#include "net/router/router.h"

using namespace bk;
using namespace bk::packets;

namespace {

constexpr Mac kRouter = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
constexpr Mac kFw     = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};

constexpr uint32_t kGateway = ip4(10, 0, 2, 2);
constexpr uint32_t kGuest   = ip4(10, 0, 2, 15);
constexpr uint32_t kMask    = ip4(255, 255, 255, 0);
constexpr uint32_t kAll     = 0xffffffff;

Bytes sub(std::span<const uint8_t> s) { return Bytes(s.begin(), s.end()); }

Bytes udp_frame(
    const Mac &dst, const Mac &src, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, const Bytes &data) {
    return build_ethernet(dst, src, kEtherIpv4, build_ipv4(sip, dip, kProtoUdp, 1, build_udp(sip, dip, sp, dp, data)));
}

Bytes tcp_frame(uint32_t sip, uint32_t dip, const Tcp &tcp) {
    return build_ethernet(kRouter, kFw, kEtherIpv4, build_ipv4(sip, dip, kProtoTcp, 1, build_tcp(sip, dip, tcp)));
}

// A frame the router sent, taken apart down to IPv4.
struct Sent {
    Ethernet eth;
    Ipv4     ip;
};
Sent parse_sent(const Bytes &frame) {
    auto eth = parse_ethernet(frame);
    REQUIRE(eth);
    REQUIRE(eth->type == kEtherIpv4);
    auto ip = parse_ipv4(eth->payload);
    REQUIRE(ip);
    return {*eth, *ip};
}

struct Fixture {
    std::ostringstream log;
    std::vector<Bytes> out;
    Router             router;
    uint64_t           now = 0;

    Fixture()
        : router(
              RouterConfig{},
              [this](Bytes f) { out.push_back(std::move(f)); },
              (Logger::reset_for_tests(), Logger::configure(log, false), Logger::get("net.router"))) {}

    void fw(const Bytes &f) { router.receive(f, now); }

    // A client message from the firmware, broadcast as before it has an address.
    void client(const Bytes &msg) { fw(udp_frame(kBroadcastMac, kFw, 0, kAll, 68, 67, msg)); }

    // The last frame sent, as a DHCP server message.
    std::pair<Sent, Dhcp> last_dhcp() {
        REQUIRE_FALSE(out.empty());
        Sent s   = parse_sent(out.back());
        auto udp = parse_udp(s.ip.src, s.ip.dst, s.ip.payload);
        REQUIRE(udp);
        CHECK(udp->src_port == 67);
        CHECK(udp->dst_port == 68);
        auto d = parse_dhcp(udp->payload);
        REQUIRE(d);
        return {s, *d};
    }

    void lease() {
        client(build_dhcp_client(DhcpDiscover, 9, kFw, std::nullopt, std::nullopt));
        client(build_dhcp_client(DhcpRequest, 9, kFw, kGuest, kGateway));
        REQUIRE(router.guest_ip() == kGuest);
        out.clear();
    }
};

// A UDP socket on 127.0.0.1 standing in for a host beyond the LAN.
struct Peer {
    int         fd   = -1;
    uint16_t    port = 0;
    sockaddr_in from{};

    Peer() {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        REQUIRE(fd >= 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
        socklen_t len = sizeof sa;
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
        port = ntohs(sa.sin_port);
    }
    ~Peer() { ::close(fd); }

    // The next datagram, waiting up to a second.
    std::optional<Bytes> receive() {
        for (int i = 0; i < 1000; ++i) {
            uint8_t   buf[2048];
            socklen_t len = sizeof from;
            ssize_t   n   = ::recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &len);
            if (n >= 0) return Bytes(buf, buf + n);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    }

    void answer(const Bytes &data) {
        REQUIRE(::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr *>(&from), sizeof from) ==
                ssize_t(data.size()));
    }
};

} // namespace

TEST_CASE("router: DHCP offers and acknowledges 10.0.2.15") {
    Fixture f;
    CHECK(f.router.guest_ip() == 0);
    f.client(build_dhcp_client(DhcpDiscover, 0x1234, kFw, std::nullopt, std::nullopt));
    REQUIRE(f.out.size() == 1);
    auto [s, d] = f.last_dhcp();
    CHECK(s.eth.dst == kBroadcastMac);
    CHECK(s.eth.src == kRouter);
    CHECK(s.ip.src == kGateway);
    CHECK(s.ip.dst == kAll);
    auto udp = parse_udp(s.ip.src, s.ip.dst, s.ip.payload);
    CHECK(sub(udp->payload) ==
          build_dhcp_server(DhcpOffer, 0x1234, kFw, kGuest, kGateway, kMask, kGateway, kGateway, 86400));
    CHECK(d.type == DhcpOffer);
    // An offer is no lease yet.
    CHECK(f.router.guest_ip() == 0);

    f.client(build_dhcp_client(DhcpRequest, 0x1234, kFw, kGuest, kGateway));
    REQUIRE(f.out.size() == 2);
    auto [s2, ack] = f.last_dhcp();
    CHECK(s2.eth.dst == kBroadcastMac);
    CHECK(s2.ip.dst == kAll);
    udp = parse_udp(s2.ip.src, s2.ip.dst, s2.ip.payload);
    CHECK(sub(udp->payload) ==
          build_dhcp_server(DhcpAck, 0x1234, kFw, kGuest, kGateway, kMask, kGateway, kGateway, 86400));
    CHECK(f.router.guest_ip() == kGuest);
    CHECK(f.log.str().find("lease 10.0.2.15 to 02:aa:bb:cc:dd:01") != std::string::npos);

    // A renewal names the address in ciaddr instead of option 50.
    Bytes renew = build_dhcp_client(DhcpRequest, 0x99, kFw, std::nullopt, std::nullopt);
    renew[12]   = 10;
    renew[14]   = 2;
    renew[15]   = 15;
    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 68, 67, renew));
    REQUIRE(f.out.size() == 3);
    CHECK(f.last_dhcp().second.type == DhcpAck);
    CHECK(f.last_dhcp().second.xid == 0x99);
}

TEST_CASE("router: a request for another address gets a NAK") {
    Fixture f;
    f.client(build_dhcp_client(DhcpRequest, 5, kFw, ip4(192, 168, 1, 50), std::nullopt));
    REQUIRE(f.out.size() == 1);
    auto [s, d] = f.last_dhcp();
    CHECK(d.type == DhcpNak);
    CHECK(d.xid == 5);
    CHECK(d.chaddr == kFw);
    CHECK(s.eth.dst == kBroadcastMac);
    CHECK(f.router.guest_ip() == 0);

    // A request for another server's offer is not ours to answer.
    f.client(build_dhcp_client(DhcpRequest, 6, kFw, kGuest, ip4(10, 0, 2, 3)));
    CHECK(f.out.size() == 1);
    CHECK(f.router.guest_ip() == 0);
}

TEST_CASE("router: answers ARP and ping for the gateway") {
    Fixture f;
    Arp     req{1, kFw, kGuest, {}, kGateway};
    f.fw(build_ethernet(kBroadcastMac, kFw, kEtherArp, build_arp(req)));
    REQUIRE(f.out.size() == 1);
    auto eth = parse_ethernet(f.out[0]);
    REQUIRE(eth);
    CHECK(eth->dst == kFw);
    CHECK(eth->src == kRouter);
    CHECK(eth->type == kEtherArp);
    auto arp = parse_arp(eth->payload);
    REQUIRE(arp);
    CHECK(arp->op == 2);
    CHECK(arp->sha == kRouter);
    CHECK(arp->spa == kGateway);
    CHECK(arp->tha == kFw);
    CHECK(arp->tpa == kGuest);

    // Other targets and replies: nothing.
    f.out.clear();
    Arp other{1, kFw, kGuest, {}, ip4(10, 0, 2, 99)};
    f.fw(build_ethernet(kBroadcastMac, kFw, kEtherArp, build_arp(other)));
    Arp reply{2, kFw, kGuest, kRouter, kGateway};
    f.fw(build_ethernet(kRouter, kFw, kEtherArp, build_arp(reply)));
    CHECK(f.out.empty());

    f.lease();
    const Bytes data = {1, 2, 3, 4};
    f.fw(build_ethernet(
        kRouter, kFw, kEtherIpv4, build_ipv4(kGuest, kGateway, kProtoIcmp, 1, build_icmp_echo({false, 7, 3, data}))));
    REQUIRE(f.out.size() == 1);
    Sent s = parse_sent(f.out[0]);
    CHECK(s.eth.dst == kFw);
    CHECK(s.eth.src == kRouter);
    CHECK(s.ip.src == kGateway);
    CHECK(s.ip.dst == kGuest);
    CHECK(s.ip.protocol == kProtoIcmp);
    auto echo = parse_icmp_echo(s.ip.payload);
    REQUIRE(echo);
    CHECK(echo->reply);
    CHECK(echo->id == 7);
    CHECK(echo->seq == 3);
    CHECK(sub(echo->data) == data);

    // A ping to an address beyond the LAN goes nowhere without a NAT.
    f.out.clear();
    f.fw(build_ethernet(kRouter,
                        kFw,
                        kEtherIpv4,
                        build_ipv4(kGuest, ip4(8, 8, 8, 8), kProtoIcmp, 1, build_icmp_echo({false, 7, 4, data}))));
    CHECK(f.out.empty());
}

TEST_CASE("router: TCP to a closed gateway port is reset, UDP is dropped") {
    Fixture f;
    f.lease();
    const Bytes data = {'h', 'i'};
    f.fw(tcp_frame(kGuest, kGateway, Tcp{5555, 22, 1000, 0, kTcpSyn, 4096, uint16_t(1460), {}}));
    REQUIRE(f.out.size() == 1);
    Sent s = parse_sent(f.out[0]);
    CHECK(s.eth.dst == kFw);
    CHECK(s.ip.src == kGateway);
    CHECK(s.ip.dst == kGuest);
    auto rst = parse_tcp(s.ip.src, s.ip.dst, s.ip.payload);
    REQUIRE(rst);
    CHECK(rst->src_port == 22);
    CHECK(rst->dst_port == 5555);
    CHECK(rst->flags == (kTcpRst | kTcpAck));
    CHECK(rst->ack == 1001);

    f.out.clear();
    f.fw(tcp_frame(kGuest, kGateway, Tcp{5555, 80, 2000, 0, kTcpPsh, 4096, std::nullopt, data}));
    REQUIRE(f.out.size() == 1);
    rst = parse_tcp(kGateway, kGuest, parse_sent(f.out[0]).ip.payload);
    REQUIRE(rst);
    CHECK(rst->flags == (kTcpRst | kTcpAck));
    CHECK(rst->ack == 2002);

    // A reset is not answered.
    f.out.clear();
    f.fw(tcp_frame(kGuest, kGateway, Tcp{5555, 80, 2000, 0, kTcpRst, 4096, std::nullopt, {}}));
    CHECK(f.out.empty());

    // UDP to the gateway, on DNS or any other port: dropped (DNS has no relay without a NAT).
    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 5000, 7, {1}));
    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 5000, 53, {1}));
    CHECK(f.out.empty());
}

TEST_CASE("router: forwarder flows reach the firmware after the lease") {
    Fixture f;
    auto    nothing = [](std::span<const uint8_t>) {};
    CHECK_FALSE(f.router.open_tcp(80, nothing, [] {}, [](bool) {}, 0));
    CHECK_FALSE(f.router.send_udp(40000, 7, Bytes{1}));
    CHECK(f.out.empty());

    f.lease();
    Bytes got;
    auto  flow = f.router.open_tcp(
        80, [&](std::span<const uint8_t> d) { got.insert(got.end(), d.begin(), d.end()); }, [] {}, [](bool) {}, 0);
    REQUIRE(flow);
    REQUIRE(f.out.size() == 1);
    Sent s = parse_sent(f.out[0]);
    CHECK(s.eth.dst == kFw);
    CHECK(s.eth.src == kRouter);
    CHECK(s.ip.src == kGateway);
    CHECK(s.ip.dst == kGuest);
    auto syn = parse_tcp(s.ip.src, s.ip.dst, s.ip.payload);
    REQUIRE(syn);
    CHECK(syn->src_port == 40000);
    CHECK(syn->dst_port == 80);
    CHECK(syn->flags == kTcpSyn);
    CHECK(f.router.next_event_ns() == TcpFlow::RTO_NS);

    // The firmware answers; the flow opens and takes data.
    f.out.clear();
    f.fw(tcp_frame(kGuest, kGateway, Tcp{80, 40000, 500, syn->seq + 1, kTcpSyn | kTcpAck, 4096, uint16_t(1460), {}}));
    CHECK(flow->state() == TcpFlow::State::Open);
    f.fw(tcp_frame(kGuest,
                   kGateway,
                   Tcp{
                       80, 40000, 501, syn->seq + 1, kTcpAck | kTcpPsh, 4096, std::nullopt, Bytes{'o', 'k'}
    }));
    CHECK(got == Bytes{'o', 'k'});

    // UDP both ways through a bound port.
    std::vector<std::pair<uint16_t, Bytes>> udp;
    uint16_t                                port = f.router.allocate_port();
    CHECK(port == 40001);
    f.router.bind_udp(port, [&](uint16_t sp, std::span<const uint8_t> d) { udp.emplace_back(sp, sub(d)); });
    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 7, port, {9}));
    REQUIRE(udp.size() == 1);
    CHECK(udp[0].first == 7);
    CHECK(udp[0].second == Bytes{9});
    f.out.clear();
    CHECK(f.router.send_udp(port, 7, Bytes{8}));
    REQUIRE(f.out.size() == 1);
    s      = parse_sent(f.out[0]);
    auto u = parse_udp(s.ip.src, s.ip.dst, s.ip.payload);
    REQUIRE(u);
    CHECK(s.ip.src == kGateway);
    CHECK(u->src_port == port);
    CHECK(u->dst_port == 7);
    f.router.unbind_udp(port);
    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 7, port, {9}));
    CHECK(udp.size() == 1);

    // send_ip carries any source address.
    f.out.clear();
    CHECK(f.router.send_ip(ip4(93, 184, 216, 34), kProtoUdp, build_udp(ip4(93, 184, 216, 34), kGuest, 80, 9, Bytes{})));
    REQUIRE(f.out.size() == 1);
    CHECK(parse_sent(f.out[0]).ip.src == ip4(93, 184, 216, 34));
}

TEST_CASE("router: reset keeps the lease address") {
    Fixture f;
    f.lease();
    auto flow = f.router.open_tcp(80, [](std::span<const uint8_t>) {}, [] {}, [](bool) {}, 0);
    REQUIRE(flow);
    auto first = parse_tcp(kGateway, kGuest, parse_sent(f.out.at(0)).ip.payload);
    REQUIRE(first);
    CHECK(first->src_port == 40000);
    f.out.clear();

    f.router.reset();
    CHECK(f.out.empty());
    CHECK(flow->finished());
    CHECK_FALSE(f.router.next_event_ns());
    CHECK(f.router.guest_ip() == kGuest);
    // Without the firmware's MAC nothing goes out.
    CHECK_FALSE(f.router.send_ip(kGateway, kProtoUdp, Bytes{}));
    CHECK_FALSE(f.router.open_tcp(80, [](std::span<const uint8_t>) {}, [] {}, [](bool) {}, 0));

    // The firmware joins again and gets the same address.
    f.client(build_dhcp_client(DhcpDiscover, 21, kFw, std::nullopt, std::nullopt));
    CHECK(f.last_dhcp().second.yiaddr == kGuest);
    f.client(build_dhcp_client(DhcpRequest, 21, kFw, kGuest, kGateway));
    CHECK(f.last_dhcp().second.type == DhcpAck);
    CHECK(f.router.guest_ip() == kGuest);
    f.out.clear();
    auto again = f.router.open_tcp(80, [](std::span<const uint8_t>) {}, [] {}, [](bool) {}, 0);
    REQUIRE(again);
    auto syn = parse_tcp(kGateway, kGuest, parse_sent(f.out.at(0)).ip.payload);
    REQUIRE(syn);
    // Not the old connection's port and ISS, which the firmware may still hold in TIME_WAIT.
    CHECK(syn->src_port == 40001);
    CHECK(syn->src_port != first->src_port);
    CHECK(syn->seq != first->seq);

    // An ARP from the firmware's address teaches its MAC too.
    f.router.reset();
    Arp gratuitous{1, kFw, kGuest, {}, kGuest};
    f.fw(build_ethernet(kBroadcastMac, kFw, kEtherArp, build_arp(gratuitous)));
    CHECK(f.router.send_ip(kGateway, kProtoUdp, build_udp(kGateway, kGuest, 1, 2, Bytes{})));
}

TEST_CASE("router: DNS goes to the NAT's relay; broadcast, multicast, the LAN and loopback do not reach it") {
    Fixture f;
    Peer    remote, upstream;
    Nat     nat([&](uint32_t                 src,
                uint8_t                  protocol,
                std::span<const uint8_t> payload) { return f.router.send_ip(src, protocol, payload); },
            kGateway,
            ip4(127, 0, 0, 1),
            Logger::get("net.nat"),
            upstream.port);
    f.router.set_nat(&nat);
    f.lease();

    // Broadcast, multicast and the rest of the LAN are nobody's business.
    f.fw(udp_frame(kRouter, kFw, kGuest, ip4(10, 0, 2, 255), 5000, 9, {1}));
    f.fw(udp_frame(kBroadcastMac, kFw, kGuest, kAll, 5000, 9, {1}));
    f.fw(udp_frame(kRouter, kFw, kGuest, ip4(224, 0, 0, 251), 5353, 5353, {1}));
    f.fw(udp_frame(kRouter, kFw, kGuest, ip4(239, 255, 255, 250), 5000, 1900, {1}));
    f.fw(udp_frame(kRouter, kFw, kGuest, ip4(10, 0, 2, 99), 5000, 9, {1}));
    // The gateway's other ports neither.
    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 5000, 7, {1}));
    // Nor the host's loopback.
    f.fw(udp_frame(kRouter, kFw, kGuest, ip4(127, 0, 0, 1), 5000, remote.port, {'h', 'i'}));
    f.fw(udp_frame(kRouter, kFw, kGuest, ip4(127, 1, 2, 3), 5000, remote.port, {'h', 'i'}));
    CHECK(nat.udp_sessions() == 0);
    CHECK(f.out.empty());
    uint8_t buf[16];
    CHECK(::recv(remote.fd, buf, sizeof buf, 0) < 0);

    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 5001, 53, {'q'}));
    CHECK(nat.udp_sessions() == 1);
    REQUIRE(upstream.receive() == Bytes{'q'});
    upstream.answer({'a'});
    for (int i = 0; i < 1000 && f.out.empty(); ++i) {
        nat.poll(f.now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(f.out.size() == 1);
    Sent s = parse_sent(f.out[0]);
    CHECK(s.eth.dst == kFw);
    CHECK(s.ip.src == kGateway);
    CHECK(s.ip.dst == kGuest);
    auto udp = parse_udp(s.ip.src, s.ip.dst, s.ip.payload);
    REQUIRE(udp);
    CHECK(udp->src_port == 53);
    CHECK(udp->dst_port == 5001);
    CHECK(sub(udp->payload) == Bytes{'a'});
}

TEST_CASE("router: TCP beyond the LAN goes to the NAT, to loopback nowhere") {
    Fixture f;
    Nat     nat([&](uint32_t                 src,
                uint8_t                  protocol,
                std::span<const uint8_t> payload) { return f.router.send_ip(src, protocol, payload); },
            kGateway,
            std::nullopt,
            Logger::get("net.nat"));
    f.router.set_nat(&nat);
    f.lease();

    // The host's loopback is not the firmware's: no connection, no answer.
    f.fw(tcp_frame(kGuest, ip4(127, 0, 0, 1), Tcp{5000, 80, 1000, 0, kTcpSyn, 4096, uint16_t(1460), {}}));
    CHECK(nat.tcp_sessions() == 0);
    CHECK(f.out.empty());

    // A segment of no connection, beyond the LAN: the NAT resets it from the destination.
    const uint32_t remote = ip4(192, 0, 2, 1);
    f.fw(tcp_frame(kGuest, remote, Tcp{5000, 80, 1000, 4242, kTcpAck, 4096, std::nullopt, {}}));
    REQUIRE(f.out.size() == 1);
    Sent s = parse_sent(f.out[0]);
    CHECK(s.ip.src == remote);
    CHECK(s.ip.dst == kGuest);
    auto rst = parse_tcp(s.ip.src, s.ip.dst, s.ip.payload);
    REQUIRE(rst);
    CHECK(rst->flags == kTcpRst);
    CHECK(rst->seq == 4242);
    CHECK(rst->src_port == 80);
    CHECK(rst->dst_port == 5000);
}

TEST_CASE("router: no traffic reaches the NAT before the lease") {
    Fixture f;
    Peer    remote;
    Nat     nat([](uint32_t, uint8_t, std::span<const uint8_t>) { return true; },
            kGateway,
            ip4(127, 0, 0, 1),
            Logger::get("net.nat"),
            remote.port);
    f.router.set_nat(&nat);
    f.fw(udp_frame(kRouter, kFw, kGuest, ip4(127, 0, 0, 1), 5000, remote.port, {1}));
    f.fw(udp_frame(kRouter, kFw, kGuest, kGateway, 5000, 53, {1}));
    CHECK(nat.udp_sessions() == 0);
}
