#include "periph/wifi/frame_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kThdMarker = 0xCAFEBABE, kTbdMarker = 0xCAFEFADE, kRhdMarker = 0xBAADF00D, kRbdMarker = 0xC0DEDBAD;
constexpr uint32_t kThdSize = 68, kTbdSize = 20, kRhdSize = 68, kRbdSize = 20;
constexpr uint32_t kMaxThds = 64, kMaxTbds = 32, kMaxBufferSize = 4096, kMaxQueued = 64;

constexpr uint32_t kThdControl = 56, kThdStatus = 60, kThdAirTime = 64;
constexpr uint32_t kThdTrigger = 1u << 8;
constexpr uint32_t kTxDone = 0x80000000, kTxOk = 0x00800000;

constexpr uint32_t kRxDone = 0x4000, kRxSuccess = 0x2000, kRxGroup = 0x0400, kRxKeyValid = 0x02000000;
constexpr uint32_t kRbdDone = 0x0002, kRbdLast = 0x0001;
constexpr uint32_t kRxBits = (1u << 16) | (1u << 17) | (1u << 19);

Logger &log() { return Logger::get("wifi.frame_engine"); }

std::string format(const char *text, uint32_t address) {
    char buf[96];
    std::snprintf(buf, sizeof buf, text, address);
    return buf;
}

uint32_t word_at(const std::vector<uint8_t> &bytes, uint32_t index) {
    uint32_t v;
    std::memcpy(&v, bytes.data() + 4 * index, 4);
    return v; // the host is little-endian, like the guest
}

void put_word(std::vector<uint8_t> &bytes, uint32_t index, uint32_t value) {
    std::memcpy(bytes.data() + 4 * index, &value, 4);
}

} // namespace

WifiFrameEngine::WifiFrameEngine(BusMemory &memory, Transmit transmit, Raise raise)
    : memory_(memory), transmit_(std::move(transmit)), raise_(std::move(raise)) {}

void WifiFrameEngine::reset() {
    last_thd_.fill(0);
    completions_.clear();
    header_head_ = payload_head_ = 0;
    last_rhd_ = last_rbd_ = 0;
    queue_.clear();
}

std::optional<std::vector<uint8_t>> WifiFrameEngine::read(uint32_t addr, uint32_t size) const {
    try {
        return memory_.read(addr, size);
    } catch (const BusError &) {
        return std::nullopt;
    }
}

std::optional<uint32_t> WifiFrameEngine::read_word(uint32_t addr) const {
    std::optional<std::vector<uint8_t>> bytes = read(addr, 4);
    if (!bytes) return std::nullopt;
    return word_at(*bytes, 0);
}

void WifiFrameEngine::write(uint32_t addr, std::span<const uint8_t> data) const {
    try {
        memory_.write(addr, data);
    } catch (const BusError &) {
        log().log(Level::Warning, format("cannot write the descriptor or buffer at 0x%08X", addr));
    }
}

void WifiFrameEngine::write_word(uint32_t addr, uint32_t value) const {
    std::vector<uint8_t> bytes(4);
    put_word(bytes, 0, value);
    write(addr, bytes);
}

void WifiFrameEngine::transmit(uint32_t queue, uint32_t head, uint64_t now_ns) {
    if (queue >= kQueues) return;
    uint32_t addr = head;
    if (addr == 0) {
        if (last_thd_[queue] == 0) return;
        // The firmware links new descriptors behind the last one it kicked; a
        // chain that has not been extended yet has nothing to send.
        std::optional<uint32_t> next = read_word(last_thd_[queue] + 4);
        if (!next || *next == 0) return;
        addr = *next;
    }

    for (uint32_t n = 0; n < kMaxThds; ++n) {
        std::optional<std::vector<uint8_t>> thd = read(addr, kThdSize);
        if (!thd || word_at(*thd, 0) != kThdMarker) {
            log().log(Level::Warning, format("transmit descriptor at 0x%08X is not valid", addr));
            return;
        }

        std::vector<uint8_t> data;
        auto                 gather = [&](uint32_t start, uint32_t end) {
            const int32_t span = static_cast<int32_t>(end - start);
            if (span < 0 || span >= static_cast<int32_t>(kMaxBufferSize)) return;
            if (std::optional<std::vector<uint8_t>> bytes = read(start, static_cast<uint32_t>(span) + 1))
                data.insert(data.end(), bytes->begin(), bytes->end());
        };

        const uint32_t start = word_at(*thd, 4), end = word_at(*thd, 5);
        if (start != 0) gather(start, end);

        uint32_t tbd_addr = word_at(*thd, 3);
        for (uint32_t t = 0; t < kMaxTbds && tbd_addr != 0; ++t) {
            std::optional<std::vector<uint8_t>> tbd = read(tbd_addr, kTbdSize);
            if (!tbd || word_at(*tbd, 0) != kTbdMarker) break;
            const int32_t span = static_cast<int32_t>(word_at(*tbd, 3) - word_at(*tbd, 2));
            if (span >= 0 && span < static_cast<int32_t>(kMaxBufferSize)) {
                gather(word_at(*tbd, 2), word_at(*tbd, 3));
                write_word(tbd_addr + 16, word_at(*tbd, 4) | kTxDone);
            }
            tbd_addr = word_at(*tbd, 1);
        }

        // The payload may still be downloading; the firmware kicks again.
        if (data.empty()) return;

        const uint32_t length = word_at(*thd, 6);
        if (length > 4) data.resize(std::min<size_t>(data.size(), length - 4));
        transmit_(data);
        completions_.push_back({now_ns + kAirTimeUs * 1000, queue, addr});
        last_thd_[queue] = addr;

        addr = word_at(*thd, 1);
        if (addr == 0) return;
    }
}

