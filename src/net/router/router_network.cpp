#include "net/router/router_network.h"

#include <algorithm>

namespace bk {

RouterNetwork::RouterNetwork(ApConfig                  ap,
                             std::vector<Publish>      publishes,
                             AccessPoint::TunedChannel tuned,
                             uint64_t                  poll_ns,
                             Network::WallClock        wall,
                             std::optional<uint32_t>   dns_upstream,
                             uint16_t                  dns_port,
                             bool                      internet)
    : ap_(std::move(ap), std::move(tuned), Logger::get("net.ap")),
      router_(
          RouterConfig{}, [this](std::vector<uint8_t> frame) { ap_.send_ethernet(frame); }, Logger::get("net.router")),
      forwarder_(router_, publishes), nat_(
                                          [this](uint32_t src, uint8_t protocol, std::span<const uint8_t> payload) {
                                              return router_.send_ip(src, protocol, payload);
                                          },
                                          RouterConfig{}.gateway,
                                          dns_upstream,
                                          Logger::get("net.nat"),
                                          dns_port),
      poll_ns_(poll_ns), wall_(std::move(wall)) {
    if (internet) router_.set_nat(&nat_);
    ap_.set_ethernet_in([this](std::vector<uint8_t> frame) { router_.receive(frame, now_); });
    // A new association starts the router afresh. The router's reset ends its
    // flows without a word and forgets its UDP bindings, so the forwarder has
    // to say goodbye to its connections and forget its UDP clients first, also
    // on link up: a client heard while the link was down holds a binding.
    // The NAT's sessions belong to the old link and go with it.
    ap_.set_link_change([this](bool, const AccessPoint::Mac &, uint64_t) {
        forwarder_.reset();
        nat_.reset();
        router_.reset();
    });
}

void RouterNetwork::tick(uint64_t now_ns) {
    now_ = now_ns;
    ap_.tick(now_ns);
    router_.tick(now_ns);
    nat_.tick(now_ns);
    if (forwarder_.empty() && nat_.empty()) return;
    uint64_t wall = wall_();
    if (polled_ && wall - *polled_ < poll_ns_) return;
    polled_ = wall;
    forwarder_.poll(now_ns);
    nat_.poll(now_ns);
}

std::optional<uint64_t> RouterNetwork::next_event_ns() const {
    std::optional<uint64_t> next;
    for (auto t : {ap_.next_event_ns(), router_.next_event_ns(), nat_.next_event_ns()})
        if (t && (!next || *t < *next)) next = t;
    return next;
}

void RouterNetwork::reset() {
    forwarder_.reset();
    nat_.reset();
    router_.reset();
    ap_.reset();
}

} // namespace bk
