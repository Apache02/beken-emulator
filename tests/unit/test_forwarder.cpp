#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
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
#include "net/forwarder.h"
#include "net/packets.h"
#include "net/stack.h"

using namespace bk;
using bk::testing::FakeGuest;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr packets::Mac kMe = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

constexpr uint64_t kSec = 1'000'000'000;

Bytes bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }

// A host socket of the test, closed on scope exit.
struct Sock {
    int fd = -1;
    explicit Sock(int f = -1) : fd(f) {}
    Sock(Sock &&o) noexcept : fd(o.fd) { o.fd = -1; }
    Sock &operator=(Sock &&o) noexcept {
        std::swap(fd, o.fd);
        return *this;
    }
    ~Sock() {
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

// A non-blocking TCP client connected to the published port.
Sock tcp_client(uint16_t port) {
    Sock s(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
    REQUIRE(s.fd >= 0);
    sockaddr_in sa = loopback(port);
    int         rc = ::connect(s.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa);
    REQUIRE((rc == 0 || errno == EINPROGRESS));
    return s;
}

Sock udp_client() {
    Sock s(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0));
    REQUIRE(s.fd >= 0);
    sockaddr_in sa = loopback(0);
    REQUIRE(::bind(s.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
    return s;
}

void send_all(int fd, const Bytes &data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0)
            off += size_t(n);
        else if (n < 0 && errno == EAGAIN)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else
            FAIL("send: " << std::strerror(errno));
    }
}

struct Received {
    Bytes data;
    bool  eof   = false; // an orderly close
    int   error = 0;
};

// Whatever the socket has now.
Received drain(int fd) {
    Received r;
    uint8_t  buf[65536];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            r.data.insert(r.data.end(), buf, buf + n);
        } else if (n == 0) {
            r.eof = true;
            break;
        } else {
            if (errno != EAGAIN) r.error = errno;
            break;
        }
    }
    return r;
}

// A stack wired to a fake guest. The guest's frames reach the stack only in
// `pump`, never from inside a stack call.
struct Rig {
    FakeGuest          guest;
    std::deque<Bytes>  to_stack;
    Stack              stack;
    uint64_t           now = 0;
    std::ostringstream log;

    Rig() : stack(kMe, [this](Bytes f) { guest.on_frame(f); }) {
        Logger::reset_for_tests();
        Logger::configure(log, false);
    }
    ~Rig() { Logger::reset_for_tests(); }

    void pump() {
        for (;;) {
            if (guest.outbox.empty()) return;
            Bytes f = std::move(guest.outbox.front());
            guest.outbox.pop_front();
            stack.receive(f, now);
        }
    }

    void bind() {
        stack.link_up(now);
        pump();
        REQUIRE(stack.dhcp_state() == Stack::Dhcp::Bound);
    }

    void poll(Forwarder &fw) {
        fw.poll(now);
        pump();
        stack.tick(now);
        pump();
    }

    // Polls (with a pause, because the kernel moves loopback data on its own
    // schedule) until `done` or two seconds pass.
    bool settle(Forwarder &fw, const std::function<bool()> &done) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end) {
            poll(fw);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }
};

Publish tcp_publish(uint16_t guest) { return {"127.0.0.1", 0, guest, false}; }
Publish udp_publish(uint16_t guest) { return {"127.0.0.1", 0, guest, true}; }

} // namespace

TEST_CASE("parse_publish takes the documented forms") {
    Publish p = parse_publish("8080:80");
    CHECK(p.host_ip == "127.0.0.1");
    CHECK(p.host_port == 8080);
    CHECK(p.guest_port == 80);
    CHECK_FALSE(p.udp);

    p = parse_publish("8080:80/tcp");
    CHECK_FALSE(p.udp);

    p = parse_publish("0.0.0.0:5000:6000/udp");
    CHECK(p.host_ip == "0.0.0.0");
    CHECK(p.host_port == 5000);
    CHECK(p.guest_port == 6000);
    CHECK(p.udp);

    p = parse_publish("192.168.1.5:1:65535");
    CHECK(p.host_ip == "192.168.1.5");
    CHECK(p.host_port == 1);
    CHECK(p.guest_port == 65535);
}

