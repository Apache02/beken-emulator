#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <cerrno>
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
#include "fake_guest.h"
#include "net/network.h"
#include "net/packets.h"

using namespace bk;
using bk::testing::FakeGuest;
using Bytes = std::vector<uint8_t>;
using Mac   = Network::Mac;

namespace {

constexpr Mac kSta = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr Mac kAp  = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};

constexpr uint64_t kMs = 1'000'000;

Bytes bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }

void append(Bytes &to, const Bytes &from) { to.insert(to.end(), from.begin(), from.end()); }

void append(Bytes &to, const Mac &m) { to.insert(to.end(), m.begin(), m.end()); }

Bytes header(uint8_t fc0, uint8_t fc1, const Mac &a1, const Mac &a2, const Mac &a3) {
    Bytes h{fc0, fc1, 0, 0};
    append(h, a1);
    append(h, a2);
    append(h, a3);
    h.insert(h.end(), {0, 0});
    return h;
}

// The firmware's side of the air: an access point that answers every join step,
// plus a fake guest behind it that speaks DHCP, TCP and UDP. Frames the network
// hands to the MAC are collected and answered between ticks, never from inside
// one.
struct Air {
    std::ostringstream log;
    bool               logging = (Logger::reset_for_tests(), Logger::configure(log, false), true);
    uint64_t           now     = 0;
    uint64_t           wall    = 0;
    Network            net;
    FakeGuest          guest;
    std::deque<Bytes>  inbox;
    std::string        ap_ssid;
    uint16_t           aid = 0xc001;

    explicit Air(std::vector<Publish>       publishes = {},
                 std::optional<std::string> ssid      = std::nullopt,
                 std::string                ap        = "cam")
        : net(std::move(publishes), kSta, std::move(ssid), 2 * kMs, [this] { return wall; }), ap_ssid(std::move(ap)) {
        net.attach([this](Bytes f) { inbox.push_back(std::move(f)); });
    }
    ~Air() { Logger::reset_for_tests(); }

    Bytes beacon_body(uint16_t capability) {
        Bytes b(8, 0);
        b.insert(b.end(), {0x64, 0x00, uint8_t(capability), uint8_t(capability >> 8), 0, uint8_t(ap_ssid.size())});
        append(b, bytes(ap_ssid));
        return b;
    }

    // What the firmware does with a frame the station sent.
    void handle(const Bytes &f) {
        if (f.size() < 24) return;
        uint8_t type = (f[0] >> 2) & 3, subtype = f[0] >> 4;
        Mac     src{}, dst{};
        std::copy(f.begin() + 10, f.begin() + 16, src.begin());
        std::copy(f.begin() + 16, f.begin() + 22, dst.begin());
        if (type == 0) {
            Bytes out;
            if (subtype == 4) { // probe request -> probe response
                out = header(0x50, 0, src, kAp, kAp);
                append(out, beacon_body(0x0401));
            } else if (subtype == 11) { // authentication -> sequence 2, success
                out = header(0xb0, 0, src, kAp, kAp);
                append(out, Bytes{0, 0, 2, 0, 0, 0});
            } else if (subtype == 0) { // association -> success
                out = header(0x10, 0, src, kAp, kAp);
                append(out, Bytes{0x01, 0x04, 0, 0, uint8_t(aid), uint8_t(aid >> 8)});
            } else {
                return;
            }
            net.transmit(out);
        } else if (type == 2 && f.size() >= 32) {
            Bytes eth;
            append(eth, dst);
            append(eth, src);
            eth.insert(eth.end(), f.begin() + 30, f.end());
            guest.on_frame(eth);
        }
    }

    // The guest's frames go out as data frames from the access point.
    void forward_guest() {
        while (!guest.outbox.empty()) {
            Bytes e = std::move(guest.outbox.front());
            guest.outbox.pop_front();
            auto eth = packets::parse_ethernet(e);
            if (!eth) continue;
            Bytes f = header(0x08, 0x02, eth->dst, kAp, eth->src);
            f.insert(f.end(), {0xaa, 0xaa, 0x03, 0, 0, 0, uint8_t(eth->type >> 8), uint8_t(eth->type)});
            f.insert(f.end(), eth->payload.begin(), eth->payload.end());
            net.transmit(f);
        }
    }

    void step(uint64_t dt = 10 * kMs) {
        now += dt;
        wall += 3 * kMs;
        net.tick(now);
        for (int i = 0; i < 50 && (!inbox.empty() || !guest.outbox.empty()); ++i) {
            while (!inbox.empty()) {
                Bytes f = std::move(inbox.front());
                inbox.pop_front();
                handle(f);
            }
            forward_guest();
        }
    }

