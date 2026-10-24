#include "net/forwarder.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bk {

namespace {

// Datagrams taken per poll and published port, so a flood cannot hold the
// emulator in one call.
constexpr int kMaxDatagrams = 256;

std::string errno_text(int e) { return std::strerror(e); }

std::string address_text(const sockaddr_in &sa) {
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof buf);
    return std::format("{}:{}", buf, ntohs(sa.sin_port));
}

std::optional<uint16_t> parse_port(const std::string &s) {
    if (s.empty() || s.size() > 5 || !std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    unsigned v = unsigned(std::stoul(s));
    if (v < 1 || v > 65535) return std::nullopt;
    return uint16_t(v);
}

} // namespace

Publish parse_publish(const std::string &text) {
    auto bad = [&] {
        return ForwardError(
            std::format("bad --publish '{}' (want [HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp], ports 1-65535)", text));
    };
    Publish     p;
    std::string body = text;
    if (auto slash = text.rfind('/'); slash != std::string::npos) {
        std::string proto = text.substr(slash + 1);
        if (proto == "udp")
            p.udp = true;
        else if (proto != "tcp")
            throw bad();
        body = text.substr(0, slash);
    }
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        size_t colon = body.find(':', start);
        parts.push_back(body.substr(start, colon == std::string::npos ? colon : colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    if (parts.size() != 2 && parts.size() != 3) throw bad();
    if (parts.size() == 3) {
        in_addr probe;
        if (::inet_pton(AF_INET, parts[0].c_str(), &probe) != 1) throw bad();
        p.host_ip = parts[0];
    }
    auto host  = parse_port(parts[parts.size() - 2]);
    auto guest = parse_port(parts.back());
    if (!host || !guest) throw bad();
    p.host_port  = *host;
    p.guest_port = *guest;
    return p;
}

std::string describe(const Publish &p) {
    return std::format("{} {}:{} -> guest port {}", p.udp ? "udp" : "tcp", p.host_ip, p.host_port, p.guest_port);
}

Forwarder::Forwarder(Stack &stack, const std::vector<Publish> &publishes)
    : stack_(stack), log_(Logger::get("net.forward")) {
    try {
        for (const Publish &p : publishes) {
            auto fail = [&](int e, const std::string &why) {
                return ForwardError(std::format("cannot publish {}:{} to guest port {}/{}: {}",
                                                p.host_ip,
                                                p.host_port,
                                                p.guest_port,
                                                p.udp ? "udp" : "tcp",
                                                why.empty() ? errno_text(e) : why));
            };
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(p.host_port);
            if (::inet_pton(AF_INET, p.host_ip.c_str(), &sa.sin_addr) != 1) throw fail(0, "not an IPv4 address");

            int fd = ::socket(AF_INET, (p.udp ? SOCK_DGRAM : SOCK_STREAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) throw fail(errno, "");
            // From here the socket belongs to the list, so a failure closes it.
            listeners_.push_back({fd, p});
            if (!p.udp) {
                int one = 1;
                ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            }
            if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) throw fail(errno, "");
            if (!p.udp && ::listen(fd, 16) != 0) throw fail(errno, "");
            socklen_t len = sizeof sa;
            if (::getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &len) != 0) throw fail(errno, "");
            Publish bound             = p;
            bound.host_port           = ntohs(sa.sin_port);
            listeners_.back().publish = bound;
            addresses_.push_back(bound);
        }
    } catch (...) {
        close();
        throw;
    }
}

Forwarder::~Forwarder() { close(); }

size_t Forwarder::udp_clients() const {
    size_t n = 0;
    for (const auto &[fd, clients] : clients_) n += clients.size();
    return n;
}

void Forwarder::drop(Connection &c) {
    if (c.flow) c.flow->abort();
    if (c.fd >= 0) ::close(c.fd);
    c.fd = -1;
}

void Forwarder::forget_clients(Listener &l) {
    auto it = clients_.find(l.fd);
    if (it == clients_.end()) return;
    for (auto &[key, client] : it->second) stack_.unbind_udp(client.local_port);
    clients_.erase(it);
}

void Forwarder::reset() {
    for (Connection &c : connections_) drop(c);
    connections_.clear();
    for (Listener &l : listeners_) forget_clients(l);
}

void Forwarder::close() {
    reset();
    for (Listener &l : listeners_) ::close(l.fd);
    listeners_.clear();
}

void Forwarder::accept_clients(const Listener &l, uint64_t now) {
    for (;;) {
        sockaddr_in sa{};
        socklen_t   len = sizeof sa;
        int         fd  = ::accept4(l.fd, reinterpret_cast<sockaddr *>(&sa), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            break;
        }
        Connection c;
        c.fd         = fd;
        c.peer       = address_text(sa);
        c.guest_port = l.publish.guest_port;
        c.accepted   = now;
        log_.log(Level::Debug, std::format("connection from {} to guest port {}", c.peer, c.guest_port));
        connections_.push_back(std::move(c));
    }
}

bool Forwarder::serve(Connection &c, uint64_t now) {
    if (!c.flow) {
        // The flow's callbacks point at the connection, which sits in a list
        // and so keeps its address until it is dropped.
        Connection *self = &c;
        c.flow           = stack_.open_tcp(
            c.guest_port,
            [self](std::span<const uint8_t> data) {
                self->to_host.insert(self->to_host.end(), data.begin(), data.end());
            },
            [] {},
            [self](bool reset) { self->reset = reset; },
            now);
        if (!c.flow) {
            if (now - c.accepted < WAIT_FOR_ADDRESS_NS) return true;
            log_.log(Level::Warning, std::format("connection from {} dropped: the firmware got no address", c.peer));
            return false;
        }
    }
    if (c.reset) {
        log_.log(Level::Debug, std::format("connection from {} reset by the firmware", c.peer));
        return false;
    }

    if (!c.host_eof && c.flow->can_write()) {
        uint8_t buf[READ_SIZE];
        ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        if (n > 0) {
            c.flow->write({buf, size_t(n)});
        } else if (n == 0) {
            c.host_eof = true;
            c.flow->close();
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            log_.log(Level::Debug, std::format("connection from {}: {}", c.peer, errno_text(errno)));
            return false;
        }
    }

    while (c.sent < c.to_host.size()) {
        ssize_t n = ::send(c.fd, c.to_host.data() + c.sent, c.to_host.size() - c.sent, MSG_NOSIGNAL);
        if (n > 0) {
            c.sent += size_t(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            log_.log(Level::Debug, std::format("connection from {}: {}", c.peer, errno_text(n < 0 ? errno : EPIPE)));
            return false;
        }
    }
    if (c.sent == c.to_host.size()) {
        c.to_host.clear();
        c.sent = 0;
    }

    bool drained = c.to_host.empty();
    if (c.flow->peer_closed() && drained && !c.shut_down) {
        ::shutdown(c.fd, SHUT_WR);
        c.shut_down = true;
    }
    if (c.reset || (c.flow->finished() && drained)) {
        log_.log(Level::Debug, std::format("connection from {} closed", c.peer));
        return false;
    }
    return true;
}

void Forwarder::serve_udp(Listener &l, uint64_t now) {
    auto &clients = clients_[l.fd];
    // Largest UDP payload; static so the poll does not put 64 KiB on the stack.
    static thread_local std::vector<uint8_t> buf(65536);
    for (int i = 0; i < kMaxDatagrams; ++i) {
        sockaddr_in sa{};
        socklen_t   len = sizeof sa;
        ssize_t     n   = ::recvfrom(l.fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr *>(&sa), &len);
        if (n < 0) break;
        UdpKey key{sa.sin_addr.s_addr, sa.sin_port};
        auto   it = clients.find(key);
        if (it == clients.end()) {
            uint16_t port = stack_.allocate_port();
            if (port == 0) {
                log_.log(Level::Debug, std::format("datagram from {} dropped: no free port", address_text(sa)));
                continue;
            }
            int fd = l.fd;
            // Replies go back through the published socket to this client.
            stack_.bind_udp(port, [this, fd, sa, key](uint16_t, std::span<const uint8_t> payload) {
                ::sendto(fd,
                         payload.data(),
                         payload.size(),
                         MSG_DONTWAIT,
                         reinterpret_cast<const sockaddr *>(&sa),
                         sizeof sa);
                if (auto c = clients_.find(fd); c != clients_.end())
                    if (auto k = c->second.find(key); k != c->second.end()) k->second.last_seen = now_;
            });
            log_.log(Level::Debug,
                     std::format("UDP client {} uses local port {} for guest port {}",
                                 address_text(sa),
                                 port,
                                 l.publish.guest_port));
            it = clients.emplace(key, UdpClient{port, now}).first;
        }
        it->second.last_seen = now;
        if (!stack_.send_udp(it->second.local_port, l.publish.guest_port, {buf.data(), size_t(n)}))
            log_.log(Level::Debug,
                     std::format("datagram from {} dropped: the firmware has no address", address_text(sa)));
    }
    for (auto it = clients.begin(); it != clients.end();) {
        if (now - it->second.last_seen >= UDP_IDLE_NS) {
            stack_.unbind_udp(it->second.local_port);
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
}

void Forwarder::poll(uint64_t now) {
    now_ = now;
    for (Listener &l : listeners_) {
        if (l.publish.udp)
            serve_udp(l, now);
        else
            accept_clients(l, now);
    }
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (serve(*it, now)) {
            ++it;
        } else {
            drop(*it);
            it = connections_.erase(it);
        }
    }
}

} // namespace bk
