#include <doctest/doctest.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
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
#include "net/router/nat.h"
#include "net/router/resolver.h"
#include "net/tcp_flow.h"

using namespace bk;
using namespace bk::packets;

namespace {

constexpr uint32_t kGateway = ip4(10, 0, 2, 2);
constexpr uint32_t kGuest   = ip4(10, 0, 2, 15);
constexpr uint32_t kLocal   = ip4(127, 0, 0, 1);

constexpr uint64_t kSec = 1'000'000'000;

Bytes bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }

// A UDP socket on 127.0.0.1 playing the remote host or the DNS server.
struct Remote {
    int         fd   = -1;
    uint16_t    port = 0;
    sockaddr_in last{}; // where the latest datagram came from

    Remote() {
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
    ~Remote() { ::close(fd); }

    // The next datagram, waiting up to a second for it.
    std::optional<Bytes> receive() {
        for (int i = 0; i < 1000; ++i) {
            uint8_t   buf[2048];
            socklen_t len = sizeof last;
            ssize_t   n   = ::recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&last), &len);
            if (n >= 0) return Bytes(buf, buf + n);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    }

    void answer(const Bytes &data) {
        REQUIRE(::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr *>(&last), sizeof last) ==
                ssize_t(data.size()));
    }
};

// What the NAT handed to the router.
struct Packet {
    uint32_t src;
    uint8_t  protocol;
    Bytes    payload;
};

struct Fixture {
    std::ostringstream  log;
    bool                logging = (Logger::reset_for_tests(), Logger::configure(log, false), true);
    std::vector<Packet> sent;
    Nat                 nat;

    explicit Fixture(std::optional<uint32_t> upstream = std::nullopt, uint16_t dns_port = Nat::kDnsPort)
        : nat(
              [this](uint32_t src, uint8_t protocol, std::span<const uint8_t> payload) {
                  sent.push_back({src, protocol, Bytes(payload.begin(), payload.end())});
                  return true;
              },
              kGateway,
              upstream,
              Logger::get("net.nat"),
              dns_port) {}
    ~Fixture() { Logger::reset_for_tests(); }

