#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "net/tcp_flow.h"

namespace bk {

// The side of a virtual network that can reach the firmware: what the port
// forwarder needs. Implemented by the client-role Stack and by the Router.
class GuestLink {
  public:
    using UdpHandler = std::function<void(uint16_t src_port, std::span<const uint8_t> payload)>;
    using TcpHandle  = std::shared_ptr<TcpFlow>;

    virtual ~GuestLink() = default;

    // The next free local port, increasing; 0 when every port is taken.
    virtual uint16_t allocate_port()                             = 0;
    virtual void     bind_udp(uint16_t port, UdpHandler handler) = 0;
    virtual void     unbind_udp(uint16_t port)                   = 0;
    // False while the firmware cannot be reached yet.
    virtual bool send_udp(uint16_t local_port, uint16_t guest_port, std::span<const uint8_t> data) = 0;
    // Connects from a fresh local port to `guest_port`. Null while the firmware
    // cannot be reached yet or no port is left.
    virtual TcpHandle open_tcp(uint16_t            guest_port,
                               TcpFlow::Deliver    deliver,
                               TcpFlow::PeerClosed on_peer_closed,
                               TcpFlow::Closed     on_closed,
                               uint64_t            now_ns) = 0;
};

} // namespace bk
