#include "periph/flash.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "core/log.h"
#include "periph/crc16.h"

namespace bk {

namespace {

constexpr uint32_t kBlockRaw = 34, kBlockData = 32, kPage = 4096;
constexpr uint32_t kOperate = 0x00, kWriteData = 0x04, kReadData = 0x08, kId = 0x10, kSrCnt = 0x14, kConf = 0x1C;
constexpr uint32_t kStart = 1u << 29, kWp = 1u << 30, kBusy = 1u << 31;
constexpr uint32_t kBlockProtect = 0x1C;
constexpr size_t   kFifoWords    = 8;

enum Opcode : uint32_t {
    kRdid  = 20,
    kRead  = 5,
    kRdsr  = 3,
    kRdsr2 = 6,
    kWren  = 1,
    kWrdi  = 2,
    kWrsr  = 4,
    kWrsr2 = 7,
    kPp    = 12,
    kSe    = 13,
    kBe1   = 14,
    kBe2   = 15,
    kCe    = 16
};

const char *opcode_name(uint32_t op) {
    switch (op) {
        case kWrsr: return "WRSR";
        case kWrsr2: return "WRSR2";
        case kPp: return "PP";
        case kSe: return "SE";
        case kBe1: return "BE1";
        case kBe2: return "BE2";
        case kCe: return "CE";
        default: return "?";
    }
}

std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *fmt, ...) {
    char    buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

Logger &log() { return Logger::get("flash"); }

} // namespace

Flash::Flash(std::string name, uint32_t size, uint32_t jedec)
    : name_(std::move(name)), size_(size), jedec_(jedec), blocks_(size / kBlockRaw),
      window_((blocks_ * kBlockData + kPage - 1) / kPage * kPage), raw_(size, 0xFF), verdicts_(blocks_) {}

bool Flash::block_valid(uint32_t n) {
    std::optional<bool> &v = verdicts_[n];
    if (!v) {
        const uint8_t *b      = raw_.data() + static_cast<size_t>(n) * kBlockRaw;
        uint16_t       stored = static_cast<uint16_t>(b[kBlockData] << 8 | b[kBlockData + 1]);
        v                     = crc16(std::span<const uint8_t>(b, kBlockData)) == stored;
    }
    return *v;
}

bool Flash::readable(uint32_t n) {
    if (n >= blocks_) return false;
    return !crc_en() || block_valid(n);
}

void Flash::invalidate_window() {
    if (invalidate) invalidate("xip", 0, window_);
}

void Flash::set_conf(uint32_t value) {
    bool changed = ((conf_ ^ value) & kCrcEn) != 0;
    conf_        = value;
    if (changed) invalidate_window();
}

uint32_t Flash::read(const std::string &range, uint32_t offset, uint32_t size) {
    if (range == "xip") {
        uint32_t value = 0;
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t l = offset + i;
            uint32_t n = l / kBlockData;
            if (!readable(n)) {
                uint64_t physical = static_cast<uint64_t>(n) * kBlockRaw + l % kBlockData;
                if (last_alert_block_ != n) {
                    log().log(Level::Error,
                              format("ALERT: XIP read of an unreadable block (bad CRC or past the end of "
                                     "flash): logical 0x%08X, block %d, physical 0x%06llX",
                                     l,
                                     static_cast<int>(n),
                                     static_cast<unsigned long long>(physical)));
                    last_alert_block_ = n;
                }
                throw Fault{Fault::Kind::Dabt, l};
            }
            value |= static_cast<uint32_t>(raw_[static_cast<size_t>(n) * kBlockRaw + l % kBlockData]) << (8 * i);
        }
        return value;
    }
    switch (offset) {
        case kOperate: return operate_ & ~(kBusy | kStart);
        case kReadData: {
            uint32_t v = read_fifo_[read_ptr_];
            read_ptr_  = (read_ptr_ + 1) % kFifoWords;
            return v;
        }
        case kId: return jedec_;
        case kSrCnt: return sr_out_ | read_ptr_ << 16 | 7u << 19 | m_value_ << 22;
        case kConf: return conf_;
        default: return 0;
    }
}

