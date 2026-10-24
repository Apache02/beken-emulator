#include "net/network.h"

namespace bk {

uint64_t Network::steady_wall_ns() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

Network::Network(
    std::vector<Publish> publishes, Mac mac, std::optional<std::string> ssid, uint64_t poll_ns, WallClock wall)
    : station_(mac, std::move(ssid), Logger::get("net.wifi")),
      stack_(mac, [this](std::vector<uint8_t> frame) { station_.send_ethernet(frame); }), forwarder_(stack_, publishes),
      poll_ns_(poll_ns), wall_(std::move(wall)) {
    station_.set_ethernet_in([this](std::vector<uint8_t> frame) { stack_.receive(frame, now_); });
    // Re-association restarts the stack, which ends its flows without a word;
    // the forwarder has to say goodbye to its connections first.
    station_.set_link_up([this](uint64_t now) {
        forwarder_.reset();
        stack_.link_up(now);
    });
}

void Network::tick(uint64_t now_ns) {
    now_ = now_ns;
    station_.tick(now_ns);
    if (forwarder_.empty()) return;
    uint64_t wall = wall_();
    if (polled_ && wall - *polled_ < poll_ns_) return;
    polled_ = wall;
    forwarder_.poll(now_ns);
}

void Network::reset() {
    forwarder_.reset();
    stack_.reset();
    station_.reset();
}

} // namespace bk
