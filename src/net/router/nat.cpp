#include "net/router/nat.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "net/tcp_flow.h"

namespace bk {

namespace {

using namespace packets;

// Datagrams taken per poll and session, so a flood cannot hold the emulator
// in one call.
constexpr int kMaxDatagrams = 64;
// Reads per poll and TCP session, for the same reason.
constexpr int kMaxReads = 16;
// Apart from one another, so a late segment of an old flow does not fit a new one.
constexpr uint32_t kIssStep = 100'000;
// Bytes already sent to a host socket kept at most before they are dropped
// from the front of the pending buffer.
constexpr size_t kCompactBytes = 65536;
// The most a datagram towards the firmware may carry: a 1500-byte MTU less
// the IPv4 and UDP headers. Nothing is fragmented.
constexpr size_t kMaxPayload = 1500 - 20 - 8;
// warn_once keys.
constexpr uint64_t kWarnNoResolver  = 0;
constexpr uint64_t kWarnDnsRefused  = 1;
constexpr uint64_t kWarnDnsNoAnswer = 2;
constexpr uint64_t kWarnNoPing      = 3;

std::string dotted(uint32_t ip) {
    return std::format("{}.{}.{}.{}", ip >> 24, ip >> 16 & 0xff, ip >> 8 & 0xff, ip & 0xff);
}

} // namespace

// One TCP connection of the firmware: the host socket, and the flow towards
// the firmware, which speaks from the destination address and port.
struct Nat::TcpSession : TcpLink {
    Nat     &nat;
    TcpKey   key;
    uint32_t guest   = 0;
    int      fd      = -1;
    uint64_t started = 0;
    // The host connect is under way; the firmware's SYN waits for it.
    bool                     connecting = true;
    Tcp                      syn{};
    std::unique_ptr<TcpFlow> flow;
    std::vector<uint8_t>     to_host; // from the firmware, not yet sent
    size_t                   sent      = 0;
    bool                     host_eof  = false;
    bool                     shut_down = false;
    bool                     reset     = false; // the flow ended with a reset

    TcpSession(Nat &nat, const TcpKey &key, uint32_t guest) : nat(nat), key(key), guest(guest) {}

    uint32_t    remote() const { return std::get<1>(key); }
    uint16_t    remote_port() const { return std::get<2>(key); }
    std::string name() const {
        return std::format("{}:{} from guest port {}", dotted(remote()), remote_port(), std::get<0>(key));
    }