    bool run_until(const std::function<bool()> &done, int steps = 3000) {
        for (int i = 0; i < steps; ++i) {
            if (done()) return true;
            step();
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        return done();
    }

    bool associated() { return net.station().state() == Station::State::Run; }
    bool bound() { return net.stack().dhcp_state() == Stack::Dhcp::Bound; }

    void join() {
        REQUIRE(run_until([&] { return associated() && bound(); }));
    }
};

struct Fd {
    int fd = -1;
    explicit Fd(int f = -1) : fd(f) {}
    Fd(Fd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

sockaddr_in loopback(uint16_t port) {
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sa;
}

Fd tcp_client(uint16_t port) {
    Fd s(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
    REQUIRE(s.fd >= 0);
    sockaddr_in sa = loopback(port);
    int         rc = ::connect(s.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa);
    REQUIRE((rc == 0 || errno == EINPROGRESS));
    return s;
}

// What the socket has now, and whether the peer has closed.
Bytes drain(int fd, bool *closed = nullptr) {
    Bytes   out;
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            out.insert(out.end(), buf, buf + n);
        } else {
            if (closed && (n == 0 || errno != EAGAIN)) *closed = true;
            return out;
        }
    }
}

} // namespace

TEST_CASE("network: the station joins, gets an address and carries a published TCP connection both ways") {
    Air air({
        {"127.0.0.1", 0, 8080, false}
    });
    REQUIRE(air.net.addresses().size() == 1);
    uint16_t port = air.net.addresses()[0].host_port;
    air.guest.listening.insert(8080);
    // Nothing is joined before the firmware shows an access point.
    CHECK(air.net.station().state() == Station::State::Scan);

    air.join();
    CHECK(air.log.str().find("[I] emulator.net.wifi: associated with 'cam' (AID 1)") != std::string::npos);
    CHECK(air.net.stack().ip() == FakeGuest::kLease);
    CHECK(air.net.stack().guest_ip() == FakeGuest::kIp);

    Fd client = tcp_client(port);
    REQUIRE(air.run_until([&] { return air.guest.connection() != nullptr; }));
    auto &c = *air.guest.connection();
    REQUIRE(::send(client.fd, "GET /", 5, MSG_NOSIGNAL) == 5);
    REQUIRE(air.run_until([&] { return c.received.size() == 5; }));
    CHECK(c.received == bytes("GET /"));

    air.guest.send(c, bytes("200 OK"));
    Bytes got;
    REQUIRE(air.run_until([&] {
        Bytes more = drain(client.fd);
        got.insert(got.end(), more.begin(), more.end());
        return got.size() == 6;
    }));
    CHECK(got == bytes("200 OK"));

    air.guest.send_fin(c);
    ::shutdown(client.fd, SHUT_WR);
    bool closed = false;
    REQUIRE(air.run_until([&] {
        drain(client.fd, &closed);
        return closed && air.net.forwarder().connections() == 0;
    }));
    CHECK_FALSE(c.got_rst);
    CHECK(c.got_fin);
}

TEST_CASE("network: a published UDP port reaches the firmware") {
    Air air({
        {"127.0.0.1", 0, 5000, true}
    });
    air.join();
    Fd          u(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0));
    sockaddr_in me = loopback(0);
    REQUIRE(::bind(u.fd, reinterpret_cast<sockaddr *>(&me), sizeof me) == 0);
    sockaddr_in to = loopback(air.net.addresses()[0].host_port);
    REQUIRE(::sendto(u.fd, "ping", 4, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 4);
    REQUIRE(air.run_until([&] { return !air.guest.datagrams.empty(); }));
    CHECK(air.guest.datagrams[0].to_port == 5000);
    CHECK(air.guest.datagrams[0].data == bytes("ping"));
    air.guest.send_udp(5000, air.guest.datagrams[0].from_port, bytes("pong"));
    Bytes got;
    REQUIRE(air.run_until([&] {
        got = drain(u.fd);
        return !got.empty();
    }));
    CHECK(got == bytes("pong"));
}

TEST_CASE("network: the SSID filter keeps the station out of other networks") {
    Air other({}, "wanted", "other");
    other.run_until([&] { return other.net.station().state() != Station::State::Scan; }, 400);
    CHECK(other.net.station().state() == Station::State::Scan);

    Air same({}, "wanted", "wanted");
    same.join();
    CHECK(same.net.station().state() == Station::State::Run);
}

TEST_CASE("network: the forwarder is polled at most once per poll interval of wall time") {
    Air air({
        {"127.0.0.1", 0, 80, false}
    });
    // The first tick polls at once.
    air.wall = 100 * kMs;
    air.net.tick(0);
    Fd client = tcp_client(air.net.addresses()[0].host_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    air.wall = 100 * kMs + 1'999'999;
    air.net.tick(1);
    CHECK(air.net.forwarder().connections() == 0);
    air.wall = 102 * kMs;
    air.net.tick(2);
    CHECK(air.net.forwarder().connections() == 1);
    // The interval counts from the latest poll.
    Fd second = tcp_client(air.net.addresses()[0].host_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    air.wall = 103 * kMs;
    air.net.tick(3);
    CHECK(air.net.forwarder().connections() == 1);
    air.wall = 104 * kMs;
    air.net.tick(4);
    CHECK(air.net.forwarder().connections() == 2);
}

TEST_CASE("network: without publishes the wall clock is never read") {
    int     reads = 0;
    Network net({}, kSta, std::nullopt, 2 * kMs, [&] {
        ++reads;
        return uint64_t(0);
    });
    for (uint64_t t = 0; t < 100; ++t) net.tick(t * 10'000);
    CHECK(reads == 0);
}

TEST_CASE("network: the scheduler hears about the station's events") {
    Air air({
        {"127.0.0.1", 0, 80, false}
    });
    REQUIRE(air.net.next_event_ns());
    CHECK(*air.net.next_event_ns() == Station::RETRY_NS);
    CHECK(air.net.next_event_ns() == air.net.station().next_event_ns());
    air.join();
    // Associated and bound: nothing left to wake for (published ports are
    // polled on every tick, not scheduled).
    CHECK_FALSE(air.net.next_event_ns());
}

TEST_CASE("network: a publish that cannot bind is a ForwardError") {
    Network  first({
        {"127.0.0.1", 0, 80, false}
    });
    uint16_t port = first.addresses()[0].host_port;
    CHECK_THROWS_AS(Network({
                        {"127.0.0.1", port, 81, false}
    }),
                    ForwardError);
}

TEST_CASE("network: reset returns to scanning and drops the connections; the listener stays") {
    Air      air({
        {"127.0.0.1", 0, 8080, false}
    });
    uint16_t port = air.net.addresses()[0].host_port;
    air.guest.listening.insert(8080);
    air.join();
    Fd client = tcp_client(port);
    REQUIRE(air.run_until([&] { return air.guest.connection() != nullptr; }));

    air.net.reset();
    CHECK(air.net.forwarder().connections() == 0);
    CHECK(air.net.station().state() == Station::State::Scan);
    CHECK(air.net.stack().dhcp_state() == Stack::Dhcp::None);
    air.inbox.clear();
    air.forward_guest();
    bool closed = false;
    REQUIRE(air.run_until([&] {
        drain(client.fd, &closed);
        return closed;
    }));

    // The firmware is still there: the station joins again and the same
    // published port works.
    air.guest.connections.clear();
    air.join();
    Fd again = tcp_client(port);
    REQUIRE(air.run_until([&] { return air.guest.connection() != nullptr; }));
}

TEST_CASE("network: joining again ends the connections of the previous association") {
    Air air({
        {"127.0.0.1", 0, 8080, false}
    });
    air.guest.listening.insert(8080);
    air.join();
    Fd client = tcp_client(air.net.addresses()[0].host_port);
    REQUIRE(air.run_until([&] { return air.guest.connection() != nullptr; }));

    // A deauthentication from the access point.
    Bytes deauth = header(0xc0, 0, kSta, kAp, kAp);
    deauth.insert(deauth.end(), {3, 0});
    air.net.transmit(deauth);
    CHECK(air.net.station().state() == Station::State::Auth);
    bool closed = false;
    air.guest.connections.clear();
    REQUIRE(air.run_until([&] { return air.associated() && air.bound(); }));
    drain(client.fd, &closed);
    CHECK(closed);
    CHECK(air.net.forwarder().connections() == 0);
}

TEST_CASE("network: close releases the host ports") {
    Air      air({
        {"127.0.0.1", 0, 80, false}
    });
    uint16_t port = air.net.addresses()[0].host_port;
    air.net.close();
    Fd          c(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in sa = loopback(port);
    CHECK(::connect(c.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == -1);
    CHECK(errno == ECONNREFUSED);
    // The air still works.
    air.join();
}
