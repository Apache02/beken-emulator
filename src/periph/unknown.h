#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// A stand-in for a block of registers that is not emulated yet: 32-bit
// registers that read back what was written, and a log line (INFO) for every
// access with the offset, the value and, when an instruction source is given,
// the address and opcode of the instruction that made it, so the log shows
// which stand-ins a firmware uses.
//
// The bus reaches a module with whole words at word addresses only (it widens
// narrow loads and stores itself); a read past the end of the block returns 0.
class Unknown : public Module {
  public:
    using InstructionSource = std::function<std::optional<Instruction>()>;

    // `size` is in bytes (rounded up to whole words). `reset_words` maps a
    // word-aligned offset inside the block to its start value; throws
    // std::invalid_argument for any other offset. `instruction` may be empty.
    Unknown(std::string name, uint32_t size, std::map<uint32_t, uint32_t> reset_words, InstructionSource instruction);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", size_}
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    // A 32-bit write at a word-aligned offset stores the word. A narrower write
    // changes only its bytes of the word that holds them; a write outside the
    // block is dropped.
    void write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void reset() override;

    // Unlogged word access for an owner that serves some registers itself.
    // `offset` is word-aligned; a peek outside the block returns 0 and a poke
    // there is dropped.
    uint32_t peek(uint32_t offset) const { return offset / 4 < words_.size() ? words_[offset / 4] : 0; }
    void     poke(uint32_t offset, uint32_t value) {
        if (offset / 4 < words_.size()) words_[offset / 4] = value;
    }

  private:
    void report(const char *what, uint32_t offset, uint32_t size, uint32_t value);

    std::string                  name_;
    uint32_t                     size_;
    std::map<uint32_t, uint32_t> reset_words_;
    InstructionSource            instruction_;
    std::vector<uint32_t>        words_;
};

} // namespace bk
