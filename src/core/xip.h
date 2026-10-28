#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace bk {

// Half-open span [start, end) of bytes within a page that must not be cached.
struct BadSpan {
    uint32_t start, end;
};

struct Page {
    std::array<uint8_t, 4096> data;
    std::vector<BadSpan>      bad;
};

// A read-only memory behind a peripheral that the bus maps page by page on
// demand: the execute-in-place window of a flash. The module also has registers
// of its own, which stay in its ranges(); a module that has this interface is
// given an address for the window by the base named "xip".
class XipMemory {
  public:
    virtual ~XipMemory() = default;

    // The size of the window in bytes, a multiple of 4096.
    virtual uint32_t xip_size() const = 0;
    // The bytes at `offset` the way the CPU would read them. Logs the details
    // of a failure and throws Fault when the bytes cannot be read.
    virtual uint32_t xip_read(uint32_t offset, uint32_t size) = 0;
    // The 4 KiB page at `page_offset`, with spans of bytes that cannot be read
    // marked bad. Throws Fault when the page cannot be given at all.
    virtual Page xip_page(uint32_t page_offset) = 0;

    // Set by the bus: tells it that [offset, offset + size) of the window changed.
    std::function<void(uint32_t, uint32_t)> xip_invalidate;
};

} // namespace bk
