#pragma once

// Shared by the tests of the GDMA modules: a memory and a clock to run them
// against, and the config word bits, which the layouts share.

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "core/module.h"

namespace bk::gdma_test {

// Byte memory with holes: a read or write outside [0, size) is a bus error.
struct FakeMemory : BusMemory {
    std::vector<uint8_t> bytes;
    int                  reads = 0, writes = 0;
    explicit FakeMemory(size_t size = 0x1000) : bytes(size) {
        for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i * 3 + 1);
    }
    std::vector<uint8_t> read(uint32_t addr, uint32_t size) override {
        ++reads;
        if (uint64_t{addr} + size > bytes.size()) throw BusError(addr, "nothing readable there");
        return {bytes.begin() + addr, bytes.begin() + addr + size};
    }
    void write(uint32_t addr, std::span<const uint8_t> data) override {
        ++writes;
        if (uint64_t{addr} + data.size() > bytes.size()) throw BusError(addr, "nothing writable there");
        std::copy(data.begin(), data.end(), bytes.begin() + addr);
    }
};

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

constexpr uint32_t kEnable = 1, kFinInt = 2, kHalfInt = 4, kRepeat = 8, kSrcInc = 0x100, kDstInc = 0x200;
constexpr uint32_t kSrcLoop = 0x400, kDstLoop = 0x800;

constexpr uint32_t cfg(uint32_t length_bytes, uint32_t sdw, uint32_t ddw, uint32_t flags) {
    return (length_bytes - 1) << 16 | ddw << 6 | sdw << 4 | flags;
}

} // namespace bk::gdma_test
