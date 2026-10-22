#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// Watchdog: one register holding a period in milliseconds and a kick key.
class Wdt : public Module {
  public:
    Wdt(std::string name, const Clock &clock, std::function<void()> reset_chip, Signal clock_off = {});

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 4}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;

  private:
    bool     running() const { return !clock_off_ || !clock_off_(); }
    uint64_t period_ns() const { return static_cast<uint64_t>(reg_ & 0xFFFF) * 1'000'000ULL; }

    std::string             name_;
    const Clock            &clock_;
    std::function<void()>   reset_chip_;
    Signal                  clock_off_;
    uint32_t                reg_       = 0x4000;
    bool                    key1_seen_ = false;
    std::optional<uint64_t> start_;
    std::optional<uint64_t> last_kick_;
};

} // namespace bk
