#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
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

// The instruction the CPU is executing: its address, its opcode and the opcode
// width in bytes (2 for Thumb, 4 for ARM).
struct Instruction {
    uint32_t pc, opcode;
    unsigned width;
};

// Thrown by BusMemory when a bus master (a DMA engine) touches an address that
// nothing readable or writable answers at.
struct BusError : std::runtime_error {
    BusError(uint32_t addr, const std::string &reason);
    uint32_t addr;
};

// What a bus master can reach: RAM, and demand ranges (flash) read-only.
// Peripheral registers are not reachable this way. Throws BusError.
class BusMemory {
  public:
    virtual ~BusMemory()                                                             = default;
    virtual std::vector<uint8_t> read(uint32_t addr, uint32_t size)                  = 0;
    virtual void                 write(uint32_t addr, std::span<const uint8_t> data) = 0;
};

// A peripheral's request line into a DMA engine: the peripheral holds the line
// up while it has a word to give and supplies the word through bind().
class RequestLine {
  public:
    using Source = std::function<std::array<uint8_t, 4>()>;

    void                   set(bool level) { level_ = level; }
    bool                   level() const { return level_; }
    void                   bind(Source read) { read_ = std::move(read); }
    std::array<uint8_t, 4> read() const { return read_(); }

  private:
    bool   level_ = false;
    Source read_;
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
    // The register ranges, served by read/write. A module with memory behind
    // it of its own also implements XipMemory.
    virtual std::vector<Range> ranges() const = 0;

    virtual uint32_t read(const std::string &range, uint32_t offset, uint32_t size)                  = 0;
    virtual void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) = 0;

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

    virtual Signal signal(const std::string &signal_name) {
        throw std::invalid_argument(name() + ": no signal " + signal_name);
    }

    // DMA engines hand out the request line for request number `n` (a later
    // call for the same number replaces the earlier line); other modules refuse.
    virtual std::shared_ptr<RequestLine> request_line(unsigned n) {
        throw std::invalid_argument(name() + ": not a DMA engine (asked for request line " + std::to_string(n) + ")");
    }

    // Interrupt controllers hand out the line driving source `bit`; other
    // modules refuse.
    virtual Line line(unsigned bit) {
        throw std::invalid_argument(name() + ": not an interrupt controller (asked for line " + std::to_string(bit) +
                                    ")");
    }
};

} // namespace bk