    // Polls until something reaches the firmware, for up to a second.
    bool poll_until_sent(uint64_t now) {
        for (int i = 0; i < 1000; ++i) {
            nat.poll(now);
            if (!sent.empty()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    size_t count(const std::string &text) const {
        size_t n = 0;
        for (size_t at = log.str().find(text); at != std::string::npos; at = log.str().find(text, at + 1)) ++n;
        return n;
    }
};

// A listening TCP socket on 127.0.0.1 playing the remote host.
struct Server {
    int      fd   = -1;
    uint16_t port = 0;

    // `receive_buffer`, when set, is the accepted sockets' SO_RCVBUF.
    explicit Server(int backlog = 8, int receive_buffer = 0) {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        REQUIRE(fd >= 0);
        if (receive_buffer)
            REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer) == 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
        REQUIRE(::listen(fd, backlog) == 0);
        socklen_t len = sizeof sa;
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
        port = ntohs(sa.sin_port);
    }
    ~Server() { ::close(fd); }

    // The next connection, non-blocking, waiting up to a second for it.
    int accept_one() {
        for (int i = 0; i < 1000; ++i) {
            int c = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK);
            if (c >= 0) return c;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL("no connection");
        return -1;
    }
};

// Everything that arrives on a host socket until `size` bytes or EOF, for up
// to a second; the NAT is polled meanwhile so it keeps moving bytes.
struct Read {
    Bytes data;
    bool  eof = false;
};
template <class Poll> Read read_host(int fd, size_t size, Poll poll) {
    Read r;
    for (int i = 0; i < 1000 && r.data.size() < size && !r.eof; ++i) {
        poll();
        uint8_t buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n > 0)
            r.data.insert(r.data.end(), buf, buf + n);
        else if (n == 0)
            r.eof = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return r;
}

// The firmware's end of TCP connections through the NAT.
struct Guest {
    Fixture &f;
    uint32_t dst;
    uint16_t dst_port;
    uint16_t port;
    uint32_t seq = 1000; // the next byte the firmware sends
    uint32_t ack = 0;    // the next byte it expects from the NAT

    Guest(Fixture &f, uint32_t dst, uint16_t dst_port, uint16_t port = 5000)
        : f(f), dst(dst), dst_port(dst_port), port(port) {}

    void segment(uint8_t flags, const Bytes &data = {}, uint64_t now = 0, uint16_t window = 16384) {
        std::optional<uint16_t> mss;
        if (flags & kTcpSyn) mss = 1460;
        Bytes wire = build_tcp(kGuest, dst, Tcp{port, dst_port, seq, ack, flags, window, mss, data});
        auto  tcp  = parse_tcp(kGuest, dst, wire);
        REQUIRE(tcp);
        f.nat.tcp_out(kGuest, dst, *tcp, now);
        seq += uint32_t(data.size()) + ((flags & (kTcpSyn | kTcpFin)) ? 1 : 0);
    }

    // The segments the NAT sent to the firmware so far, taken out of `f.sent`.
    struct Seg {
        uint8_t  flags;
        uint32_t seq;
        uint32_t ack;
        bool     mss;
        Bytes    payload;
    };
    std::vector<Seg> take() {
        std::vector<Seg> out;
        for (const Packet &p : f.sent) {
            CHECK(p.src == dst);
            CHECK(p.protocol == kProtoTcp);
            auto tcp = parse_tcp(dst, kGuest, p.payload);
            REQUIRE(tcp);
            CHECK(tcp->src_port == dst_port);
            CHECK(tcp->dst_port == port);
            out.push_back({tcp->flags, tcp->seq, tcp->ack, tcp->mss.has_value(), sub(tcp->payload)});
        }
        f.sent.clear();
        return out;
    }

    // Polls until the NAT sends something, then takes it.
    std::vector<Seg> wait(uint64_t now = 0) {
        f.poll_until_sent(now);
        return take();
    }

    static Bytes sub(std::span<const uint8_t> s) { return Bytes(s.begin(), s.end()); }

    // SYN, the NAT's SYN|ACK once the host accepted, our ACK; returns the host's end.
    int open(Server &server) {
        segment(kTcpSyn);
        auto syn_ack = wait();
        REQUIRE(syn_ack.size() == 1);
        CHECK(syn_ack[0].flags == (kTcpSyn | kTcpAck));
        CHECK(syn_ack[0].ack == seq);
        CHECK(syn_ack[0].mss);
        ack = syn_ack[0].seq + 1;
        segment(kTcpAck);
        CHECK(take().empty());
        return server.accept_one();
    }
};

// A port of 127.0.0.1 nobody listens on.
uint16_t closed_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
    socklen_t len = sizeof sa;
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
    ::close(fd);
    return ntohs(sa.sin_port);
}

// A temporary resolv.conf, removed on scope exit.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string &text) {
        char name[] = "/tmp/bk_resolv_XXXXXX";
        int  fd     = ::mkstemp(name);
        REQUIRE(fd >= 0);
        ::close(fd);
        path = name;
        std::ofstream(path) << text;
    }
    ~TempFile() { std::filesystem::remove(path); }
};

} // namespace

TEST_CASE("nat: UDP goes out through a host socket and the answer comes back from the destination") {
    Fixture f;
    Remote  remote;
    f.nat.udp_out(kGuest, 5000, kLocal, remote.port, bytes("ping"), 0);
    CHECK(f.nat.udp_sessions() == 1);
    REQUIRE(remote.receive() == bytes("ping"));
    const uint16_t host_port = ntohs(remote.last.sin_port);

    remote.answer(bytes("pong"));
    REQUIRE(f.poll_until_sent(kSec));
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0].src == kLocal);
    CHECK(f.sent[0].protocol == kProtoUdp);
    auto udp = parse_udp(kLocal, kGuest, f.sent[0].payload);
    REQUIRE(udp);
    CHECK(udp->src_port == remote.port);
    CHECK(udp->dst_port == 5000);
    CHECK(Bytes(udp->payload.begin(), udp->payload.end()) == bytes("pong"));

    // The same guest port and destination reuse the socket; another guest port
    // gets one of its own.
    f.nat.udp_out(kGuest, 5000, kLocal, remote.port, bytes("again"), 2 * kSec);
    REQUIRE(remote.receive() == bytes("again"));
    CHECK(ntohs(remote.last.sin_port) == host_port);
    CHECK(f.nat.udp_sessions() == 1);
    f.nat.udp_out(kGuest, 5001, kLocal, remote.port, bytes("other"), 2 * kSec);
    REQUIRE(remote.receive() == bytes("other"));
    CHECK(ntohs(remote.last.sin_port) != host_port);
    CHECK(f.nat.udp_sessions() == 2);

    // The first connection to an address is told once.
    CHECK(f.count(std::format("INFO emulator.net.nat: firmware connects to 127.0.0.1:{} (udp)", remote.port)) == 1);
    CHECK(f.count("firmware connects to") == 1);
}