    void send_segment(const Tcp &segment) override {
        nat.to_guest_(remote(), kProtoTcp, build_tcp(remote(), guest, segment));
    }
    uint64_t now_ns() const override { return nat.now_; }
};

Nat::Nat(ToGuest to_guest, uint32_t gateway, std::optional<uint32_t> dns_upstream, Logger &log, uint16_t dns_port)
    : to_guest_(std::move(to_guest)), gateway_(gateway), dns_upstream_(dns_upstream), log_(log), dns_port_(dns_port) {}

Nat::~Nat() { reset(); }

void Nat::reset() {
    for (auto &[key, s] : udp_) ::close(s.fd);
    udp_.clear();
    for (auto &[key, s] : tcp_) {
        s->flow->discard();
        ::close(s->fd);
    }
    tcp_.clear();
    for (auto &[key, s] : icmp_) ::close(s.fd);
    icmp_.clear();
}

void Nat::udp_out(
    uint32_t guest, uint16_t guest_port, uint32_t dst, uint16_t dst_port, std::span<const uint8_t> data, uint64_t now) {
    send_udp({guest_port, dst, dst_port}, guest, dst, dst_port, data, now);
}

void Nat::dns_query(uint32_t guest, uint16_t guest_port, std::span<const uint8_t> query, uint64_t now) {
    if (!dns_upstream_) {
        log_.warn_once(kWarnNoResolver, "no DNS server: no IPv4 nameserver in /etc/resolv.conf");
        return;
    }
    send_udp({guest_port, gateway_, kDnsPort}, guest, *dns_upstream_, dns_port_, query, now);
}

void Nat::tcp_out(uint32_t guest, uint32_t dst, const Tcp &segment, uint64_t now) {
    now_ = now;
    const TcpKey key{segment.src_port, dst, segment.dst_port};
    auto         it = tcp_.find(key);
    if (it == tcp_.end()) {
        if ((segment.flags & (kTcpSyn | kTcpAck | kTcpRst)) == kTcpSyn)
            open_tcp(key, guest, segment);
        else
            refuse(guest, dst, segment);
        return;
    }
    TcpSession &s = *it->second;
    if (s.connecting) {
        // Repeated SYNs wait for the host like the first; an RST gives up.
        if (segment.flags & kTcpRst) {
            log_.log(Level::Debug, std::format("TCP to {} given up by the firmware", s.name()));
            s.reset = true;
            settle(it, false);
        }
        return;
    }
    s.flow->receive(segment);
    settle(it, serve(s, false));
}

void Nat::refuse(uint32_t guest, uint32_t dst, const Tcp &segment) {
    if (segment.flags & kTcpRst) return;
    log_.log(
        Level::Debug,
        std::format(
            "TCP to {}:{} from guest port {}: no connection, reset", dotted(dst), segment.dst_port, segment.src_port));
    Tcp rst{segment.dst_port, segment.src_port, 0, 0, kTcpRst, 0, std::nullopt, {}};
    if (segment.flags & kTcpAck) {
        rst.seq = segment.ack;
    } else {
        rst.flags |= kTcpAck;
        rst.ack = segment.seq + uint32_t(segment.payload.size()) + ((segment.flags & kTcpSyn) ? 1 : 0) +
                  ((segment.flags & kTcpFin) ? 1 : 0);
    }
    to_guest_(dst, kProtoTcp, build_tcp(dst, guest, rst));
}

void Nat::open_tcp(const TcpKey &key, uint32_t guest, const Tcp &syn) {
    const auto [guest_port, dst, dst_port] = key;
    tell(dst, dst_port, "tcp");
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_.log(Level::Debug, std::format("TCP to {}:{}: {}", dotted(dst), dst_port, std::strerror(errno)));
        refuse(guest, dst, syn);
        return;
    }
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(dst_port);
    sa.sin_addr.s_addr = htonl(dst);
    const int rc       = ::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa);
    if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
        log_.log(Level::Debug, std::format("TCP to {}:{}: {}", dotted(dst), dst_port, std::strerror(errno)));
        ::close(fd);
        refuse(guest, dst, syn);
        return;
    }

    auto s         = std::make_unique<TcpSession>(*this, key, guest);
    s->fd          = fd;
    s->started     = now_;
    s->syn         = syn;
    s->syn.payload = {};
    // The callbacks point at the session, which lives on the heap until it is removed.
    TcpSession *self = s.get();
    s->flow          = std::make_unique<TcpFlow>(
        *s,
        dst_port,
        guest_port,
        next_iss_,
        [self](std::span<const uint8_t> data) { self->to_host.insert(self->to_host.end(), data.begin(), data.end()); },
        [] {},
        [self](bool reset) { self->reset = reset; });
    next_iss_ += kIssStep;
    log_.log(Level::Debug, std::format("TCP to {}: connecting", s->name()));
    TcpSession &session = *tcp_.emplace(key, std::move(s)).first->second;
    if (rc == 0) connected(session);
}

void Nat::connected(TcpSession &s) {
    log_.log(Level::Debug, std::format("TCP to {}: connected", s.name()));
    s.connecting = false;
    s.flow->accept(s.syn);
}

