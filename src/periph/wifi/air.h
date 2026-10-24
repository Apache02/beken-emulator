#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace bk {

// The far end of the WiFi MAC: whatever the frames the firmware transmits go
// to, and whatever hands frames back to the MAC.
class WifiAir {
  public:
    using Deliver = std::function<void(std::vector<uint8_t>)>;

    virtual ~WifiAir() = default;

    // Gives the air the function that hands a received frame to the MAC.
    virtual void attach(Deliver deliver) = 0;
    // Every frame the firmware sends, without its FCS.
    virtual void                    transmit(std::span<const uint8_t> frame) = 0;
    virtual void                    tick(uint64_t now_ns)                    = 0;
    virtual std::optional<uint64_t> next_event_ns() const                    = 0;
    virtual void                    reset()                                  = 0;
};

} // namespace bk
