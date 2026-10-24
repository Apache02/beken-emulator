#include "periph/unknown.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace bk {

Unknown::Unknown(std::string                  name,
                 uint32_t                     size,
                 std::map<uint32_t, uint32_t> reset_words,
                 InstructionSource            instruction,
                 Level                        level)
    : name_(std::move(name)), size_(size), reset_words_(std::move(reset_words)), instruction_(std::move(instruction)),
      level_(level), words_((uint64_t{size} + 3) / 4, 0) {
    for (const auto &[offset, word] : reset_words_) {
        if (offset % 4 != 0 || offset / 4 >= words_.size())
            throw std::invalid_argument(name_ + ": reset word offset " + std::to_string(offset) +
                                        " is not a word inside the block");
    }
    reset();
}

void Unknown::reset() {
    std::fill(words_.begin(), words_.end(), 0);
    for (const auto &[offset, word] : reset_words_) words_[offset / 4] = word;
}

uint32_t Unknown::read(const std::string &, uint32_t offset, uint32_t size) {
    // The bus passes whole-word reads only.
    const uint32_t value = offset / 4 < words_.size() ? words_[offset / 4] : 0;
    report("read", offset, size, value);
    return value;
}

void Unknown::write(const std::string &, uint32_t offset, uint32_t size, uint32_t value) {
    if (size < 4) value &= (1u << (size * 8)) - 1;
    report("write", offset, size, value);
    if (offset / 4 >= words_.size()) return;
    uint32_t &word = words_[offset / 4];
    if (size == 4 && offset % 4 == 0) {
        word = value;
    } else if (size < 4 && offset % 4 + size <= 4) {
        const unsigned shift = (offset % 4) * 8;
        const uint32_t mask  = ((1u << (size * 8)) - 1) << shift;
        word                 = (word & ~mask) | ((value << shift) & mask);
    }
}

void Unknown::report(const char *what, uint32_t offset, uint32_t size, uint32_t value) {
    char text[160];
    int  n = std::snprintf(text,
                          sizeof text,
                          "%s %s +0x%04X (%u bytes) = 0x%0*X",
                          name_.c_str(),
                          what,
                          offset,
                          size,
                          static_cast<int>(size * 2),
                          value);
    if (instruction_) {
        if (std::optional<Instruction> insn = instruction_()) {
            std::snprintf(text + n,
                          sizeof text - static_cast<size_t>(n),
                          "  pc 0x%08X insn %0*X",
                          insn->pc,
                          static_cast<int>(insn->width * 2),
                          insn->opcode);
        }
    }
    Logger::get("unknown").log(level_, text);
}

} // namespace bk