TEST_CASE("nat: a ping goes out on an ICMP socket and the reply keeps the firmware's id") {
    Fixture f;
    // Needs net.ipv4.ping_group_range to hold the test's group; where it does
    // not, the NAT warns once and the firmware gets no answer.
    int probe = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (probe >= 0) ::close(probe);

    const Bytes data = {1, 2, 3, 4};
    f.nat.icmp_out(kGuest, kLocal, {false, 0x1234, 7, data}, 0);
    if (probe < 0) {
        CHECK(f.nat.icmp_sessions() == 0);
        CHECK(f.count("ping_group_range") == 1);
        f.nat.icmp_out(kGuest, kLocal, {false, 0x1234, 8, data}, 0);
        CHECK(f.count("ping_group_range") == 1);
        return;
    }
    REQUIRE(f.poll_until_sent(0));
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0].src == kLocal);
    CHECK(f.sent[0].protocol == kProtoIcmp);
    auto echo = parse_icmp_echo(f.sent[0].payload);
    REQUIRE(echo);
    CHECK(echo->reply);
    CHECK(echo->id == 0x1234);
    CHECK(echo->seq == 7);
    CHECK(Bytes(echo->data.begin(), echo->data.end()) == data);
    CHECK(f.nat.icmp_sessions() == 1);
    CHECK_FALSE(f.nat.empty());
    CHECK(f.nat.next_event_ns() == Nat::kIcmpIdleNs);

    // A second request of the run reuses the socket; an idle one is closed.
    f.nat.icmp_out(kGuest, kLocal, {false, 0x1234, 8, data}, kSec);
    CHECK(f.nat.icmp_sessions() == 1);
    f.nat.tick(kSec + Nat::kIcmpIdleNs);
    CHECK(f.nat.icmp_sessions() == 0);
    CHECK(f.nat.empty());
}

TEST_CASE("nat: TCP and UDP to one address are told once in all") {
    Fixture f;
    Remote  remote;
    f.nat.udp_out(kGuest, 5000, kLocal, remote.port, bytes("ping"), 0);
    Guest g(f, kLocal, closed_port());
    g.segment(kTcpSyn);
    g.wait();
    CHECK(f.count("firmware connects to") == 1);
    CHECK(f.count(std::format("INFO emulator.net.nat: firmware connects to 127.0.0.1:{} (udp)", remote.port)) == 1);
}

TEST_CASE("nat: a DNS query is relayed and answered from the gateway") {
    Remote  upstream;
    Fixture f(kLocal, upstream.port);
    f.nat.dns_query(kGuest, 6000, bytes("query"), 0);
    REQUIRE(upstream.receive() == bytes("query"));
    CHECK(f.nat.udp_sessions() == 1);

    upstream.answer(bytes("answer"));
    REQUIRE(f.poll_until_sent(kSec));
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0].src == kGateway);
    CHECK(f.sent[0].protocol == kProtoUdp);
    auto udp = parse_udp(kGateway, kGuest, f.sent[0].payload);
    REQUIRE(udp);
    CHECK(udp->src_port == 53);
    CHECK(udp->dst_port == 6000);
    CHECK(Bytes(udp->payload.begin(), udp->payload.end()) == bytes("answer"));
    // The relay names its upstream once, not as a connection of the firmware.
    f.nat.dns_query(kGuest, 6001, bytes("query"), 0);
    REQUIRE(upstream.receive() == bytes("query"));
    CHECK(f.count(std::format("INFO emulator.net.nat: DNS relayed to 127.0.0.1:{}", upstream.port)) == 1);
    CHECK(f.count("firmware connects to") == 0);
    f.sent.clear();
    upstream.answer(bytes("answer"));
    REQUIRE(f.poll_until_sent(kSec));
    f.sent.clear();

    // Plain UDP from the same port to the upstream itself is a session apart,
    // answered from the upstream's address.
    f.nat.udp_out(kGuest, 6000, kLocal, upstream.port, bytes("direct"), kSec);
    REQUIRE(upstream.receive() == bytes("direct"));
    CHECK(f.nat.udp_sessions() == 3);
    f.sent.clear();
    upstream.answer(bytes("back"));
    REQUIRE(f.poll_until_sent(2 * kSec));
    CHECK(f.sent[0].src == kLocal);

    // Answered sessions idle out without a warning.
    f.nat.tick(2 * kSec + Nat::kUdpIdleNs);
    CHECK(f.nat.udp_sessions() == 0);
    CHECK(f.count("WARNING") == 0);
}

