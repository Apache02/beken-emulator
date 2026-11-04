#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// GPIO: 40 pins in two banks, each with a config word, plus interrupt
// registers. Peripheral functions are not modelled. A write that changes a
// pin's mode (output, input, peripheral function or disabled, and its pull) is
// logged at INFO; a change of the output level alone is not.
class Gpio : public Module {
  public:
    static constexpr unsigned kPins = 40;

    // `pin_reset` is the reset value of every pin word; `reset_words` overlays
    // it (and sets the plain words). `irq` and `site` may be empty.
    Gpio(
        std::string name, Line irq, uint32_t pin_reset, std::map<uint32_t, uint32_t> reset_words, SiteSource site = {});

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x100}
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    // Pin words and plain words go back to the reset values and nothing stays
    // latched. Levels forced by the host survive.
    void reset() override;

    // Host side. A pin number of kPins or more throws std::invalid_argument.
    // set_input forces the level of a pin's pad (nullopt releases it).
    void set_input(unsigned pin, std::optional<bool> level);
    bool pad_level(unsigned pin) const;
    // `fn(pin, level)` is called on every pad change.
    void watch(std::function<void(unsigned, bool)> fn) { watchers_.push_back(std::move(fn)); }

  private:
    static void check_pin(unsigned pin);
    uint32_t    word(uint32_t offset) const;
    bool        compute_pad(unsigned pin) const;
    void        refresh(unsigned pin);
    bool        enabled(unsigned pin) const;
    unsigned    mode(unsigned pin) const;
    void        settle();
    uint64_t    pending() const;
    void        update_irq();
    void        log_mode(unsigned pin, uint32_t cfg) const;

    std::string                                      name_;
    Line                                             irq_;
    SiteSource                                       site_;
    uint32_t                                         pin_reset_;
    std::map<uint32_t, uint32_t>                     reset_words_, regs_;
    std::array<std::optional<bool>, kPins>           external_;
    std::array<bool, kPins>                          pad_{};
    uint64_t                                         latched_ = 0;
    std::vector<std::function<void(unsigned, bool)>> watchers_;
};

} // namespace bk
