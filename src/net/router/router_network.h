#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "net/forwarder.h"
#include "net/network.h"
#include "net/router/access_point.h"
#include "net/router/nat.h"
#include "net/router/resolver.h"
#include "net/router/router.h"
#include "periph/wifi/air.h"

namespace bk {

// The far end of the WiFi air for firmware that runs as a station: a virtual
// access point, the router behind it, the port forwarder in front and the NAT
// with the DNS relay towards the host's resolver beyond. The
// counterpart of Network for the access point role; the MAC module talks to
// it as a WifiAir.
class RouterNetwork : public WifiAir {
  public:
    // Throws ForwardError when a publish cannot be bound. DNS goes to
    // `dns_upstream`:`dns_port`, by default the host's resolver.
    RouterNetwork(ApConfig                  ap,
                  std::vector<Publish>      publishes,
                  AccessPoint::TunedChannel tuned,
                  uint64_t                  poll_ns      = Network::kDefaultPollNs,
                  Network::WallClock        wall         = Network::steady_wall_ns,
                  std::optional<uint32_t>   dns_upstream = host_resolver(),
                  uint16_t                  dns_port     = Nat::kDnsPort);
    RouterNetwork(const RouterNetwork &)            = delete;
    RouterNetwork &operator=(const RouterNetwork &) = delete;

    void attach(Deliver deliver) override { ap_.attach(std::move(deliver)); }
    void transmit(std::span<const uint8_t> frame) override { ap_.transmit(frame); }
    // Ticks the access point, the router and the NAT and, at most once per
    // poll interval of wall time, polls the forwarder and the NAT. The wall
    // clock is not read at all without publishes and NAT sessions.
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;
    // Closes the host sockets; the network keeps answering the air.
    void close() {
        forwarder_.close();
        nat_.reset();
    }

    // The publishes as bound, with the real host ports.
    const std::vector<Publish> &addresses() const { return forwarder_.addresses(); }
    AccessPoint                &access_point() { return ap_; }
    Router                     &router() { return router_; }
    Nat                        &nat() { return nat_; }

  private:
    AccessPoint        ap_;
    Router             router_;
    Forwarder          forwarder_;
    Nat                nat_;
    uint64_t           poll_ns_;
    Network::WallClock wall_;
    uint64_t           now_ = 0;
    // Wall time of the latest poll.
    std::optional<uint64_t> polled_;
};

} // namespace bk
