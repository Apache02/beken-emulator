#include "periph/i2c2.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kConfig = 0x00, kStatus = 0x04, kData = 0x08;

constexpr unsigned kClkDivShift = 6;
constexpr uint32_t kClkDivMask  = 0x3FF;
constexpr uint32_t kEnable      = 1u << 31;

constexpr uint32_t kIntFlag = 1u << 0, kRxFifoEmpty = 1u << 4, kAck = 1u << 8, kStop = 1u << 9, kStart = 1u << 10,
                   kAckReq = 1u << 12, kTxMode = 1u << 13, kMaster = 1u << 14, kBusy = 1u << 15;
constexpr unsigned kIntModeShift = 6;
constexpr uint32_t kIntModeMask  = 0x3;
// `stop` is not stored: it is a one-shot command, as in I2c1.
constexpr uint32_t kWritable = kIntFlag | (kIntModeMask << kIntModeShift) | kAck | kStart;

Logger &log() { return Logger::get("i2c2"); }

std::string describe(uint32_t value) {
    std::string out;
    for (auto [name, bit] : {
             std::pair{"int_flag", kIntFlag},
             std::pair{"ack",      kAck    },
             std::pair{"stop",     kStop   },
             std::pair{"start",    kStart  }
    }) {
        if (value & bit) out += std::string(name) + " ";
    }
    return out + "int_mode=" + std::to_string((value >> kIntModeShift) & kIntModeMask);
}

std::string hex(const char *format, uint32_t value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, format, value);
    return buf;
}

} // namespace

I2c2::I2c2(std::string name, const Clock &clock, Line irq, uint32_t ref_hz)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)), ref_hz_(ref_hz) {}

void I2c2::attach(I2cDevice *device) {
    auto [it, added] = devices_.emplace(device->address(), device);
    if (!added)
        throw std::invalid_argument(name_ + " address " + hex("0x%02X", device->address()) + " is used twice (" +
                                    it->second->type() + " and " + device->type() + ")");
}

void I2c2::reset() {
    config_ = status_ = data_ = 0;
    transmit_ = ack_req_ = busy_ = false;
    phase_                       = Phase::Idle;
    device_                      = nullptr;
    event_.reset();
    level_ = false;
    drive();
}

uint32_t I2c2::status() const {
    return status_ | kRxFifoEmpty | (transmit_ ? kTxMode : 0) | (ack_req_ ? kAckReq : 0) |
           (busy_ ? kMaster | kBusy : 0);
}

uint32_t I2c2::read(const std::string &, uint32_t offset, uint32_t) {
    switch (offset) {
        case kConfig: return config_;
        case kStatus: return status();
        case kData: return data_;
        default: return 0;
    }
}

void I2c2::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kConfig: write_config(value); break;
        case kStatus: write_status(value); break;
        case kData: data_ = value & 0xFF; break;
        default: break;
    }
}

std::optional<uint64_t> I2c2::next_event_ns() const {
    if (!event_) return std::nullopt;
    return event_->due_ns;
}

void I2c2::tick(uint64_t now_ns) {
    if (event_ && now_ns >= event_->due_ns) {
        Kind kind = event_->kind;
        event_.reset();
        finish(kind);
        drive();
    }
}

void I2c2::write_config(uint32_t value) {
    config_ = value;
    log().log(Level::Debug,
              "config <= " + hex("0x%08X", value) + " (" + (value & kEnable ? "enable " : "") +
                  "clk_div=" + std::to_string((value >> kClkDivShift) & kClkDivMask) + ")");
    // Switching the engine off and on again is how software recovers it: off,
    // it holds nothing of the transfer.
    if (!(config_ & kEnable)) {
        abort();
        status_ = 0;
        drive();
    }
}

void I2c2::write_status(uint32_t value) {
    const uint32_t previous = status_;
    log().log(Level::Debug, "status <= " + hex("0x%08X", value) + " (" + describe(value) + ")");
    // The engine is off: the word does not take the write.
    if (!(config_ & kEnable)) return;
    status_ = value & kWritable;
    if ((value & kStop) && phase_ != Phase::Idle) {
        stop();
    } else if ((value & kStart) && !(previous & kStart)) {
        start();
    } else if ((previous & kIntFlag) && !(status_ & kIntFlag) && phase_ != Phase::Idle) {
        ack_req_ = false;
        proceed();
    }
    drive();
}

uint64_t I2c2::byte_ns() const {
    const uint64_t divider = (config_ >> kClkDivShift) & kClkDivMask;
    return std::max<uint64_t>(9 * (6 + 3 * (divider + 1)) * 1'000'000'000ULL / ref_hz_, 1);
}

void I2c2::schedule(Kind kind, uint64_t duration_ns) { event_ = Event{clock_.now_ns() + duration_ns, kind}; }

void I2c2::start() {
    busy_  = true;
    phase_ = Phase::Idle;
    schedule(Kind::Address, byte_ns());
}

void I2c2::stop() {
    if (device_) {
        device_->stop();
        device_ = nullptr;
    }
    phase_ = Phase::Idle;
    status_ &= ~kIntFlag;
    ack_req_ = false;
    schedule(Kind::Stop, byte_ns() / 4);
}

void I2c2::abort() {
    if (device_) device_->stop();
    device_ = nullptr;
    phase_  = Phase::Idle;
    event_.reset();
    ack_req_ = busy_ = false;
}

// Software cleared `int_flag`: the next phase starts.
void I2c2::proceed() {
    if (transmit_)
        schedule(Kind::Send, byte_ns());
    else if (phase_ == Phase::Addressed || (status_ & kAck))
        schedule(Kind::Receive, byte_ns());
}

// What happens when a bus phase ends.
void I2c2::finish(Kind kind) {
    auto answer = [this](bool ack) { status_ = (status_ & ~kAck) | (ack ? kAck : 0); };
    switch (kind) {
        case Kind::Address: {
            const uint32_t address = data_ >> 1;
            const bool     read    = data_ & 1;
            auto           it      = devices_.find(static_cast<uint8_t>(address));
            device_                = address < 0x80 && it != devices_.end() ? it->second : nullptr;
            const bool ack         = device_ && device_->begin(read);
            log().log(Level::Debug,
                      "address " + hex("0x%02X", address) + (read ? " read: " : " write: ") + (ack ? "ack" : "nack"));
            if (!ack) device_ = nullptr;
            transmit_ = !read;
            answer(ack);
            phase_ = Phase::Addressed;
            status_ |= kIntFlag;
            break;
        }
        case Kind::Send: {
            const bool ack = device_ && device_->write(static_cast<uint8_t>(data_));
            log().log(Level::Debug, "write " + hex("0x%02X", data_) + (ack ? ": ack" : ": nack"));
            answer(ack);
            phase_ = Phase::Data;
            status_ |= kIntFlag;
            break;
        }
        case Kind::Receive:
            data_ = device_ ? device_->read() : 0xFF;
            log().log(Level::Debug, "read " + hex("0x%02X", data_));
            ack_req_ = true;
            phase_   = Phase::Data;
            status_ |= kIntFlag;
            break;
        case Kind::Stop: busy_ = false; break;
    }
}

void I2c2::drive() {
    const bool level = (status_ & kIntFlag) != 0;
    if (level != level_) {
        level_ = level;
        log().log(Level::Debug, std::string("interrupt line ") + (level ? "high" : "low"));
    }
    if (irq_) irq_(level);
}

} // namespace bk
