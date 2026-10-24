#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "net/forwarder.h"
#include "net/stack.h"
#include "net/station.h"
#include "periph/wifi/air.h"

namespace bk {

// The far end of the WiFi air: a virtual station that joins the firmware's
// access point, the host IP stack behind it and the port forwarder in front.
// The MAC module talks to it as a WifiAir.
class Network : public WifiAir {
  public:
    using Mac = Station::Mac;
    // Wall time in nanoseconds from any fixed origin; replaceable in tests.
    using WallClock = std::function<uint64_t()>;

    static constexpr Mac      kDefaultMac    = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    static constexpr uint64_t kDefaultPollNs = 2'000'000;

    static uint64_t steady_wall_ns();

    // Throws ForwardError when a publish cannot be bound. `ssid`, when given,
    // is the only network the station joins.
    explicit Network(std::vector<Publish>       publishes = {},
                     Mac                        mac       = kDefaultMac,
                     std::optional<std::string> ssid      = std::nullopt,
                     uint64_t                   poll_ns   = kDefaultPollNs,
                     WallClock                  wall      = steady_wall_ns);
    Network(const Network &)            = delete;
    Network &operator=(const Network &) = delete;

    void attach(Deliver deliver) override { station_.attach(std::move(deliver)); }
    void transmit(std::span<const uint8_t> frame) override { station_.transmit(frame); }
    // Ticks the station and, at most once per poll interval of wall time, the
    // forwarder. The wall clock is not read at all without publishes.
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override { return station_.next_event_ns(); }
    void                    reset() override;
    // Closes the host sockets; the network keeps answering the air.
    void close() { forwarder_.close(); }

    // The publishes as bound, with the real host ports.
    const std::vector<Publish> &addresses() const { return forwarder_.addresses(); }
    Station                    &station() { return station_; }
    Stack                      &stack() { return stack_; }
    Forwarder                  &forwarder() { return forwarder_; }

  private:
    Station   station_;
    Stack     stack_;
    Forwarder forwarder_;
    uint64_t  poll_ns_;
    WallClock wall_;
    uint64_t  now_ = 0;
    // Wall time of the latest poll.
    std::optional<uint64_t> polled_;
};

} // namespace bk
