#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tuple>

#include "core/log.h"
#include "net/packets.h"

namespace bk {

// Carries the firmware's traffic beyond the LAN through host sockets: every
// UDP conversation (guest port, destination address and port) gets a connected
// host socket of its own, and what comes back on it goes to the firmware from
// that destination. DNS queries to the gateway are relayed the same way to the
// host's resolver and answered from the gateway. Every TCP connection the
// firmware opens becomes a non-blocking host connection; the firmware's side
// is a TcpFlow that speaks from the destination. Nothing blocks; `poll` moves
// whatever is ready. Sessions live on device time.
class Nat {
  public:
    // Router::send_ip: an IPv4 packet towards the firmware; false while it has no address.
    using ToGuest = std::function<bool(uint32_t src, uint8_t protocol, std::span<const uint8_t> payload)>;

    static constexpr uint64_t kUdpIdleNs = 60'000'000'000;
    static constexpr uint16_t kDnsPort   = 53;
    // Open sockets at most; beyond that the longest idle session gives way.
    static constexpr size_t kMaxUdpSessions = 256;
    // A host connection not made by then is given up; the firmware gets an RST.
    static constexpr uint64_t kConnectTimeoutNs = 10'000'000'000;
    // The most taken from a host socket at once.
    static constexpr size_t kTcpReadSize = 16384;

    // `dns_port` is the upstream's port; tests point it at a socket of their own.
    Nat(ToGuest                 to_guest,
        uint32_t                gateway,
        std::optional<uint32_t> dns_upstream,
        Logger                 &log,
        uint16_t                dns_port = kDnsPort);
    ~Nat();
    Nat(const Nat &)            = delete;
    Nat &operator=(const Nat &) = delete;

    void udp_out(uint32_t                 guest,
                 uint16_t                 guest_port,
                 uint32_t                 dst,
                 uint16_t                 dst_port,
                 std::span<const uint8_t> data,
                 uint64_t                 now_ns);
    // A query to the gateway's port 53, relayed to the upstream; the answer comes from gateway:53.
    void dns_query(uint32_t guest, uint16_t guest_port, std::span<const uint8_t> query, uint64_t now_ns);
    // A TCP segment from the firmware to `dst` beyond the LAN. A SYN opens a
    // session; a segment for no session gets an RST.
    void tcp_out(uint32_t guest, uint32_t dst, const packets::Tcp &segment, uint64_t now_ns);
    // Host sockets; never blocks.
    void poll(uint64_t now_ns);
    // Closes the UDP sessions idle for kUdpIdleNs, gives up host connections
    // older than kConnectTimeoutNs and runs the flows' timers.
    void                    tick(uint64_t now_ns);
    std::optional<uint64_t> next_event_ns() const;
    // Closes every session's socket; the TCP flows end without a word.
    void   reset();
    size_t udp_sessions() const { return udp_.size(); }
    size_t tcp_sessions() const { return tcp_.size(); }
    // Bytes queued towards the firmware over all TCP sessions, not yet acknowledged.
    size_t tcp_buffered() const;
    // Bytes from the firmware held for the host sockets, over all TCP sessions.
    size_t tcp_held() const;
    // No session, so nothing to poll.
    bool empty() const { return udp_.empty() && tcp_.empty(); }

  private:
    struct UdpSession {
        int      fd        = -1;
        uint32_t guest     = 0;
        uint64_t last_seen = 0;
        bool     answered  = false; // a datagram came back on it
    };
    struct TcpSession;

    // Guest port, then the address and port the firmware talks to (the
    // gateway's for DNS), which answers come from.
    using UdpKey = std::tuple<uint16_t, uint32_t, uint16_t>;
    // Guest port, destination address and port.
    using TcpKey = std::tuple<uint16_t, uint32_t, uint16_t>;
    using TcpMap = std::map<TcpKey, std::unique_ptr<TcpSession>>;

    void send_udp(const UdpKey            &key,
                  uint32_t                 guest,
                  uint32_t                 host,
                  uint16_t                 host_port,
                  std::span<const uint8_t> data,
                  uint64_t                 now);
    void receive(const UdpKey &key, UdpSession &s, uint64_t now);
    void make_room();
    // The DNS relay's session to the upstream, not the firmware's own UDP.
    bool is_dns(const UdpKey &key) const { return std::get<1>(key) == gateway_ && std::get<2>(key) == kDnsPort; }
    // The first connection to each remote address, at INFO.
    void tell(uint32_t remote, uint16_t port, const char *protocol);
    // One warning in all for an upstream that refuses DNS queries.
    void dns_refused();

    void open_tcp(const TcpKey &key, uint32_t guest, const packets::Tcp &syn);
    // RST to a segment from the firmware that has no flow to go to.
    void refuse(uint32_t guest, uint32_t dst, const packets::Tcp &segment);
    // The host connection is made: the SYN|ACK goes out.
    void connected(TcpSession &s);
    // Moves bytes between the host socket and the flow; false when the session is over.
    bool serve(TcpSession &s, bool read);
    // Removes the session at `it` if it is over; the next entry.
    TcpMap::iterator settle(TcpMap::iterator it, bool keep);
    void             close_tcp(TcpSession &s);

    ToGuest                      to_guest_;
    uint32_t                     gateway_;
    std::optional<uint32_t>      dns_upstream_;
    Logger                      &log_;
    uint16_t                     dns_port_;
    std::map<UdpKey, UdpSession> udp_;
    TcpMap                       tcp_;
    // The latest time passed in, for the TCP flows' clock.
    uint64_t now_      = 0;
    uint32_t next_iss_ = 0;
    // Remote addresses already reported at INFO, over TCP and UDP.
    std::set<uint32_t> seen_;
    // The DNS relay's upstream is reported once.
    bool dns_told_ = false;
};

} // namespace bk