void Flash::write(const std::string &range, uint32_t offset, uint32_t, uint32_t value) {
    if (range != "ctrl") return;
    switch (offset) {
        case kOperate:
            operate_ = value;
            if (value & kStart) run((value >> 24) & 0x1F, value & 0xFFFFFF, (value & kWp) != 0);
            break;
        case kWriteData:
            write_fifo_.push_back(value);
            if (write_fifo_.size() > kFifoWords) write_fifo_.pop_front();
            break;
        case kSrCnt: m_value_ = (value >> 22) & 0xFF; break;
        case kConf: set_conf(value); break;
        default: break;
    }
}

void Flash::run(uint32_t opcode, uint32_t addr, bool wp) {
    switch (opcode) {
        case kRdid: break;
        case kRead: {
            uint32_t start = addr & ~(kBlockData - 1);
            for (uint32_t w = 0; w < kFifoWords; ++w) {
                uint32_t word = 0;
                for (uint32_t b = 0; b < 4; ++b) {
                    uint32_t a = start + 4 * w + b;
                    word |= static_cast<uint32_t>(a < size_ ? raw_[a] : 0xFF) << (8 * b);
                }
                read_fifo_[w] = word;
            }
            read_ptr_ = 0;
            break;
        }
        case kRdsr: sr_out_ = (sr_ & 0xFC) | (wel_ ? 2u : 0u); break;
        case kRdsr2: sr_out_ = sr2_; break;
        case kWren:
        case kWrdi:
            wel_ = opcode == kWren;
            log().log(Level::Debug, wel_ ? "write enable set" : "write enable cleared");
            break;
        case kWrsr:
        case kWrsr2:
        case kPp:
        case kSe:
        case kBe1:
        case kBe2:
        case kCe: write_command(opcode, addr, wp); break;
        default: break;
    }
}

void Flash::write_command(uint32_t opcode, uint32_t addr, bool wp) {
    const char *name = opcode_name(opcode);
    if (!wp) {
        log().log(Level::Warning,
                  format("flash %s at 0x%06X ignored: WP# not released (operate_sw.wp = 0)", name, addr));
        return;
    }
    bool protect = (sr_ & kBlockProtect) != 0;
    if (opcode == kPp && protect) {
        log().log(Level::Warning,
                  format("flash PP at 0x%06X suppressed: block protection is set in the status "
                         "register",
                         addr));
        return;
    }
    // The controller issues its own write enable, so an explicit WREN is consumed.
    if (wel_) log().log(Level::Debug, "write enable consumed by " + std::string(name));
    wel_ = false;
    if (opcode == kWrsr || opcode == kWrsr2) {
        uint32_t data = (conf_ >> 10) & 0xFFFF;
        sr_           = data & 0xFC;
        if (opcode == kWrsr2) sr2_ = data >> 8;
        log().log(Level::Debug, format("%s: status 0x%02X, status2 0x%02X", name, sr_, sr2_));
        return;
    }
    if (opcode == kPp) {
        program(addr);
        return;
    }
    if (protect) {
        log().log(Level::Warning,
                  format("flash %s at 0x%06X ignored: block protection is set in the status register", name, addr));
        return;
    }
    erase(opcode, addr);
}

void Flash::program(uint32_t addr) {
    std::array<uint8_t, kBlockData> data;
    data.fill(0xFF);
    for (size_t w = 0; w < write_fifo_.size(); ++w)
        for (size_t b = 0; b < 4; ++b) data[4 * w + b] = static_cast<uint8_t>(write_fifo_[w] >> (8 * b));
    write_fifo_.clear();
    uint64_t start = addr & ~(kBlockData - 1);
    uint64_t end   = std::min<uint64_t>(start + kBlockData, size_);
    log().log(Level::Debug, format("PP at 0x%06X", static_cast<uint32_t>(start)));
    if (start >= end) return;
    for (uint64_t i = start; i < end; ++i) raw_[i] &= data[i - start];
    touched(static_cast<uint32_t>(start), static_cast<uint32_t>(end));
}

