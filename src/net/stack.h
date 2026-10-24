#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "net/packets.h"
#include "net/tcp_flow.h"

namespace bk {

// The host side of the virtual network: one IPv4 station on Ethernet frames.
// It gets its address from the DHCP server of the firmware, answers ARP and
// ping, and carries UDP datagrams and TCP connections for the application
// side. Time is passed into every call; the stack has no clock of its own. The
// TCP flows read the time of the latest call, so a `write` between two calls
// is stamped with that call's time.
class Stack {
  public:
    enum class Dhcp { None, Discovering, Requesting, Bound };

    using Mac        = packets::Mac;
    using Output     = std::function<void(std::vector<uint8_t>)>;
    using UdpHandler = std::function<void(uint16_t src_port, std::span<const uint8_t> payload)>;

    static constexpr uint64_t DHCP_RETRY_NS = 3'000'000'000;
    static constexpr uint16_t FIRST_PORT    = 40000;
    static constexpr uint32_t TCP_ISS_STEP  = 100'000;

    using TcpHandle = std::shared_ptr<TcpFlow>;

    Stack(Mac mac, Output output);
    Stack(const Stack &)            = delete;
    Stack &operator=(const Stack &) = delete;

    // Forgets the address, the DHCP exchange, the UDP bindings, the TCP flows
    // (silently: no segment, no callback) and the ports.
    void reset();
    // The link came up: start from scratch and ask for an address.
    void                    link_up(uint64_t now_ns);
    void                    receive(std::span<const uint8_t> frame, uint64_t now_ns);
    void                    tick(uint64_t now_ns);
    std::optional<uint64_t> next_event_ns() const;

    // The next free local port, increasing; 0 when every port is taken.
    uint16_t allocate_port();
    void     bind_udp(uint16_t port, UdpHandler handler);
    void     unbind_udp(uint16_t port);
    // False while there is no address yet.
    bool send_udp(uint16_t local_port, uint16_t guest_port, std::span<const uint8_t> data);
    // Connects from a fresh local port to `guest_port` and sends the SYN.
    // Null while there is no address or no port is left. Finished flows leave
    // the table on the next tick; the handle stays valid (and inert) after that.
    TcpHandle open_tcp(uint16_t            guest_port,
                       TcpFlow::Deliver    deliver,
                       TcpFlow::PeerClosed on_peer_closed,
                       TcpFlow::Closed     on_closed,
                       uint64_t            now_ns);

    uint32_t   ip() const { return ip_; }
    uint32_t   guest_ip() const { return guest_ip_; }
    const Mac &guest_mac() const { return guest_mac_; }
    Dhcp       dhcp_state() const { return dhcp_; }

  private:
    class Link : public TcpLink {
      public:
        explicit Link(Stack &stack) : stack_(stack) {}
        void     send_segment(const packets::Tcp &segment) override;
        uint64_t now_ns() const override { return stack_.now_; }

      private:
        Stack &stack_;
    };

    void on_arp(const packets::Ethernet &eth);
    void on_ipv4(const packets::Ethernet &eth, uint64_t now);
    void on_dhcp(const packets::Ethernet &eth, const packets::Udp &udp, uint64_t now);
    void send_discover(uint64_t now);
    void send_request(uint64_t now);
    void send_dhcp(uint8_t type, std::optional<uint32_t> requested, std::optional<uint32_t> server_id);
    // To the firmware; false while unbound.
    bool send_ip(uint8_t protocol, uint32_t dst, std::span<const uint8_t> payload);
    bool is_free(uint16_t port) const;
    void on_tcp(const packets::Ipv4 &ip);

    Mac    mac_;
    Output output_;
    Link   link_{*this};
    // The time of the latest call, for the TCP flows.
    uint64_t now_ = 0;

    uint32_t ip_        = 0;
    uint32_t guest_ip_  = 0;
    Mac      guest_mac_ = {};

    Dhcp                    dhcp_ = Dhcp::None;
    uint32_t                xid_  = 0;
    std::optional<uint64_t> deadline_;
    // From the offer being requested.
    uint32_t offered_ip_ = 0;
    uint32_t server_id_  = 0;
    Mac      server_mac_ = {};

    std::map<uint16_t, UdpHandler> udp_;
    std::map<uint16_t, TcpHandle>  flows_;
    uint16_t                       next_port_ = FIRST_PORT;
    uint16_t                       ident_     = 0;
    uint32_t                       next_iss_  = 0;
};

} // namespace bk