bool Nat::serve(TcpSession &s, bool read) {
    if (s.reset) {
        log_.log(Level::Debug, std::format("TCP to {} reset by the firmware", s.name()));
        return false;
    }
    // Reading stops while the flow is full, so a fast host waits for a slow firmware.
    for (int i = 0; read && i < kMaxReads && !s.host_eof && s.flow->can_write(); ++i) {
        // Static so the poll does not put the buffer on the stack.
        static thread_local std::vector<uint8_t> buf(kTcpReadSize);
        ssize_t                                  n = ::recv(s.fd, buf.data(), buf.size(), 0);
        if (n > 0) {
            s.flow->write({buf.data(), size_t(n)});
        } else if (n == 0) {
            s.host_eof = true;
            s.flow->close();
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            log_.log(Level::Debug, std::format("TCP to {}: {}", s.name(), std::strerror(errno)));
            s.flow->abort();
            return false;
        }
    }

    while (s.sent < s.to_host.size()) {
        ssize_t n = ::send(s.fd, s.to_host.data() + s.sent, s.to_host.size() - s.sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            s.sent += size_t(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            log_.log(Level::Debug, std::format("TCP to {}: {}", s.name(), std::strerror(n < 0 ? errno : EPIPE)));
            s.flow->abort();
            return false;
        }
    }
    if (s.sent == s.to_host.size()) {
        s.to_host.clear();
        s.sent = 0;
    } else if (s.sent >= kCompactBytes) {
        // A long upload may never drain completely; drop what is sent.
        s.to_host.erase(s.to_host.begin(), s.to_host.begin() + ptrdiff_t(s.sent));
        s.sent = 0;
    }

    const bool drained = s.to_host.empty();
    if (s.flow->peer_closed() && drained && !s.shut_down) {
        ::shutdown(s.fd, SHUT_WR);
        s.shut_down = true;
    }
    return !(s.reset || (s.flow->finished() && drained));
}

Nat::TcpMap::iterator Nat::settle(TcpMap::iterator it, bool keep) {
    if (keep) return std::next(it);
    close_tcp(*it->second);
    return tcp_.erase(it);
}

void Nat::close_tcp(TcpSession &s) {
    log_.log(Level::Debug, std::format("TCP to {} closed", s.name()));
    s.flow->discard();
    // A reset from the firmware reaches the host as one.
    if (s.reset) {
        linger hard{1, 0};
        ::setsockopt(s.fd, SOL_SOCKET, SO_LINGER, &hard, sizeof hard);
    }
    ::close(s.fd);
}

size_t Nat::tcp_buffered() const {
    size_t n = 0;
    for (const auto &[key, s] : tcp_) n += s->flow->buffered();
    return n;
}

size_t Nat::tcp_held() const {
    size_t n = 0;
    for (const auto &[key, s] : tcp_) n += s->to_host.size();
    return n;
}

void Nat::make_room() {
    if (udp_.size() < kMaxUdpSessions) return;
    auto oldest = udp_.begin();
    for (auto it = udp_.begin(); it != udp_.end(); ++it)
        if (it->second.last_seen < oldest->second.last_seen) oldest = it;
    log_.log(Level::Debug,
             std::format("too many UDP sessions; closing the one from guest port {} to {}:{}",
                         std::get<0>(oldest->first),
                         dotted(std::get<1>(oldest->first)),
                         std::get<2>(oldest->first)));
    ::close(oldest->second.fd);
    udp_.erase(oldest);
}

void Nat::tell(uint32_t remote, uint16_t port, const char *protocol) {
    if (!seen_.insert(remote).second) return;
    if (port == 0)
        log_.log(Level::Info, std::format("firmware reaches {} ({})", dotted(remote), protocol));
    else
        log_.log(Level::Info, std::format("firmware connects to {}:{} ({})", dotted(remote), port, protocol));
}

void Nat::dns_refused() {
    log_.warn_once(
        kWarnDnsRefused,
        std::format("DNS server {}:{} refuses queries; DNS queries are dropped", dotted(*dns_upstream_), dns_port_));
}

void Nat::send_udp(
    const UdpKey &key, uint32_t guest, uint32_t host, uint16_t host_port, std::span<const uint8_t> data, uint64_t now) {
    auto it = udp_.find(key);
    if (it == udp_.end()) {
        make_room();
        int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            log_.log(Level::Debug,
                     std::format("UDP to {}:{} dropped: {}", dotted(host), host_port, std::strerror(errno)));
            return;
        }
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(host_port);
        sa.sin_addr.s_addr = htonl(host);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) {
            log_.log(Level::Debug,
                     std::format("UDP to {}:{} dropped: {}", dotted(host), host_port, std::strerror(errno)));
            ::close(fd);
            return;
        }
        if (!is_dns(key)) {
            tell(host, host_port, "udp");
        } else if (!dns_told_) {
            dns_told_ = true;
            log_.log(Level::Info, std::format("DNS relayed to {}:{}", dotted(host), host_port));
        }
        log_.log(Level::Debug,
                 std::format("UDP session from guest port {} to {}:{}", std::get<0>(key), dotted(host), host_port));
        it = udp_.emplace(key, UdpSession{fd, guest, now}).first;
    }
    it->second.guest     = guest;
    it->second.last_seen = now;
    if (::send(it->second.fd, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
        // A refusal of an earlier datagram may surface here rather than on receive.
        if (errno == ECONNREFUSED && is_dns(key)) dns_refused();
        log_.log(Level::Debug, std::format("UDP to {}:{}: {}", dotted(host), host_port, std::strerror(errno)));
    }
}