TEST_CASE("parse_publish rejects what it cannot read") {
    for (const char *bad : {"",
                            "80",
                            "a:b",
                            "80:0",
                            "0:80",
                            "65536:80",
                            "80:65536",
                            "-1:80",
                            "+80:80",
                            "80:80/sctp",
                            "80:80/",
                            "80:80/tcp/udp",
                            "1:2:3:4",
                            "localhost:80:80",
                            "300.1.1.1:80:80",
                            ":80:80",
                            "80::80",
                            "80:80:",
                            "1234567:80"}) {
        try {
            parse_publish(bad);
            FAIL("accepted '" << bad << "'");
        } catch (const ForwardError &e) {
            CHECK(std::string(e.what()) == std::string("bad --publish '") + bad +
                                               "' (want [HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp], ports 1-65535)");
        }
    }
}

TEST_CASE("describe prints protocol, host address and guest port") {
    CHECK(describe({"127.0.0.1", 8080, 80, false}) == "tcp 127.0.0.1:8080 -> guest port 80");
    CHECK(describe({"0.0.0.0", 53, 5353, true}) == "udp 0.0.0.0:53 -> guest port 5353");
}

TEST_CASE("forwarder reports the real ports it bound") {
    Rig       rig;
    Forwarder fw(rig.stack, {tcp_publish(80), udp_publish(53)});
    REQUIRE(fw.addresses().size() == 2);
    CHECK(fw.addresses()[0].host_port != 0);
    CHECK_FALSE(fw.addresses()[0].udp);
    CHECK(fw.addresses()[0].guest_port == 80);
    CHECK(fw.addresses()[1].host_port != 0);
    CHECK(fw.addresses()[1].udp);
    CHECK(fw.addresses()[1].host_ip == "127.0.0.1");
}

TEST_CASE("forwarder with no publishes does nothing") {
    Rig       rig;
    Forwarder fw(rig.stack, {});
    CHECK(fw.empty());
    fw.poll(0);
    fw.reset();
    fw.close();
}

