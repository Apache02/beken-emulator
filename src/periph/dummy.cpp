#include "periph/dummy.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace bk {

Dummy::Dummy(std::string                  name,
             uint32_t                     size,
             std::map<uint32_t, uint32_t> reset_words,
             SiteSource                   site,
             Level                        level,
             std::map<uint32_t, uint32_t> self_clear)
    : name_(std::move(name)), size_(size), reset_words_(std::move(reset_words)), site_(std::move(site)), level_(level),
      self_clear_(std::move(self_clear)), words_((uint64_t{size} + 3) / 4, 0) {
    auto check = [this](const std::map<uint32_t, uint32_t> &words, const char *what) {
        for (const auto &[offset, word] : words) {
            if (offset % 4 != 0 || offset / 4 >= words_.size())
                throw std::invalid_argument(name_ + ": " + what + " offset " + std::to_string(offset) +
                                            " is not a word inside the block");
        }
    };
    check(reset_words_, "reset word");
    check(self_clear_, "self_clear");
    reset();
}

void Dummy::reset() {
    std::fill(words_.begin(), words_.end(), 0);
    for (const auto &[offset, word] : reset_words_) words_[offset / 4] = word;
}

uint32_t Dummy::read(const std::string &, uint32_t offset, uint32_t size) {
    // The bus passes whole-word reads only.
    const uint32_t value = offset / 4 < words_.size() ? words_[offset / 4] : 0;
    report("read", offset, size, value);
    return value;
}

void Dummy::write(const std::string &, uint32_t offset, uint32_t size, uint32_t value) {
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
    if (auto it = self_clear_.find(offset & ~3u); it != self_clear_.end()) word &= ~it->second;
}

void Dummy::report(const char *what, uint32_t offset, uint32_t size, uint32_t value) {
    if (level_ == Level::None) return;

    char text[160];
    std::snprintf(text,
                  sizeof text,
                  "%s %s +0x%04X (%u bytes) = 0x%0*X",
                  name_.c_str(),
                  what,
                  offset,
                  size,
                  static_cast<int>(size * 2),
                  value);
    Logger::get("dummy").log(level_, site_ ? text + by_site(site_()) : std::string(text));
}

} // namespace bk