void WifiFrameEngine::tick(uint64_t now_ns) {
    auto due = std::stable_partition(
        completions_.begin(), completions_.end(), [now_ns](const Completion &c) { return c.due_ns > now_ns; });
    const std::vector<Completion> done(due, completions_.end());
    completions_.erase(due, completions_.end());

    for (const Completion &c : done) {
        std::optional<std::vector<uint8_t>> tail = read(c.thd + kThdControl, 12);
        if (!tail) {
            log().log(Level::Warning, format("cannot write the descriptor or buffer at 0x%08X", c.thd));
            continue;
        }
        std::vector<uint8_t> words(8);
        put_word(words, 0, word_at(*tail, 1) | kTxDone | kTxOk);
        put_word(words, 1, static_cast<uint32_t>(kAirTimeUs));
        write(c.thd + kThdStatus, words);
        if (word_at(*tail, 0) & kThdTrigger) raise_(1u << (6 + c.queue));
    }
}

std::optional<uint64_t> WifiFrameEngine::next_event_ns() const {
    std::optional<uint64_t> earliest;
    for (const Completion &c : completions_)
        if (!earliest || c.due_ns < *earliest) earliest = c.due_ns;
    return earliest;
}

void WifiFrameEngine::set_rx_heads(std::optional<uint32_t> header, std::optional<uint32_t> payload) {
    if (header) header_head_ = *header & ~3u;
    if (payload) payload_head_ = *payload & ~3u;
    last_rhd_ = last_rbd_ = 0;
    flush();
}

void WifiFrameEngine::receive(std::span<const uint8_t> frame, uint64_t tsf, int rssi, std::optional<uint32_t> key) {
    queue_.push_back({
        {frame.begin(), frame.end()},
        tsf, rssi, key
    });
    while (queue_.size() > kMaxQueued) queue_.pop_front();
    flush();
}

void WifiFrameEngine::flush() {
    bool delivered = false;
    while (!queue_.empty() && deliver(queue_.front())) {
        queue_.pop_front();
        delivered = true;
    }
    if (delivered) raise_(kRxBits);
}

bool WifiFrameEngine::deliver(const Received &rx) {
    uint32_t rhd_addr = header_head_;
    if (last_rhd_ != 0) {
        std::optional<uint32_t> next = read_word(last_rhd_ + 4);
        if (!next) return false;
        rhd_addr = *next;
    }
    std::optional<std::vector<uint8_t>> rhd = read(rhd_addr, kRhdSize);
    if (!rhd || word_at(*rhd, 0) != kRhdMarker || (word_at(*rhd, 16) & kRxDone)) return false;

    struct Buffer {
        uint32_t addr, start, capacity;
        uint32_t status;
    };
    std::vector<Buffer> buffers;
    const size_t        length = rx.frame.size();
    size_t              room   = 0;
    uint32_t            next   = payload_head_;
    if (last_rbd_ != 0) {
        std::optional<uint32_t> n = read_word(last_rbd_ + 4);
        if (!n) return false;
        next = *n;
    }
    while (room < length) {
        std::optional<std::vector<uint8_t>> rbd = read(next, kRbdSize);
        if (!rbd || word_at(*rbd, 0) != kRbdMarker) return false;
        const uint32_t start = word_at(*rbd, 2), end = word_at(*rbd, 3);
        if (end < start) return false;
        const uint32_t capacity = end - start + 1;
        buffers.push_back({next, start, capacity, word_at(*rbd, 4)});
        room += capacity;
        next = word_at(*rbd, 1);
    }

    size_t offset = 0;
    for (size_t i = 0; i < buffers.size(); ++i) {
        const Buffer &b    = buffers[i];
        const size_t  take = std::min<size_t>(b.capacity, length - offset);
        if (take > 0) write(b.start, std::span<const uint8_t>(rx.frame).subspan(offset, take));
        offset += take;
        const bool last = i + 1 == buffers.size();
        write_word(b.addr + 16, (b.status & 0xFFFF0000u) | kRbdDone | (last ? kRbdLast : 0));
    }

    uint32_t status = kRxDone | kRxSuccess;
    if (length > 4 && (rx.frame[4] & 1)) status |= kRxGroup;
    if (rx.key) status |= kRxKeyValid | (*rx.key & 0x3FF) << 15;

    const uint32_t       rssi = static_cast<uint32_t>(rx.rssi) & 0xFF;
    std::vector<uint8_t> out(kRhdSize);
    put_word(out, 0, kRhdMarker);
    put_word(out, 1, word_at(*rhd, 1));
    put_word(out, 2, buffers.empty() ? 0 : buffers.front().addr);
    for (uint32_t i = 3; i <= 6; ++i) put_word(out, i, word_at(*rhd, i));
    put_word(out, 7, static_cast<uint32_t>(length));
    put_word(out, 8, static_cast<uint32_t>(rx.tsf));
    put_word(out, 9, static_cast<uint32_t>(rx.tsf >> 32));
    put_word(out, 10, static_cast<uint32_t>(length + 4) & 0xFFF);
    put_word(out, 12, rssi | rssi << 24);
    put_word(out, 16, status);
    write(rhd_addr, out);

    last_rhd_ = rhd_addr;
    if (!buffers.empty()) last_rbd_ = buffers.back().addr;
    return true;
}

} // namespace bk