TEST_CASE("nat: idle UDP sessions are closed; reset closes all") {
    Remote  remote;
    Fixture f(kLocal, remote.port);
    CHECK_FALSE(f.nat.next_event_ns());
    f.nat.udp_out(kGuest, 5000, kLocal, remote.port, bytes("a"), 0);
    // A silent upstream: the query is not answered, its session just idles out.
    f.nat.dns_query(kGuest, 5001, bytes("q"), 10 * kSec);
    CHECK(f.nat.udp_sessions() == 2);
    CHECK(f.nat.next_event_ns() == Nat::kUdpIdleNs);

    f.nat.tick(Nat::kUdpIdleNs - 1);
    CHECK(f.nat.udp_sessions() == 2);
    // Traffic keeps a session alive.
    f.nat.udp_out(kGuest, 5000, kLocal, remote.port, bytes("b"), 30 * kSec);
    CHECK(f.nat.next_event_ns() == 10 * kSec + Nat::kUdpIdleNs);
    f.nat.tick(10 * kSec + Nat::kUdpIdleNs);
    CHECK(f.nat.udp_sessions() == 1);
    CHECK(f.nat.next_event_ns() == 30 * kSec + Nat::kUdpIdleNs);
    f.nat.tick(30 * kSec + Nat::kUdpIdleNs);
    CHECK(f.nat.udp_sessions() == 0);
    CHECK_FALSE(f.nat.next_event_ns());

    Remote other;
    f.nat.udp_out(kGuest, 5000, kLocal, other.port, bytes("c"), 100 * kSec);
    f.nat.dns_query(kGuest, 5002, bytes("d"), 100 * kSec);
    CHECK(f.nat.udp_sessions() == 2);
    REQUIRE(other.receive() == bytes("c"));
    f.nat.reset();
    CHECK(f.nat.udp_sessions() == 0);
    CHECK_FALSE(f.nat.next_event_ns());
    // Nothing of the old sessions reaches the firmware.
    other.answer(bytes("late"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.nat.poll(101 * kSec);
    CHECK(f.sent.empty());
}

TEST_CASE("nat: without a resolver DNS is dropped with one warning") {
    Fixture f;
    f.nat.dns_query(kGuest, 6000, bytes("query"), 0);
    f.nat.dns_query(kGuest, 6001, bytes("query"), kSec);
    f.nat.poll(2 * kSec);
    CHECK(f.sent.empty());
    CHECK(f.nat.udp_sessions() == 0);
    CHECK(f.count("WARNING emulator.net.nat: no DNS server: no IPv4 nameserver in /etc/resolv.conf") == 1);
}

TEST_CASE("nat: an upstream that refuses DNS is told with one warning") {
    const uint16_t port = closed_port();
    Fixture        f(kLocal, port);
    // Several queries, on one guest port and on others.
    for (int i = 0; i < 4; ++i) {
        f.nat.dns_query(kGuest, uint16_t(6000 + i % 2), bytes("query"), uint64_t(i) * kSec);
        for (int j = 0; j < 20; ++j) {
            f.nat.poll(uint64_t(i) * kSec);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(f.sent.empty());
    CHECK(f.count(std::format(
              "WARNING emulator.net.nat: DNS server 127.0.0.1:{} refuses queries; DNS queries are dropped", port)) ==
          1);
    CHECK(f.count("WARNING") == 1);
}

TEST_CASE("nat: a silent DNS upstream is told with one warning when its sessions idle out") {
    Remote  upstream; // bound, never answers
    Fixture f(kLocal, upstream.port);
    f.nat.dns_query(kGuest, 6000, bytes("q1"), 0);
    f.nat.dns_query(kGuest, 6001, bytes("q2"), kSec);
    f.nat.poll(kSec);
    CHECK(f.count("WARNING") == 0);
    f.nat.tick(Nat::kUdpIdleNs - 1);
    CHECK(f.count("WARNING") == 0);
    f.nat.tick(kSec + Nat::kUdpIdleNs);
    CHECK(f.nat.udp_sessions() == 0);
    CHECK(f.count(
              std::format("WARNING emulator.net.nat: DNS server 127.0.0.1:{} does not answer; DNS queries are dropped",
                          upstream.port)) == 1);
    CHECK(f.count("WARNING") == 1);
}

TEST_CASE("host_resolver reads the first IPv4 nameserver") {
    CHECK_FALSE(host_resolver("/nonexistent/resolv.conf"));
    {
        TempFile only6("nameserver ::1\nnameserver fe80::1%eth0\n");
        CHECK_FALSE(host_resolver(only6.path.string()));
    }
    {
        TempFile commented("# nameserver 1.1.1.1\n; nameserver 1.0.0.1\nsearch lan\nnameserver 9.9.9.9 # quad9\n");
        CHECK(host_resolver(commented.path.string()) == ip4(9, 9, 9, 9));
    }
    {
        TempFile two("nameserver 2001:db8::1\nnameserver bogus\n\tnameserver\t192.168.1.1\nnameserver 8.8.8.8\n");
        CHECK(host_resolver(two.path.string()) == ip4(192, 168, 1, 1));
    }
    {
        TempFile empty("");
        CHECK_FALSE(host_resolver(empty.path.string()));
    }
}

TEST_CASE("nat: a TCP connection opens only after the host accepts, and carries bytes both ways") {
    Fixture f;
    Server  server;
    Guest   g(f, kLocal, server.port);
    CHECK(f.nat.empty());
    int fd = g.open(server);
    CHECK(f.nat.tcp_sessions() == 1);
    CHECK_FALSE(f.nat.empty());

    // Firmware to host: delivered and acknowledged.
    g.segment(kTcpAck | kTcpPsh, bytes("hello"));
    auto acked = g.take();
    REQUIRE(acked.size() == 1);
    CHECK(acked[0].flags == kTcpAck);
    CHECK(acked[0].ack == g.seq);
    CHECK(read_host(fd, 5, [&] { f.nat.poll(0); }).data == bytes("hello"));

    // Host to firmware, from the remote address and port.
    REQUIRE(::send(fd, "world", 5, 0) == 5);
    auto data = g.wait();
    REQUIRE(data.size() == 1);
    CHECK((data[0].flags & kTcpAck));
    CHECK(data[0].seq == g.ack);
    CHECK(data[0].payload == bytes("world"));
    g.ack += 5;
    g.segment(kTcpAck);

    // A second connection to the same address is not told again.
    Guest other(f, kLocal, server.port, 5001);
    int   fd2 = other.open(server);
    CHECK(f.nat.tcp_sessions() == 2);
    CHECK(f.count(std::format("INFO emulator.net.nat: firmware connects to 127.0.0.1:{} (tcp)", server.port)) == 1);

    // Reset closes the host sockets without a word to the firmware.
    f.nat.reset();
    CHECK(f.nat.tcp_sessions() == 0);
    CHECK(f.nat.empty());
    CHECK_FALSE(f.nat.next_event_ns());
    CHECK(read_host(fd, 1, [] {}).eof);
    CHECK(read_host(fd2, 1, [] {}).eof);
    CHECK(f.sent.empty());
    ::close(fd);
    ::close(fd2);
}

TEST_CASE("nat: a refused connection is reset") {
    Fixture f;
    Guest   g(f, kLocal, closed_port());
    g.segment(kTcpSyn);
    auto rst = g.wait();
    REQUIRE(rst.size() == 1);
    CHECK(rst[0].flags == (kTcpRst | kTcpAck));
    CHECK(rst[0].ack == g.seq);
    CHECK(f.nat.tcp_sessions() == 0);
    CHECK(f.nat.empty());
}

TEST_CASE("nat: a connection that never completes is reset after the timeout") {
    // A silent peer on loopback: the backlog of 0 is taken by a connection
    // nobody accepts, so the kernel drops further SYNs and the NAT's connect
    // stays under way.
    Fixture f;
    Server  server(0);
    int     filler = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(filler >= 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(server.port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(filler, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);

    Guest g(f, kLocal, server.port);
    g.segment(kTcpSyn, {}, kSec);
    for (int i = 0; i < 50; ++i) {
        f.nat.poll(kSec);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // No SYN|ACK while the host is still connecting.
    REQUIRE(f.nat.tcp_sessions() == 1);
    CHECK(f.sent.empty());
    CHECK(f.nat.next_event_ns() == kSec + Nat::kConnectTimeoutNs);
    // The firmware's repeated SYN changes nothing.
    g.seq = 1000;
    g.segment(kTcpSyn, {}, 3 * kSec);
    f.nat.poll(3 * kSec);
    CHECK(f.sent.empty());
    CHECK(f.nat.tcp_sessions() == 1);
    CHECK(f.nat.next_event_ns() == kSec + Nat::kConnectTimeoutNs);

    SUBCASE("the host never answers: reset after the timeout") {
        f.nat.tick(kSec + Nat::kConnectTimeoutNs - 1);
        CHECK(f.nat.tcp_sessions() == 1);
        CHECK(f.sent.empty());
        f.nat.tick(kSec + Nat::kConnectTimeoutNs);
        CHECK(f.nat.tcp_sessions() == 0);
        CHECK(f.nat.empty());
        CHECK_FALSE(f.nat.next_event_ns());
        auto rst = g.take();
        REQUIRE(rst.size() == 1);
        CHECK(rst[0].flags == (kTcpRst | kTcpAck));
        CHECK(rst[0].ack == g.seq);
    }

    SUBCASE("the host answers late: the SYN|ACK follows") {
        // Room in the backlog; the kernel's own SYN retry gets through.
        ::close(server.accept_one());
        for (int i = 0; i < 5000 && f.sent.empty(); ++i) {
            f.nat.poll(4 * kSec);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto syn_ack = g.take();
        REQUIRE(syn_ack.size() == 1);
        CHECK(syn_ack[0].flags == (kTcpSyn | kTcpAck));
        CHECK(syn_ack[0].ack == g.seq);
        // The connect deadline is gone; the flow's own timer runs.
        CHECK(f.nat.next_event_ns() == 4 * kSec + TcpFlow::RTO_NS);
    }
    ::close(filler);
}

TEST_CASE("nat: half close from either side, and a host error aborts the flow") {
    Fixture f;
    Server  server;
    auto    poll = [&] { f.nat.poll(0); };

    SUBCASE("the firmware closes first") {
        Guest g(f, kLocal, server.port);
        int   fd = g.open(server);
        g.segment(kTcpFin | kTcpAck);
        auto acked = g.take();
        REQUIRE(acked.size() == 1);
        CHECK(acked[0].ack == g.seq);
        Read r = read_host(fd, 1, poll);
        CHECK(r.eof);
        CHECK(r.data.empty());

        // The host may still talk.
        REQUIRE(::send(fd, "late", 4, 0) == 4);
        auto data = g.wait();
        REQUIRE(data.size() == 1);
        CHECK(data[0].payload == bytes("late"));
        g.ack += 4;
        g.segment(kTcpAck);

        ::close(fd);
        auto fin = g.wait();
        REQUIRE(fin.size() == 1);
        CHECK(fin[0].flags == (kTcpFin | kTcpAck));
        CHECK(fin[0].seq == g.ack);
        g.ack += 1;
        g.segment(kTcpAck);
        CHECK(f.nat.tcp_sessions() == 0);
    }

    SUBCASE("the host closes first") {
        Guest g(f, kLocal, server.port);
        int   fd = g.open(server);
        REQUIRE(::shutdown(fd, SHUT_WR) == 0);
        auto fin = g.wait();
        REQUIRE(fin.size() == 1);
        CHECK(fin[0].flags == (kTcpFin | kTcpAck));
        g.ack += 1;

        // The firmware may still talk.
        g.segment(kTcpAck | kTcpPsh, bytes("more"));
        CHECK(read_host(fd, 4, poll).data == bytes("more"));
        g.take();
        g.segment(kTcpFin | kTcpAck);
        CHECK(f.nat.tcp_sessions() == 0);
        CHECK(read_host(fd, 1, poll).eof);
        ::close(fd);
    }

    SUBCASE("a host error") {
        Guest  g(f, kLocal, server.port);
        int    fd = g.open(server);
        linger hard{1, 0};
        REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_LINGER, &hard, sizeof hard) == 0);
        ::close(fd);
        auto rst = g.wait();
        REQUIRE(rst.size() == 1);
        CHECK(rst[0].flags == (kTcpRst | kTcpAck));
        CHECK(rst[0].seq == g.ack);
        CHECK(f.nat.tcp_sessions() == 0);
    }

    SUBCASE("the firmware resets") {
        Guest g(f, kLocal, server.port);
        int   fd = g.open(server);
        g.segment(kTcpRst);
        CHECK(f.nat.tcp_sessions() == 0);
        CHECK(f.sent.empty());
        // The host sees the end too.
        uint8_t buf[4];
        ssize_t n = -1;
        for (int i = 0; i < 1000; ++i) {
            n = ::recv(fd, buf, sizeof buf, 0);
            if (n >= 0 || errno != EAGAIN) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK((n == 0 || (n < 0 && errno == ECONNRESET)));
        ::close(fd);
    }
}

TEST_CASE("nat: a full flow stops reading the host socket") {
    Fixture f;
    Server  server;
    Guest   g(f, kLocal, server.port);
    int     fd = g.open(server);

    // The host pushes 1 MiB while the firmware acknowledges nothing.
    const Bytes chunk(65536, 0x5a);
    size_t      pushed = 0, most = 0;
    for (int i = 0; i < 300 && pushed < (1u << 20); ++i) {
        ssize_t n = ::send(fd, chunk.data(), chunk.size(), MSG_NOSIGNAL);
        if (n > 0) pushed += size_t(n);
        f.nat.poll(0);
        most = std::max(most, f.nat.tcp_buffered());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(most >= TcpFlow::SEND_BUFFER_LIMIT);
    CHECK(most <= TcpFlow::SEND_BUFFER_LIMIT + Nat::kTcpReadSize);
    // Only the firmware's window went out.
    size_t   delivered = 0;
    uint32_t end       = g.ack;
    for (const auto &s : g.take()) {
        delivered += s.payload.size();
        end = std::max(end, s.seq + uint32_t(s.payload.size()));
    }
    CHECK(delivered == 16384);

    // An acknowledgement makes room: the next window goes out.
    g.ack = end;
    g.segment(kTcpAck);
    CHECK(g.take().size() > 0);
    ::close(fd);
}

TEST_CASE("nat: a long upload to a slow host keeps only what is still to send") {
    Fixture f;
    Server  server(8, 4096);
    Guest   g(f, kLocal, server.port);
    int     fd = g.open(server);

    // The firmware sends twice as fast as the host reads, so the bytes for the
    // host never drain completely. What the NAT holds beyond the bytes not yet
    // read by the host is what it already sent: that part stays small.
    const Bytes chunk(2800, 0x5a);
    size_t      pushed = 0, read = 0, worst = 0;
    for (int i = 0; i < 6000; ++i) {
        g.segment(kTcpAck | kTcpPsh, chunk);
        pushed += chunk.size();
        f.sent.clear();
        uint8_t buf[1400];
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n > 0) read += size_t(n);
        f.nat.poll(0);
        const size_t unread = pushed - read;
        if (f.nat.tcp_held() > unread) worst = std::max(worst, f.nat.tcp_held() - unread);
    }
    CHECK(read > 1'000'000);
    CHECK(f.nat.tcp_held() > 0);
    CHECK(worst <= 65536);
    ::close(fd);
}

TEST_CASE("nat: a stray segment gets an RST") {
    Fixture f;
    Guest   g(f, kLocal, closed_port());

    g.ack = 777;
    g.segment(kTcpAck, bytes("x"));
    auto rst = g.take();
    REQUIRE(rst.size() == 1);
    CHECK(rst[0].flags == kTcpRst);
    CHECK(rst[0].seq == 777);

    const uint32_t seq = g.seq;
    g.segment(kTcpPsh, bytes("abc"));
    rst = g.take();
    REQUIRE(rst.size() == 1);
    CHECK(rst[0].flags == (kTcpRst | kTcpAck));
    CHECK(rst[0].ack == seq + 3);

    g.segment(kTcpSyn | kTcpAck);
    CHECK(g.take().size() == 1);
    // An RST is never answered.
    g.segment(kTcpRst);
    CHECK(g.take().empty());
    CHECK(f.nat.tcp_sessions() == 0);
}
