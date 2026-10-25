#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <chrono>
#include <deque>
#include <functional>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/log.h"
#include "net/packets.h"
#include "net/router/router_network.h"
#include "net/stack.h"
#include "net/station.h"

using namespace bk;
using namespace bk::packets;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr Station::Mac kFw     = {0x22, 0xac, 0x01, 0x02, 0x03, 0x04};
constexpr Station::Mac kRouter = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

constexpr uint64_t kMs = 1'000'000;

ApConfig open_ap() {
    ApConfig ap;
    ap.ssid = "TESTNET";
    return ap;
}

// The firmware's side, played by the client classes: a Station that joins the
// network's access point and a Stack behind it that asks for a lease. Frames
// either side hands to the air are queued and passed over between ticks, never
// from inside one.
struct Air {
    std::ostringstream log;
    bool               logging = (Logger::reset_for_tests(), Logger::configure(log, false), true);
    uint64_t           now     = 0;
    uint64_t           wall    = 0;
    RouterNetwork      net;
    Station            station{kFw, "TESTNET", Logger::get("test.station")};
    Stack              stack{kFw, [this](Bytes f) { station.send_ethernet(f); }};
    std::deque<Bytes>  to_station, to_net;
    // Every Ethernet frame the firmware side received.
    std::vector<Bytes> received;

    // DNS goes to 127.0.0.1:`dns_port`, a socket of the test, never the host's resolver.
    explicit Air(std::vector<Publish> publishes = {}, uint16_t dns_port = Nat::kDnsPort, bool internet = true)
        : net(
              open_ap(),
              std::move(publishes),
              [] { return std::optional<unsigned>(); },
              2 * kMs,
              [this] { return wall; },
              ip4(127, 0, 0, 1),
              dns_port,
              internet) {
        net.attach([this](Bytes f) { to_station.push_back(std::move(f)); });
        station.attach([this](Bytes f) { to_net.push_back(std::move(f)); });
        station.set_ethernet_in([this](Bytes f) {
            received.push_back(f);
            stack.receive(f, now);
        });
        station.set_link_up([this](uint64_t t) { stack.link_up(t); });
    }
    ~Air() { Logger::reset_for_tests(); }

    void step(uint64_t dt = 10 * kMs) {
        now += dt;
        wall += 3 * kMs;
        net.tick(now);
        station.tick(now);
        stack.tick(now);
        for (int i = 0; i < 50 && (!to_station.empty() || !to_net.empty()); ++i) {
            while (!to_station.empty()) {
                Bytes f = std::move(to_station.front());
                to_station.pop_front();
                station.transmit(f);
            }
            while (!to_net.empty()) {
                Bytes f = std::move(to_net.front());
                to_net.pop_front();
                net.transmit(f);
            }
        }
    }

    bool run_until(const std::function<bool()> &done, int steps = 3000) {
        for (int i = 0; i < steps; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }

    bool bound() { return stack.dhcp_state() == Stack::Dhcp::Bound; }
};

} // namespace

TEST_CASE("router network: a virtual station joins, gets 10.0.2.15 and pings the gateway") {
    Air air;
    REQUIRE(air.run_until([&] { return air.bound(); }));
    CHECK(air.station.state() == Station::State::Run);
    CHECK(air.stack.ip() == ip4(10, 0, 2, 15));
    CHECK(air.stack.guest_ip() == ip4(10, 0, 2, 2));
    CHECK(air.net.access_point().station() == kFw);
    CHECK(air.net.router().guest_ip() == ip4(10, 0, 2, 15));

    const Bytes data = {9, 8, 7};
    air.received.clear();
    air.station.send_ethernet(build_ethernet(
        kRouter,
        kFw,
        kEtherIpv4,
        build_ipv4(ip4(10, 0, 2, 15), ip4(10, 0, 2, 2), kProtoIcmp, 1, build_icmp_echo({false, 5, 1, data}))));
    bool answered = air.run_until(
        [&] {
            for (const Bytes &f : air.received) {
                auto eth = parse_ethernet(f);
                if (!eth || eth->type != kEtherIpv4) continue;
                auto ip = parse_ipv4(eth->payload);
                if (!ip || ip->protocol != kProtoIcmp || ip->src != ip4(10, 0, 2, 2)) continue;
                auto echo = parse_icmp_echo(ip->payload);
                if (echo && echo->reply && echo->id == 5 && echo->seq == 1 &&
                    Bytes(echo->data.begin(), echo->data.end()) == data)
                    return true;
            }
            return false;
        },
        100);
    CHECK(answered);
}

