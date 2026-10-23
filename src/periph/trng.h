#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// True random number generator that is not random: while enabled, every read
// of `data` returns a counter (0, 1, 2, ...), so runs are repeatable. While
// disabled `data` is a plain latch. It starts enabled.
class Trng : public Module {
  public:
    explicit Trng(std::string name) : name_(std::move(name)) {}

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 8}
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void     reset() override;

  private:
    std::string name_;
    uint32_t    ctrl_ = 1, count_ = 0, latch_ = 0;
};

} // namespace bk
