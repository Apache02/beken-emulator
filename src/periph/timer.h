#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// Three periodic timers sharing one register block and one divider.
class Timer : public Module {
  public:
    Timer(std::string name, const Clock &clock, Line irq, uint32_t hz = 26000000);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x10}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;

  private:
    uint64_t period_ns(uint32_t counter) const;
    bool     running(size_t i) const { return (enable_ >> i & 1) && counter_[i] > 0; }
    void     update_line();

    std::string                            name_;
    const Clock                           &clock_;
    Line                                   irq_;
    uint32_t                               hz_;
    std::array<uint32_t, 3>                counter_{};
    std::array<std::optional<uint64_t>, 3> next_{};
    uint32_t                               enable_ = 0, divider_ = 0, status_ = 0;
};

} // namespace bk
