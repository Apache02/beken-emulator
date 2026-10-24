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

namespace bk {

// A virtual 802.11 client of the access point the firmware runs. It scans for
// a beacon, authenticates, associates and then bridges data frames to and from
// Ethernet. Only open networks are joined. It has no notion of IP: whoever
// owns it feeds `send_ethernet` and takes `ethernet_in`.
class Station {
  public:
    enum class State { Scan, Auth, Assoc, Run };

    using Mac        = std::array<uint8_t, 6>;
    using Deliver    = std::function<void(std::vector<uint8_t>)>;
    using EthernetIn = std::function<void(std::vector<uint8_t>)>;
    using LinkUp     = std::function<void(uint64_t now_ns)>;

    static constexpr uint64_t LATENCY_NS    = 200'000;
    static constexpr uint64_t RETRY_NS      = 1'000'000'000;
    static constexpr uint64_t AUTH_DELAY_NS = 50'000'000;

    Station(Mac mac, std::optional<std::string> ssid_filter, Logger &log);

    // Hands received frames to the MAC.
    void attach(Deliver deliver) { deliver_ = std::move(deliver); }
    // An Ethernet frame for the host stack, once the firmware sent data.
    void set_ethernet_in(EthernetIn f) { ethernet_in_ = std::move(f); }
    // Called when the association completes.
    void set_link_up(LinkUp f) { link_up_ = std::move(f); }

    // Every frame the firmware sends, without its FCS.
    void transmit(std::span<const uint8_t> frame);
    // An Ethernet frame from the host stack towards the firmware.
    void                    send_ethernet(std::span<const uint8_t> frame);
    void                    tick(uint64_t now_ns);
    std::optional<uint64_t> next_event_ns() const;
    void                    reset();

    State                     state() const { return state_; }
    const std::optional<Mac> &bssid() const { return bssid_; }

  private:
    struct Pending {
        uint64_t             due;
        std::vector<uint8_t> frame;
    };

    void                 on_management(std::span<const uint8_t> f);
    void                 on_data(std::span<const uint8_t> f);
    void                 scan_frame(std::span<const uint8_t> f);
    void                 queue(std::vector<uint8_t> frame);
    std::vector<uint8_t> header(uint8_t fc0, uint8_t fc1, const Mac &a1, const Mac &a2, const Mac &a3);
    std::vector<uint8_t> management(uint8_t subtype, const Mac &dest, std::span<const uint8_t> body);
    void                 send_probe_request();
    void                 send_auth_request();
    void                 send_assoc_request();
    static void          elements(std::vector<uint8_t> &body, const std::string &ssid);

    Mac                        mac_;
    std::optional<std::string> filter_;
    Logger                    &log_;
    Deliver                    deliver_;
    EthernetIn                 ethernet_in_;
    LinkUp                     link_up_;

    State                   state_ = State::Scan;
    std::optional<Mac>      bssid_;
    std::string             ssid_;
    std::optional<uint64_t> next_;
    uint64_t                now_ = 0;
    unsigned                seq_ = 0;
    std::deque<Pending>     outbox_;
};

} // namespace bk