TEST_CASE("a publish that cannot bind is an error and closes what was opened") {
    Rig rig;
    // A port that was free a moment ago, for the first publish.
    uint16_t first;
    {
        Sock        probe(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in sa = loopback(0);
        REQUIRE(::bind(probe.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
        socklen_t len = sizeof sa;
        REQUIRE(::getsockname(probe.fd, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
        first = ntohs(sa.sin_port);
    }
    // A taken port for the second.
    Sock        taken(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in sa = loopback(0);
    REQUIRE(::bind(taken.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
    REQUIRE(::listen(taken.fd, 1) == 0);
    socklen_t len = sizeof sa;
    REQUIRE(::getsockname(taken.fd, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
    uint16_t busy = ntohs(sa.sin_port);

    try {
        Forwarder fw(rig.stack,
                     {
                         {"127.0.0.1", first, 80, false},
                         {"127.0.0.1", busy,  81, false}
        });
        FAIL("the second publish should not bind");
    } catch (const ForwardError &e) {
        CHECK(std::string(e.what()) ==
              "cannot publish 127.0.0.1:" + std::to_string(busy) + " to guest port 81/tcp: Address already in use");
    }
    // The first listener is gone again.
    Sock        c(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in to = loopback(first);
    CHECK(::connect(c.fd, reinterpret_cast<sockaddr *>(&to), sizeof to) == -1);
    CHECK(errno == ECONNREFUSED);
}

TEST_CASE("a publish on a host address that does not exist is an error") {
    Rig rig;
    CHECK_THROWS_AS(Forwarder(rig.stack,
                              {
                                  {"203.0.113.9", 40123, 80, false}
    }),
                    ForwardError);
    CHECK_THROWS_WITH_AS(Forwarder(rig.stack,
                                   {
                                       {"nonsense", 40123, 80, true}
    }),
                         "cannot publish nonsense:40123 to guest port 80/udp: not an IPv4 address",
                         ForwardError);
}

TEST_CASE("TCP: data goes both ways and a connection is torn down by both closes") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);

    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    CHECK(fw.connections() == 1);
    auto &c = *rig.guest.connection();

    send_all(client.fd, bytes("hello"));
    REQUIRE(rig.settle(fw, [&] { return c.received.size() == 5; }));
    CHECK(c.received == bytes("hello"));

    rig.guest.send(c, bytes("world"));
    Received got;
    REQUIRE(rig.settle(fw, [&] {
        Received r = drain(client.fd);
        got.data.insert(got.data.end(), r.data.begin(), r.data.end());
        return got.data.size() == 5;
    }));
    CHECK(got.data == bytes("world"));

    // The host closes its side: a FIN goes to the firmware.
    ::shutdown(client.fd, SHUT_WR);
    REQUIRE(rig.settle(fw, [&] { return c.got_fin; }));
    CHECK(fw.connections() == 1);
    // The firmware closes too: the host sees the end and the connection goes.
    rig.guest.send_fin(c);
    REQUIRE(rig.settle(fw, [&] {
        got = drain(client.fd);
        return got.eof;
    }));
    CHECK(rig.settle(fw, [&] { return fw.connections() == 0; }));
    CHECK_FALSE(c.got_rst);
}

TEST_CASE("TCP: a connection to a closed guest port ends with the reset") {
    Rig rig;
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(81)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.syns == 1; }));
    CHECK(rig.settle(fw, [&] { return fw.connections() == 0; }));
    Received r;
    CHECK(rig.settle(fw, [&] {
        r = drain(client.fd);
        return r.eof || r.error != 0;
    }));
}

TEST_CASE("TCP: the firmware resetting drops the connection") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    rig.guest.send_rst(*rig.guest.connection());
    CHECK(rig.settle(fw, [&] { return fw.connections() == 0; }));
    Received r;
    CHECK(rig.settle(fw, [&] {
        r = drain(client.fd);
        return r.eof || r.error != 0;
    }));
}

TEST_CASE("TCP: a host socket error aborts the flow and drops the connection") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    auto &c = *rig.guest.connection();
    // Closing with a zero linger time resets the connection.
    linger lin{1, 0};
    ::setsockopt(client.fd, SOL_SOCKET, SO_LINGER, &lin, sizeof lin);
    client.close();
    CHECK(rig.settle(fw, [&] { return fw.connections() == 0; }));
    CHECK(rig.settle(fw, [&] { return c.got_rst; }));
}

TEST_CASE("TCP: end of input from the host closes the flow") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    send_all(client.fd, bytes("bye"));
    client.close();
    auto &c = *rig.guest.connection();
    REQUIRE(rig.settle(fw, [&] { return c.got_fin; }));
    CHECK(c.received == bytes("bye"));
}

TEST_CASE("TCP: a close by the firmware shuts down only the write side of the host socket") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    auto &c = *rig.guest.connection();
    rig.guest.send(c, bytes("last words"));
    rig.guest.send_fin(c);
    Received got;
    REQUIRE(rig.settle(fw, [&] {
        Received r = drain(client.fd);
        got.data.insert(got.data.end(), r.data.begin(), r.data.end());
        got.eof = got.eof || r.eof;
        return got.eof;
    }));
    CHECK(got.data == bytes("last words"));
    // The host can still talk to the firmware.
    send_all(client.fd, bytes("ack"));
    CHECK(rig.settle(fw, [&] { return c.received == bytes("ack"); }));
    CHECK(fw.connections() == 1);
}

TEST_CASE("TCP: one poll reads at most 16384 bytes") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    Bytes     data(40000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7);
    send_all(client.fd, data);
    // Loopback hands the connection and the bytes over together; give it a
    // moment, then count the polls that move data.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.poll(fw);
    REQUIRE(rig.guest.connection());
    auto &c = *rig.guest.connection();
    CHECK(c.received.size() == 16384);
    rig.poll(fw);
    CHECK(c.received.size() == 32768);
    rig.poll(fw);
    CHECK(c.received.size() == 40000);
    CHECK(c.received == data);
}

TEST_CASE("TCP: an upload larger than the flow's send buffer still arrives whole") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    send_all(client.fd, Bytes(200000, 'x'));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // The polls below do not pump, so the flow stays in SYN-SENT and fills up;
    // the forwarder has to stop reading at the limit instead of buffering it all.
    for (int i = 0; i < 10; ++i) fw.poll(rig.now);
    rig.pump();
    REQUIRE(
        rig.settle(fw, [&] { return rig.guest.connection() && rig.guest.connection()->received.size() == 200000; }));
}

TEST_CASE("TCP: bytes the host socket does not take yet are kept and sent later") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    auto &c = *rig.guest.connection();

    // More than both socket buffers of loopback can hold.
    Bytes data(16'000'000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 31 + i / 251);
    rig.guest.send(c, data);
    rig.pump();
    // The host does not read: the socket fills and the rest waits in the forwarder.
    for (int i = 0; i < 50; ++i) rig.poll(fw);
    Bytes got;
    CHECK(fw.connections() == 1);
    REQUIRE(rig.settle(fw, [&] {
        Received r = drain(client.fd);
        got.insert(got.end(), r.data.begin(), r.data.end());
        return got.size() >= data.size();
    }));
    CHECK(got == data);
}

