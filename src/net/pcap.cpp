#include "net/pcap.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <unistd.h>

namespace bk {

namespace {

void put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}

constexpr uint32_t kMagic   = 0xa1b2c3d4; // microsecond timestamps
constexpr uint32_t kSnapLen = 65535;
// Radiotap: the Channel field, bit 3 of the present word.
constexpr uint32_t kRadiotapChannel = 1u << 3;
// Radiotap channel flag: 2 GHz spectrum.
constexpr uint16_t kChannel2Ghz = 0x0080;
// warn_once key.
constexpr uint64_t kWarnWrite = 0;

} // namespace

PcapWriter::PcapWriter(const std::string &path) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) throw std::runtime_error(std::format("cannot open {} for capture: {}", path, std::strerror(errno)));
    std::vector<uint8_t> header;
    put32(header, kMagic);
    put16(header, 2); // version 2.4
    put16(header, 4);
    put32(header, 0); // thiszone
    put32(header, 0); // sigfigs
    put32(header, kSnapLen);
    put32(header, kLinkType);
    if (!put(header)) {
        int err = errno;
        ::close(fd_);
        throw std::runtime_error(std::format("cannot write the capture to {}: {}", path, std::strerror(err)));
    }
}

PcapWriter::~PcapWriter() {
    if (fd_ >= 0) ::close(fd_);
}

bool PcapWriter::put(const std::vector<uint8_t> &bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = ::write(fd_, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}

bool PcapWriter::write(uint64_t ts_ns, std::span<const uint8_t> frame, std::optional<unsigned> channel) {
    if (!ok_) return false;
    std::vector<uint8_t> radiotap = {0, 0}; // version, pad
    put16(radiotap, channel ? 12 : 8);
    put32(radiotap, channel ? kRadiotapChannel : 0);
    if (channel) {
        put16(radiotap, uint16_t(2407 + 5 * *channel));
        put16(radiotap, kChannel2Ghz);
    }
    const uint32_t length = uint32_t(radiotap.size() + frame.size());

    std::vector<uint8_t> record;
    put32(record, uint32_t(ts_ns / 1'000'000'000));
    put32(record, uint32_t(ts_ns % 1'000'000'000 / 1000));
    put32(record, length);
    put32(record, length);
    record.insert(record.end(), radiotap.begin(), radiotap.end());
    record.insert(record.end(), frame.begin(), frame.end());
    ok_ = put(record);
    return ok_;
}

void CapturingAir::attach(Deliver deliver) {
    if (!deliver) {
        inner_.attach({});
        return;
    }
    inner_.attach([this, deliver = std::move(deliver)](std::vector<uint8_t> frame) {
        record(frame);
        deliver(std::move(frame));
    });
}

void CapturingAir::transmit(std::span<const uint8_t> frame) {
    record(frame);
    inner_.transmit(frame);
}

void CapturingAir::record(std::span<const uint8_t> frame) {
    if (!pcap_.ok()) return;
    if (!pcap_.write(now_, frame, channel_ ? channel_() : std::nullopt))
        log_.warn_once(kWarnWrite, "the capture stopped: cannot write to it any more");
}

} // namespace bk
