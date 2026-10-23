#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "core/module.h"

namespace bk {

// A signal that is active while the register field described by `spec` equals
// `spec.equals`. `read_register` is asked at every evaluation.
inline Signal register_signal(std::function<uint32_t(uint32_t offset)> read_register, SignalSpec spec) {
    return [read_register = std::move(read_register), spec] {
        uint64_t mask    = spec.width >= 32 ? 0xFFFFFFFFull : ((1ull << spec.width) - 1);
        uint32_t reg     = read_register(spec.offset);
        uint64_t shifted = spec.shift >= 32 ? 0 : (reg >> spec.shift);
        return (shifted & mask) == spec.equals;
    };
}

} // namespace bk