TEST_CASE("TCP: a client waits for an address and is dropped after 30 seconds with a warning") {
    Rig       rig;
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    // Accepted at device time 5 s.
    rig.now = 5 * kSec;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rig.poll(fw);
    REQUIRE(fw.connections() == 1);
    rig.now = 5 * kSec + 29'999'999'999;
    rig.poll(fw);
    CHECK(fw.connections() == 1);
    CHECK(rig.log.str().empty());
    rig.now = 35 * kSec;
    rig.poll(fw);
    CHECK(fw.connections() == 0);
    CHECK(rig.log.str() == "WARNING emulator.net.forward: connection from 127.0.0.1:" + [&] {
        sockaddr_in sa{};
        socklen_t   len = sizeof sa;
        ::getsockname(client.fd, reinterpret_cast<sockaddr *>(&sa), &len);
        return std::to_string(ntohs(sa.sin_port));
    }() + " dropped: the firmware got no address\n");
    Received r = drain(client.fd);
    CHECK((r.eof || r.error != 0));
}

TEST_CASE("TCP: a client that waits is served as soon as the firmware has an address") {
    Rig rig;
    rig.guest.listening.insert(80);
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      client = tcp_client(fw.addresses()[0].host_port);
    send_all(client.fd, bytes("early"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rig.now = kSec;
    rig.poll(fw);
    REQUIRE(fw.connections() == 1);
    CHECK(rig.guest.syns == 0);
    rig.now = 10 * kSec;
    rig.bind();
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() && rig.guest.connection()->received.size() == 5; }));
    CHECK(rig.log.str().empty());
}

TEST_CASE("TCP: several clients get their own flows") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    Sock      a = tcp_client(fw.addresses()[0].host_port);
    Sock      b = tcp_client(fw.addresses()[0].host_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connections.size() == 2; }));
    CHECK(rig.guest.connections[0].peer_port != rig.guest.connections[1].peer_port);
    send_all(a.fd, bytes("A"));
    send_all(b.fd, bytes("B"));
    REQUIRE(rig.settle(
        fw, [&] { return rig.guest.connections[0].received.size() + rig.guest.connections[1].received.size() == 2; }));
    // Replies stay with their own connection.
    rig.guest.send(rig.guest.connections[1], bytes("to the second"));
    Received got;
    REQUIRE(rig.settle(fw, [&] {
        got        = drain(a.fd);
        Received r = drain(b.fd);
        got.data.insert(got.data.end(), r.data.begin(), r.data.end());
        return !got.data.empty();
    }));
    CHECK(got.data == bytes("to the second"));
}

TEST_CASE("UDP: each client gets its own local port and its own replies") {
    Rig rig;
    rig.bind();
    Forwarder   fw(rig.stack, {udp_publish(5000)});
    uint16_t    port    = fw.addresses()[0].host_port;
    Sock        a       = udp_client();
    Sock        b       = udp_client();
    sockaddr_in to      = loopback(port);
    auto        send_to = [&](Sock &s, const std::string &text) {
        REQUIRE(::sendto(s.fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof to) ==
                ssize_t(text.size()));
    };
    send_to(a, "from a");
    send_to(b, "from b");
    REQUIRE(rig.settle(fw, [&] { return rig.guest.datagrams.size() == 2; }));
    CHECK(fw.udp_clients() == 2);
    // Copies: the vector grows below.
    auto da = rig.guest.datagrams[0];
    auto db = rig.guest.datagrams[1];
    CHECK(da.to_port == 5000);
    CHECK(da.from_port >= Stack::FIRST_PORT);
    CHECK(da.from_port != db.from_port);
    CHECK(da.data == bytes("from a"));
    CHECK(db.data == bytes("from b"));

    rig.guest.send_udp(5000, db.from_port, bytes("reply b"));
    rig.pump();
    Received got;
    REQUIRE(rig.settle(fw, [&] {
        got = drain(b.fd);
        return !got.data.empty();
    }));
    CHECK(got.data == bytes("reply b"));
    CHECK(drain(a.fd).data.empty());

    // A second datagram from `a` reuses its port.
    send_to(a, "again");
    REQUIRE(rig.settle(fw, [&] { return rig.guest.datagrams.size() == 3; }));
    CHECK(rig.guest.datagrams[2].from_port == da.from_port);
    CHECK(fw.udp_clients() == 2);
}