TEST_CASE("router network: a published UDP port reaches the guest") {
    Air air({
        {"127.0.0.1", 0, 7000, true}
    });
    REQUIRE(air.net.addresses().size() == 1);
    uint16_t port = air.net.addresses()[0].host_port;
    CHECK(port != 0);
    REQUIRE(air.run_until([&] { return air.bound(); }));
    Bytes got;
    air.stack.bind_udp(7000,
                       [&](uint16_t, std::span<const uint8_t> payload) { got.assign(payload.begin(), payload.end()); });

    int         u      = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in to     = {};
    to.sin_family      = AF_INET;
    to.sin_port        = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(u >= 0);
    REQUIRE(::sendto(u, "hello", 5, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 5);
    bool arrived = air.run_until(
        [&] {
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            return !got.empty();
        },
        1000);
    ::close(u);
    REQUIRE(arrived);
    CHECK(got == Bytes{'h', 'e', 'l', 'l', 'o'});
}

TEST_CASE("router network: a published UDP client from before the join gets replies") {
    Air         air({
        {"127.0.0.1", 0, 7000, true}
    });
    uint16_t    port   = air.net.addresses().at(0).host_port;
    int         u      = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in to     = {};
    to.sin_family      = AF_INET;
    to.sin_port        = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(u >= 0);
    // The client speaks while the link is down: its binding is made, the datagram is dropped.
    REQUIRE(::sendto(u, "early", 5, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 5);
    for (int i = 0; i < 20; ++i) {
        air.step();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    REQUIRE(air.run_until([&] { return air.bound(); }));

    std::optional<uint16_t> from;
    air.stack.bind_udp(7000, [&](uint16_t src_port, std::span<const uint8_t>) { from = src_port; });
    REQUIRE(::sendto(u, "hello", 5, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 5);
    REQUIRE(air.run_until(
        [&] {
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            return from.has_value();
        },
        1000));
    // The firmware's reply reaches the client.
    REQUIRE(air.stack.send_udp(7000, *from, Bytes{'o', 'k'}));
    char buf[16];
    bool replied = air.run_until(
        [&] {
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            return ::recv(u, buf, sizeof buf, 0) == 2;
        },
        1000);
    ::close(u);
    CHECK(replied);
}

TEST_CASE("router network: without internet nothing leaves the LAN") {
    // The DNS server, played by a socket on 127.0.0.1.
    int         u = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(u >= 0);
    REQUIRE(::bind(u, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
    socklen_t len = sizeof sa;
    REQUIRE(::getsockname(u, reinterpret_cast<sockaddr *>(&sa), &len) == 0);

    Air air({}, ntohs(sa.sin_port), false);
    REQUIRE(air.run_until([&] { return air.bound(); }));
    auto udp_to = [&](uint32_t dst, uint16_t to) {
        air.station.send_ethernet(build_ethernet(
            kRouter,
            kFw,
            kEtherIpv4,
            build_ipv4(ip4(10, 0, 2, 15), dst, kProtoUdp, 1, build_udp(ip4(10, 0, 2, 15), dst, 5000, to, Bytes{'x'}))));
    };
    // A DNS query and a datagram to a remote address are dropped.
    udp_to(ip4(10, 0, 2, 2), 53);
    udp_to(ip4(203, 0, 113, 10), 4999);
    for (int i = 0; i < 300; ++i) {
        air.step();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    CHECK(air.net.nat().udp_sessions() == 0);
    uint8_t probe[64];
    CHECK(::recv(u, probe, sizeof probe, 0) < 0);
    ::close(u);
}

TEST_CASE("router network: joining again drops the NAT sessions") {
    // The DNS server, played by a socket on 127.0.0.1.
    int         u = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(u >= 0);
    REQUIRE(::bind(u, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
    socklen_t len = sizeof sa;
    REQUIRE(::getsockname(u, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
    const uint16_t port = ntohs(sa.sin_port);

    Air air({}, port);
    REQUIRE(air.run_until([&] { return air.bound(); }));
    auto udp_to = [&](uint32_t dst, uint16_t from, uint16_t to) {
        air.station.send_ethernet(build_ethernet(
            kRouter,
            kFw,
            kEtherIpv4,
            build_ipv4(ip4(10, 0, 2, 15), dst, kProtoUdp, 1, build_udp(ip4(10, 0, 2, 15), dst, from, to, Bytes{'x'}))));
    };
    // A DNS query to the gateway opens a NAT session.
    auto send = [&](uint16_t from) { udp_to(ip4(10, 0, 2, 2), from, 53); };

    // The host's loopback is out of the firmware's reach.
    udp_to(ip4(127, 0, 0, 1), 4999, port);
    for (int i = 0; i < 20; ++i) air.step();
    CHECK(air.net.nat().udp_sessions() == 0);
    uint8_t probe[64];
    CHECK(::recv(u, probe, sizeof probe, 0) < 0);
    auto received = [&] {
        uint8_t buf[64];
        for (int i = 0; i < 1000; ++i) {
            air.step();
            if (::recv(u, buf, sizeof buf, 0) == 1) return true;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        return false;
    };

    send(5000);
    REQUIRE(received());
    CHECK(air.net.nat().udp_sessions() == 1);
    CHECK(air.net.next_event_ns().has_value());

    // The firmware starts over: the link goes down and comes back.
    air.station.reset();
    REQUIRE(air.run_until([&] { return air.station.state() != Station::State::Run; }));
    REQUIRE(air.run_until([&] { return air.station.state() == Station::State::Run && air.bound(); }));
    CHECK(air.net.nat().udp_sessions() == 0);

    // A reset of the network drops them too.
    send(5001);
    REQUIRE(received());
    CHECK(air.net.nat().udp_sessions() == 1);
    air.net.reset();
    CHECK(air.net.nat().udp_sessions() == 0);
    ::close(u);
}

TEST_CASE("router network: without publishes the wall clock is never read") {
    std::ostringstream log;
    Logger::reset_for_tests();
    Logger::configure(log, false);
    int reads = 0;
    {
        RouterNetwork net(open_ap(), {}, {}, 2 * kMs, [&] {
            ++reads;
            return uint64_t(0);
        });
        for (uint64_t t = 0; t < 100; ++t) net.tick(t * 10'000'000);
    }
    CHECK(reads == 0);
    Logger::reset_for_tests();
}
