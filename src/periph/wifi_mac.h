#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"
#include "periph/unknown.h"

namespace bk {

// The WiFi MAC, so far only its monotonic counters (core bank, 0xC0000000):
//
// - 0x11C counter 1: 32 bits at 40 MHz; writes are ignored.
// - 0x120 / 0x124 counter 2: a 48-bit microsecond counter. `lo` holds the low
//   32 bits, `hi` the high 16 bits plus a self-clearing software-update flag
//   in bit 31 (reads 0). A write to `lo` is remembered but not stored; a write
//   to `hi` with bit 31 set loads the counter from `hi` and the last `lo`.
//
// They count from 0 while the MAC is powered (the `mac_powered` signal, always
// powered without one) and read 0 while it is powered down. Every other
// register of the core and platform banks is a stand-in: plain memory that
// logs its accesses (see Unknown).
class WifiMac : public Module {
  public:
    static constexpr uint32_t kCoreSize = 0x1580, kPlSize = 0x57C;

    // `powered` may be empty. `instruction` is passed to the stand-in banks.
    WifiMac(std::string name, const Clock &clock, Signal powered, Unknown::InstructionSource instruction);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"core", kCoreSize},
            {"pl",   kPlSize  }
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    // Notices power changes (the counters restart on every power-up).
    void tick(uint64_t now_ns) override;
    void reset() override;

    // The 48-bit microsecond counter, or nothing while powered down.
    std::optional<uint64_t> microseconds();

  private:
    std::optional<uint64_t> elapsed_ns();
    uint32_t                counter(uint32_t word);
    void                    software_update(uint32_t value);

    std::string             name_;
    const Clock            &clock_;
    Signal                  powered_;
    Unknown                 core_, pl_;
    std::optional<uint64_t> start_;
    uint64_t                offset_us_  = 0;
    uint32_t                lo_written_ = 0;
};

} // namespace bk