void Flash::erase(uint32_t opcode, uint32_t addr) {
    uint64_t start = 0, end = size_;
    if (opcode != kCe) {
        uint32_t span = opcode == kSe ? 0x1000 : opcode == kBe1 ? 0x8000 : 0x10000;
        start         = addr & ~(span - 1);
        end           = std::min<uint64_t>(start + span, size_);
    }
    log().log(Level::Debug,
              format("%s at 0x%06X: erasing 0x%06X..0x%06X",
                     opcode_name(opcode),
                     addr,
                     static_cast<uint32_t>(start),
                     static_cast<uint32_t>(end)));
    if (start >= end) return;
    std::fill(raw_.begin() + static_cast<std::ptrdiff_t>(start), raw_.begin() + static_cast<std::ptrdiff_t>(end), 0xFF);
    touched(static_cast<uint32_t>(start), static_cast<uint32_t>(end));
}

void Flash::touched(uint32_t start, uint32_t end) {
    uint32_t first = start / kBlockRaw, last = (end - 1) / kBlockRaw;
    for (uint32_t n = first; n <= last && n < blocks_; ++n) verdicts_[n].reset();
    uint64_t lo = static_cast<uint64_t>(first) * kBlockData;
    uint64_t hi = std::min<uint64_t>(static_cast<uint64_t>(last + 1) * kBlockData, window_);
    if (invalidate && lo < hi) invalidate("xip", static_cast<uint32_t>(lo), static_cast<uint32_t>(hi - lo));
}

Page Flash::page(const std::string &range, uint32_t page_offset) {
    if (range != "xip") return Module::page(range, page_offset);
    Page p{};
    for (uint32_t pos = 0; pos < kPage; pos += kBlockData) {
        uint32_t n = (page_offset + pos) / kBlockData;
        if (readable(n)) {
            std::copy_n(raw_.begin() + static_cast<std::ptrdiff_t>(n) * kBlockRaw, kBlockData, p.data.begin() + pos);
        } else {
            p.bad.push_back({pos, pos + kBlockData});
        }
    }
    return p;
}

void Flash::reset() {
    operate_ = m_value_ = 0;
    sr_ = sr2_ = sr_out_ = 0;
    wel_                 = false;
    read_fifo_           = {};
    read_ptr_            = 0;
    write_fifo_.clear();
    set_conf(kCrcEn);
}

void Flash::check_load(uint32_t offset, uint32_t size) const {
    if (offset % kBlockRaw != 0)
        throw std::invalid_argument(
            format("flash load of %u bytes at 0x%X must start on a 34-byte block boundary", size, offset));
    if (static_cast<uint64_t>(offset) + size > size_)
        throw std::invalid_argument(
            format("flash load of %u bytes at 0x%X does not fit the %u byte flash", size, offset, size_));
}

void Flash::load(uint32_t offset, std::span<const uint8_t> data) {
    // Compare in 64 bits before narrowing: a size past 4 GiB must not wrap.
    const uint64_t room = offset < raw_.size() ? raw_.size() - offset : 0;
    if (data.size() > room)
        throw std::invalid_argument(format("flash load of %llu bytes at 0x%X does not fit the %u byte flash",
                                           static_cast<unsigned long long>(data.size()),
                                           offset,
                                           size_));
    uint32_t size = static_cast<uint32_t>(data.size());
    check_load(offset, size);
    std::copy(data.begin(), data.end(), raw_.begin() + offset);
    // Only blocks covered completely lose their cached verdict.
    uint32_t end = std::min((offset + size) / kBlockRaw, blocks_);
    for (uint32_t n = offset / kBlockRaw; n < end; ++n) verdicts_[n].reset();
    invalidate_window();
}

} // namespace bk