TEST_CASE("UDP: datagrams before the firmware has an address are dropped") {
    Rig         rig;
    Forwarder   fw(rig.stack, {udp_publish(5000)});
    Sock        a  = udp_client();
    sockaddr_in to = loopback(fw.addresses()[0].host_port);
    REQUIRE(::sendto(a.fd, "x", 1, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 1);
    rig.settle(fw, [&] { return fw.udp_clients() == 1; });
    rig.bind();
    rig.poll(fw);
    CHECK(rig.guest.datagrams.empty());
}

TEST_CASE("UDP: a client idle for 60 seconds is forgotten and its port released") {
    Rig rig;
    rig.bind();
    Forwarder   fw(rig.stack, {udp_publish(5000)});
    Sock        a  = udp_client();
    sockaddr_in to = loopback(fw.addresses()[0].host_port);
    rig.now        = 10 * kSec;
    REQUIRE(::sendto(a.fd, "x", 1, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 1);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.datagrams.size() == 1; }));
    uint16_t first = rig.guest.datagrams[0].from_port;

    rig.now = 10 * kSec + 59'999'999'999;
    rig.poll(fw);
    CHECK(fw.udp_clients() == 1);
    rig.now = 70 * kSec;
    rig.poll(fw);
    CHECK(fw.udp_clients() == 0);

    // A late reply finds no handler.
    rig.guest.send_udp(5000, first, bytes("late"));
    rig.pump();
    CHECK(drain(a.fd).data.empty());
    // The client is new again, with a new port.
    REQUIRE(::sendto(a.fd, "y", 1, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 1);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.datagrams.size() == 2; }));
    CHECK(rig.guest.datagrams[1].from_port != first);
}

TEST_CASE("UDP: traffic in either direction keeps a client alive") {
    Rig rig;
    rig.bind();
    Forwarder   fw(rig.stack, {udp_publish(5000)});
    Sock        a  = udp_client();
    sockaddr_in to = loopback(fw.addresses()[0].host_port);
    REQUIRE(::sendto(a.fd, "x", 1, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 1);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.datagrams.size() == 1; }));
    uint16_t port = rig.guest.datagrams[0].from_port;
    // The handler stamps with the time of the latest poll.
    rig.now = 50 * kSec;
    rig.poll(fw);
    rig.guest.send_udp(5000, port, bytes("hi"));
    rig.pump();
    rig.now = 100 * kSec;
    rig.poll(fw);
    CHECK(fw.udp_clients() == 1);
    rig.now = 110 * kSec;
    rig.poll(fw);
    CHECK(fw.udp_clients() == 0);
}

TEST_CASE("reset aborts flows, closes connections and forgets clients; listeners stay") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder   fw(rig.stack, {tcp_publish(80), udp_publish(5000)});
    uint16_t    tcp_port = fw.addresses()[0].host_port;
    Sock        client   = tcp_client(tcp_port);
    Sock        u        = udp_client();
    sockaddr_in to       = loopback(fw.addresses()[1].host_port);
    REQUIRE(::sendto(u.fd, "x", 1, 0, reinterpret_cast<sockaddr *>(&to), sizeof to) == 1);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() && rig.guest.datagrams.size() == 1; }));
    CHECK(fw.connections() == 1);
    CHECK(fw.udp_clients() == 1);

    fw.reset();
    rig.pump();
    CHECK(fw.connections() == 0);
    CHECK(fw.udp_clients() == 0);
    // The firmware saw the reset, the host the close.
    CHECK(rig.guest.connection()->got_rst);
    Received r;
    CHECK(rig.settle(fw, [&] {
        r = drain(client.fd);
        return r.eof || r.error != 0;
    }));

    // The stack goes down with it, comes up again, and the same publish works.
    rig.stack.reset();
    rig.guest.connections.clear();
    rig.bind();
    Sock again = tcp_client(tcp_port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    CHECK(fw.connections() == 1);
}

TEST_CASE("close also closes the listeners and may be repeated") {
    Rig rig;
    rig.guest.listening.insert(80);
    rig.bind();
    Forwarder fw(rig.stack, {tcp_publish(80)});
    uint16_t  port   = fw.addresses()[0].host_port;
    Sock      client = tcp_client(port);
    REQUIRE(rig.settle(fw, [&] { return rig.guest.connection() != nullptr; }));
    fw.close();
    rig.pump();
    CHECK(rig.guest.connection()->got_rst);
    CHECK(fw.empty());
    CHECK(fw.connections() == 0);
    fw.close();
    fw.poll(0);

    Sock        c(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in sa = loopback(port);
    CHECK(::connect(c.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == -1);
    CHECK(errno == ECONNREFUSED);
}

TEST_CASE("destroying the forwarder frees its ports") {
    Rig      rig;
    uint16_t port;
    {
        Forwarder fw(rig.stack, {tcp_publish(80)});
        port = fw.addresses()[0].host_port;
    }
    Sock        c(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in sa = loopback(port);
    CHECK(::connect(c.fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == -1);
}
