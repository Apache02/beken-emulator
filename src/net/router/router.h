#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "core/log.h"
#include "net/guest_link.h"
#include "net/packets.h"
#include "net/tcp_flow.h"

namespace bk {

struct RouterConfig {
    uint32_t     gateway = packets::ip4(10, 0, 2, 2);
    uint32_t     guest   = packets::ip4(10, 0, 2, 15);
    uint32_t     netmask = packets::ip4(255, 255, 255, 0);
    uint32_t     lease_s = 86400;
    packets::Mac mac     = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
};

class Nat;

// The network behind the virtual access point, as the firmware (a station)
// sees it: a gateway that answers ARP and ping, a DHCP server that leases the
// one guest address, and a GuestLink so the port forwarder can reach the
// firmware from the gateway address. Unicast UDP and TCP beyond the LAN and DNS
// to the gateway are for the NAT; without one they are dropped. Time is passed into
// every call, as in Stack.
class Router : public GuestLink {
  public:
    using Mac    = packets::Mac;
    using Output = std::function<void(std::vector<uint8_t>)>; // Ethernet towards the firmware

    static constexpr uint16_t FIRST_PORT   = 40000;
    static constexpr uint32_t TCP_ISS_STEP = 100'000;

    Router(RouterConfig config, Output output, Logger &log);
    Router(const Router &)            = delete;
    Router &operator=(const Router &) = delete;

    // Null: off-LAN traffic is dropped.
    void                    set_nat(Nat *nat) { nat_ = nat; }
    void                    receive(std::span<const uint8_t> frame, uint64_t now_ns);
    void                    tick(uint64_t now_ns);
    std::optional<uint64_t> next_event_ns() const;
    // Link down/up: forgets the firmware's MAC, the UDP bindings and the TCP
    // flows (silently: no segment, no callback) and the ports; the lease
    // address stays the same.
    void reset();
    // 0 until a lease was acknowledged.
    uint32_t guest_ip() const { return guest_ip_; }
    // Sends an IPv4 packet from `src` to the firmware; false while it has no
    // lease or its MAC is not known.
    bool send_ip(uint32_t src, uint8_t protocol, std::span<const uint8_t> payload);

    // GuestLink: the forwarder's flows, from the gateway address.
    uint16_t  allocate_port() override;
    void      bind_udp(uint16_t port, UdpHandler handler) override;
    void      unbind_udp(uint16_t port) override;
    bool      send_udp(uint16_t local_port, uint16_t guest_port, std::span<const uint8_t> data) override;
    TcpHandle open_tcp(uint16_t            guest_port,
                       TcpFlow::Deliver    deliver,
                       TcpFlow::PeerClosed on_peer_closed,
                       TcpFlow::Closed     on_closed,
                       uint64_t            now_ns) override;

  private:
    class Link : public TcpLink {
      public:
        explicit Link(Router &router) : router_(router) {}
        void     send_segment(const packets::Tcp &segment) override;
        uint64_t now_ns() const override { return router_.now_; }

      private:
        Router &router_;
    };

    void on_arp(const packets::Ethernet &eth);
    void on_ipv4(const packets::Ethernet &eth);
    void on_dhcp(const packets::Udp &udp);
    void on_tcp(const packets::Ipv4 &ip);
    void send_dhcp(uint8_t type, const packets::Dhcp &request, uint32_t yiaddr);
    bool reachable() const { return guest_ip_ != 0 && guest_mac_.has_value(); }
    bool is_free(uint16_t port) const;

    RouterConfig config_;
    Output       output_;
    Logger      &log_;
    Nat         *nat_ = nullptr;
    Link         link_{*this};
    // The time of the latest call, for the TCP flows.
    uint64_t now_ = 0;

    uint32_t           guest_ip_ = 0;
    std::optional<Mac> guest_mac_;

    std::map<uint16_t, UdpHandler> udp_;
    std::map<uint16_t, TcpHandle>  flows_;
    uint16_t                       next_port_ = FIRST_PORT;
    uint16_t                       ident_     = 0;
    uint32_t                       next_iss_  = 0;
};

} // namespace bk