void Nat::icmp_out(uint32_t guest, uint32_t dst, const IcmpEcho &echo, uint64_t now) {
    now_ = now;
    if (echo.reply) return;
    const IcmpKey key{dst, echo.id};
    auto          it = icmp_.find(key);
    if (it == icmp_.end()) {
        if (icmp_.size() >= kMaxIcmpSessions) {
            auto oldest = icmp_.begin();
            for (auto i = icmp_.begin(); i != icmp_.end(); ++i)
                if (i->second.last_seen < oldest->second.last_seen) oldest = i;
            ::close(oldest->second.fd);
            icmp_.erase(oldest);
        }
        int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM)
                log_.warn_once(kWarnNoPing,
                               "ping to the internet needs an unprivileged ICMP socket: put your group in "
                               "net.ipv4.ping_group_range (sysctl)");
            else
                log_.log(Level::Debug, std::format("ping to {} dropped: {}", dotted(dst), std::strerror(errno)));
            return;
        }
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(dst);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) {
            log_.log(Level::Debug, std::format("ping to {} dropped: {}", dotted(dst), std::strerror(errno)));
            ::close(fd);
            return;
        }
        tell(dst, 0, "icmp");
        it = icmp_.emplace(key, IcmpSession{fd, guest, now}).first;
    }
    it->second.guest     = guest;
    it->second.last_seen = now;
    // The kernel replaces the identifier and the checksum.
    Bytes message = build_icmp_echo({false, echo.id, echo.seq, echo.data});
    if (::send(it->second.fd, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
        log_.log(Level::Debug, std::format("ping to {}: {}", dotted(dst), std::strerror(errno)));
}

void Nat::receive_icmp(const IcmpKey &key, IcmpSession &s, uint64_t now) {
    static thread_local std::vector<uint8_t> buf(65536);
    for (int i = 0; i < kMaxDatagrams; ++i) {
        ssize_t n = ::recv(s.fd, buf.data(), buf.size(), 0);
        if (n < 0) return; // nothing more, or an error that ends this poll
        s.last_seen = now;
        auto echo   = parse_icmp_echo({buf.data(), size_t(n)});
        if (!echo || !echo->reply || size_t(n) > kMaxPayload) continue;
        if (!to_guest_(key.first, kProtoIcmp, build_icmp_echo({true, key.second, echo->seq, echo->data})))
            log_.log(Level::Debug,
                     std::format("ping reply from {} dropped: the firmware has no address", dotted(key.first)));
    }
}

void Nat::receive(const UdpKey &key, UdpSession &s, uint64_t now) {
    const auto [guest_port, src, src_port] = key;
    // Largest UDP payload; static so the poll does not put 64 KiB on the stack.
    static thread_local std::vector<uint8_t> buf(65536);
    for (int i = 0; i < kMaxDatagrams; ++i) {
        ssize_t n = ::recv(s.fd, buf.data(), buf.size(), 0);
        if (n < 0) {
            // A refusal (an ICMP error for an earlier datagram) leaves the socket usable.
            if (errno == ECONNREFUSED && is_dns(key)) dns_refused();
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                log_.log(Level::Debug, std::format("UDP from {}:{}: {}", dotted(src), src_port, std::strerror(errno)));
            return;
        }
        s.last_seen = now;
        s.answered  = true;
        if (size_t(n) > kMaxPayload) {
            log_.log(Level::Debug,
                     std::format("UDP from {}:{} dropped: {} bytes do not fit the link", dotted(src), src_port, n));
            continue;
        }
        Bytes udp = build_udp(src, s.guest, src_port, guest_port, {buf.data(), size_t(n)});
        if (!to_guest_(src, kProtoUdp, udp))
            log_.log(Level::Debug,
                     std::format("UDP from {}:{} dropped: the firmware has no address", dotted(src), src_port));
    }
}

void Nat::poll(uint64_t now) {
    now_ = now;
    for (auto &[key, s] : udp_) receive(key, s, now);
    for (auto &[key, s] : icmp_) receive_icmp(key, s, now);
    for (auto it = tcp_.begin(); it != tcp_.end();) {
        TcpSession &s = *it->second;
        if (s.connecting) {
            pollfd p{s.fd, POLLOUT, 0};
            if (::poll(&p, 1, 0) <= 0) {
                ++it;
                continue;
            }
            int       err = 0;
            socklen_t len = sizeof err;
            if (::getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
            if (err != 0) {
                log_.log(Level::Debug, std::format("TCP to {}: {}", s.name(), std::strerror(err)));
                refuse(s.guest, s.remote(), s.syn);
                it = settle(it, false);
                continue;
            }
            connected(s);
        }
        it = settle(it, serve(s, true));
    }
}

void Nat::tick(uint64_t now) {
    now_ = now;
    for (auto it = tcp_.begin(); it != tcp_.end();) {
        TcpSession &s = *it->second;
        if (s.connecting) {
            if (now - s.started < kConnectTimeoutNs) {
                ++it;
                continue;
            }
            log_.log(Level::Debug, std::format("TCP to {}: no answer", s.name()));
            refuse(s.guest, s.remote(), s.syn);
            it = settle(it, false);
            continue;
        }
        s.flow->tick();
        it = settle(it, !(s.reset || (s.flow->finished() && s.to_host.empty())));
    }
    for (auto it = udp_.begin(); it != udp_.end();) {
        if (now - it->second.last_seen >= kUdpIdleNs) {
            log_.log(Level::Debug,
                     std::format("UDP session from guest port {} to {}:{} idle, closed",
                                 std::get<0>(it->first),
                                 dotted(std::get<1>(it->first)),
                                 std::get<2>(it->first)));
            if (is_dns(it->first) && !it->second.answered)
                log_.warn_once(kWarnDnsNoAnswer,
                               std::format("DNS server {}:{} does not answer; DNS queries are dropped",
                                           dotted(*dns_upstream_),
                                           dns_port_));
            ::close(it->second.fd);
            it = udp_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = icmp_.begin(); it != icmp_.end();) {
        if (now - it->second.last_seen >= kIcmpIdleNs) {
            ::close(it->second.fd);
            it = icmp_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<uint64_t> Nat::next_event_ns() const {
    std::optional<uint64_t> next;
    for (const auto &[key, s] : udp_)
        if (!next || s.last_seen + kUdpIdleNs < *next) next = s.last_seen + kUdpIdleNs;
    for (const auto &[key, s] : icmp_)
        if (!next || s.last_seen + kIcmpIdleNs < *next) next = s.last_seen + kIcmpIdleNs;
    for (const auto &[key, s] : tcp_) {
        auto d = s->connecting ? std::optional(s->started + kConnectTimeoutNs) : s->flow->deadline();
        if (d && (!next || *d < *next)) next = d;
    }
    return next;
}

} // namespace bk
