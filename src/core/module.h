#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bk {

// Thrown by modules to raise a data or prefetch abort at `addr`.
struct Fault {
    enum class Kind { Dabt, Pabt } kind;
    uint32_t addr;
};

// A named address range exposed by a module.
struct Range {
    std::string name;
    uint32_t    size;
};

// Half-open span [start, end) of bytes within a page that must not be cached.
struct BadSpan {
    uint32_t start, end;
};

struct Page {
    std::array<uint8_t, 4096> data;
    std::vector<BadSpan>      bad;
};

// Interrupt or level line driven by a module: called with the new level.
using Line = std::function<void(bool)>;
// Polled boolean input provided by another module.
using Signal = std::function<bool()>;

// Describes a boolean signal derived from a register field: active while
// ((register(offset) >> shift) & ((1 << width) - 1)) == equals.
struct SignalSpec {
    uint32_t offset;
    unsigned shift = 0, width = 1;
    uint32_t equals = 1;
};

struct Clock {
    virtual ~Clock()                = default;
    virtual uint64_t now_ns() const = 0;
};

// Contract between the core and every device. The core never knows concrete
// module types; collaborators (clock, lines, signals) arrive through each
// module's constructor.
class Module {
  public:
    virtual ~Module() = default;

    virtual std::string name() const = 0;
    // Whether images can be loaded into this module (see check_load/load).
    virtual bool loadable() const { return false; }
    // Ranges served page by page on demand rather than via read/write.
    virtual std::vector<Range> demand_ranges() const { return {}; }
    virtual std::vector<Range> ranges() const = 0;

    virtual uint32_t read(const std::string &range, uint32_t offset, uint32_t size)                  = 0;
    virtual void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) = 0;
    virtual Page     page(const std::string &range, uint32_t page_offset) {
        (void)range;
        throw Fault{Fault::Kind::Dabt, page_offset};
    }

    virtual void tick(uint64_t now_ns) { (void)now_ns; }
    // Time of the next scheduled event, if any.
    virtual std::optional<uint64_t> next_event_ns() const { return std::nullopt; }
    virtual void                    reset() {}

    // Throws std::invalid_argument if [offset, offset+size) cannot be loaded.
    virtual void check_load(uint32_t offset, uint32_t size) const {
        (void)offset;
        (void)size;
        throw std::invalid_argument(name() + ": not loadable");
    }
    virtual void load(uint32_t offset, std::span<const uint8_t> data) {
        check_load(offset, static_cast<uint32_t>(data.size()));
    }

    // Set by the bus: tells it that [offset, offset+size) of `range` changed.
    std::function<void(const std::string &, uint32_t, uint32_t)> invalidate;

    virtual Signal signal(const std::string &signal_name) {
        throw std::invalid_argument(name() + ": no signal " + signal_name);
    }

    // Interrupt controllers hand out the line driving source `bit`; other
    // modules refuse.
    virtual Line line(unsigned bit) {
        throw std::invalid_argument(name() + ": not an interrupt controller (asked for line " + std::to_string(bit) +
                                    ")");
    }
};

} // namespace bk
