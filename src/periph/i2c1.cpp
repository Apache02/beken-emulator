#include "periph/i2c1.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kConfig = 0x00, kData = 0x04;
constexpr uint32_t kEnable = 1u << 0, kStart = 1u << 1, kStop = 1u << 2, kTxAck = 1u << 3, kTxMode = 1u << 4;
constexpr unsigned kClkDivShift = 6;
constexpr uint32_t kClkDivMask  = 0x3FF;
constexpr uint32_t kIntFlag = 1u << 16, kRxAck = 1u << 17, kAckReq = 1u << 18, kBusy = 1u << 19;
constexpr uint32_t kWritable = kEnable | kStart | kTxAck | kTxMode | (kClkDivMask << kClkDivShift) | kIntFlag;

Logger &log() { return Logger::get("i2c"); }

std::string describe(uint32_t value) {
    std::string out;
    for (auto [name, bit] : {
             std::pair{"enable",   kEnable },
             std::pair{"start",    kStart  },
             std::pair{"stop",     kStop   },
             std::pair{"tx_ack",   kTxAck  },
             std::pair{"tx_mode",  kTxMode },
             std::pair{"int_flag", kIntFlag}
    }) {
        if (value & bit) out += std::string(name) + " ";
    }
    return out + "clk_div=" + std::to_string((value >> kClkDivShift) & kClkDivMask);
}

std::string hex(const char *format, uint32_t value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, format, value);
    return buf;
}

} // namespace

I2c1::I2c1(std::string name, const Clock &clock, Line irq, uint32_t ref_hz)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)), ref_hz_(ref_hz) {}

void I2c1::attach(I2cDevice *device) {
    auto [it, added] = devices_.emplace(device->address(), device);
    if (!added)
        throw std::invalid_argument(name_ + " address " + hex("0x%02X", device->address()) + " is used twice (" +
                                    it->second->type() + " and " + device->type() + ")");
}

void I2c1::reset() {
    config_ = 0;
    data_   = 0;
    rx_ack_ = ack_req_ = busy_ = false;
    phase_                     = Phase::Idle;
    device_                    = nullptr;
    event_.reset();
    level_ = false;
    drive();
}

uint32_t I2c1::read(const std::string &, uint32_t offset, uint32_t) {
    if (offset == kData) return data_;
    return config_ | (rx_ack_ ? kRxAck : 0) | (ack_req_ ? kAckReq : 0) | (busy_ ? kBusy : 0);
}

void I2c1::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    if (offset == kData)
        data_ = value & 0xFF;
    else if (offset == kConfig)
        write_config(value);
}

std::optional<uint64_t> I2c1::next_event_ns() const {
    if (!event_) return std::nullopt;
    return event_->due_ns;
}

void I2c1::tick(uint64_t now_ns) {
    if (event_ && now_ns >= event_->due_ns) {
        Kind kind = event_->kind;
        event_.reset();
        finish(kind);
        drive();
    }
}

void I2c1::write_config(uint32_t value) {
    const uint32_t previous = config_;
    config_                 = value & kWritable;
    log().log(Level::Debug, "config <= " + hex("0x%08X", value) + " (" + describe(value) + ")");
    if (!(config_ & kEnable)) {
        abort();
        config_ &= ~kIntFlag;
        drive();
        return;
    }
    if ((value & kStop) && phase_ != Phase::Idle) {
        stop();
    } else if ((value & kStart) && !(previous & kStart)) {
        start();
    } else if ((previous & kIntFlag) && !(config_ & kIntFlag) && phase_ != Phase::Idle) {
        ack_req_ = false;
        proceed();
    }
    drive();
}

uint64_t I2c1::byte_ns() const {
    const uint64_t divider = (config_ >> kClkDivShift) & kClkDivMask;
    return std::max<uint64_t>(9 * (6 + 3 * (divider + 1)) * 1'000'000'000ULL / ref_hz_, 1);
}

void I2c1::schedule(Kind kind, uint64_t duration_ns) { event_ = Event{clock_.now_ns() + duration_ns, kind}; }

void I2c1::start() {
    busy_  = true;
    phase_ = Phase::Idle;
    schedule(Kind::Address, byte_ns());
}

void I2c1::stop() {
    if (device_) {
        device_->stop();
        device_ = nullptr;
    }
    phase_ = Phase::Idle;
    config_ &= ~kIntFlag;
    ack_req_ = false;
    schedule(Kind::Stop, byte_ns() / 4);
}

void I2c1::abort() {
    if (device_) device_->stop();
    device_ = nullptr;
    phase_  = Phase::Idle;
    event_.reset();
    rx_ack_ = ack_req_ = busy_ = false;
}

// Software cleared `int_flag`: the next phase starts.
void I2c1::proceed() {
    if (config_ & kTxMode)
        schedule(Kind::Send, byte_ns());
    else if (phase_ == Phase::Addressed || (config_ & kTxAck))
        schedule(Kind::Receive, byte_ns());
}

// What happens when a bus phase ends.
void I2c1::finish(Kind kind) {
    switch (kind) {
        case Kind::Address: {
            const uint32_t address = data_ >> 1;
            const bool     read    = data_ & 1;
            auto           it      = devices_.find(static_cast<uint8_t>(address));
            device_                = address < 0x80 && it != devices_.end() ? it->second : nullptr;
            rx_ack_                = device_ && device_->begin(read);
            log().log(Level::Debug,
                      "address " + hex("0x%02X", address) + (read ? " read: " : " write: ") +
                          (rx_ack_ ? "ack" : "nack"));
            if (!rx_ack_) device_ = nullptr;
            phase_ = Phase::Addressed;
            config_ |= kIntFlag;
            break;
        }
        case Kind::Send:
            rx_ack_ = device_ && device_->write(static_cast<uint8_t>(data_));
            log().log(Level::Debug, "write " + hex("0x%02X", data_) + (rx_ack_ ? ": ack" : ": nack"));
            phase_ = Phase::Data;
            config_ |= kIntFlag;
            break;
        case Kind::Receive:
            data_ = device_ ? device_->read() : 0xFF;
            log().log(Level::Debug, "read " + hex("0x%02X", data_));
            ack_req_ = true;
            phase_   = Phase::Data;
            config_ |= kIntFlag;
            break;
        case Kind::Stop: busy_ = false; break;
    }
}

void I2c1::drive() {
    const bool level = (config_ & kIntFlag) != 0;
    if (level != level_) {
        level_ = level;
        log().log(Level::Debug, std::string("interrupt line ") + (level ? "high" : "low"));
    }
    if (irq_) irq_(level);
}

} // namespace bk
