#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/log.h"
#include "net/router/authenticator.h"
#include "net/station.h"
#include "periph/wifi/air.h"

namespace bk {

struct ApConfig {
    std::string                ssid;
    std::optional<std::string> password; // WPA2-PSK when set, else open
    unsigned                   channel = 1;
    Station::Mac               bssid   = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
};

// A virtual 802.11 access point for firmware that runs as a station. It
// beacons, answers probe requests, takes one station through open system
// authentication and association (with a password, then the WPA2-PSK 4-way
// handshake) and then bridges its data frames to and from Ethernet. It has no
// notion of IP: whoever owns it takes `ethernet_in` and feeds `send_ethernet`.
class AccessPoint : public WifiAir {
  public:
    using Mac        = Station::Mac;
    using EthernetIn = std::function<void(std::vector<uint8_t>)>;
    using LinkChange = std::function<void(bool up, const Mac &station, uint64_t now_ns)>;
    // The channel the firmware's radio is tuned to, or nothing when unknown.
    using TunedChannel = std::function<std::optional<unsigned>()>;

    static constexpr uint64_t kBeaconIntervalNs = 102'400'000; // 100 TU
    static constexpr uint64_t kLatencyNs        = 200'000;

    // `random` draws the group key and the nonces of the handshake; empty means std::random_device.
    AccessPoint(ApConfig config, TunedChannel tuned, Logger &log, Authenticator::Random random = {});

    void                    attach(Deliver deliver) override { deliver_ = std::move(deliver); }
    void                    transmit(std::span<const uint8_t> frame) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;

    // An Ethernet frame from the station, for the router.
    void set_ethernet_in(EthernetIn f) { ethernet_in_ = std::move(f); }
    // Called when a station's link comes up or goes away.
    void set_link_change(LinkChange f) { link_change_ = std::move(f); }

    // Towards the associated station; dropped while none is (or, with WPA2, before the handshake).
    void               send_ethernet(std::span<const uint8_t> eth);
    std::optional<Mac> station() const { return station_; }

  private:
    struct Pending {
        uint64_t             due;
        std::vector<uint8_t> frame;
        bool                 on_channel; // heard only while the radio is on our channel
    };

    void                 on_management(std::span<const uint8_t> f);
    void                 on_data(std::span<const uint8_t> f);
    void                 on_probe_request(std::span<const uint8_t> f);
    void                 on_auth(const Mac &sta, std::span<const uint8_t> body);
    void                 on_assoc(const Mac &sta, uint8_t subtype);
    void                 associated(const Mac &sta);
    void                 forget(const char *why);
    void                 on_eapol(std::span<const uint8_t> eapol);
    void                 send_eapol(std::span<const uint8_t> eapol);
    bool                 heard() const;
    void                 queue(std::vector<uint8_t> frame, bool on_channel = false);
    std::vector<uint8_t> header(uint8_t fc0, uint8_t fc1, const Mac &a1, const Mac &a3);
    std::vector<uint8_t> beacon_like(uint8_t subtype, const Mac &dest, uint64_t at_ns);
    uint16_t             capabilities() const;

    ApConfig                               config_;
    TunedChannel                           tuned_;
    Logger                                &log_;
    Authenticator::Random                  random_;
    std::optional<std::array<uint8_t, 32>> pmk_;  // with a password
    std::optional<Authenticator>           auth_; // the handshake with the associated station
    Deliver                                deliver_;
    EthernetIn                             ethernet_in_;
    LinkChange                             link_change_;

    std::optional<Mac>  authenticated_;       // passed authentication, not yet associated
    std::optional<Mac>  station_;             // associated
    bool                port_open_   = false; // data may flow; with WPA2 only after the handshake
    uint64_t            next_beacon_ = 0;
    uint64_t            now_         = 0;
    unsigned            seq_         = 0;
    std::deque<Pending> outbox_;
};

// RC word 0x94 bits [31:25] hold the tuned frequency minus 2400 MHz; channel n is 2407 + 5n MHz
// (14 is 2484). Returns nothing for 0 or a value that is no 2.4 GHz channel.
std::optional<unsigned> channel_from_rc(uint32_t pll_ldo_chan);

} // namespace bk
