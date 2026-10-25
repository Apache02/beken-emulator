#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/log.h"
#include "periph/wifi/air.h"

namespace bk {

// A pcap file (or named pipe) of 802.11 frames with a radiotap header. Every
// record goes out in one write(), so a reader that follows the file or the
// pipe, such as `wireshark -k -i FILE`, sees it at once.
class PcapWriter {
  public:
    // LINKTYPE_IEEE802_11_RADIOTAP.
    static constexpr uint32_t kLinkType = 127;

    // Creates or truncates `path` and writes the file header. A named pipe
    // blocks here until a reader opens it. Throws std::runtime_error.
    explicit PcapWriter(const std::string &path);
    ~PcapWriter();
    PcapWriter(const PcapWriter &)            = delete;
    PcapWriter &operator=(const PcapWriter &) = delete;

    // One frame, without its FCS, stamped `ts_ns` (device time), with a radiotap
    // header that names `channel` (1-13) when known. False once a write has
    // failed (for a pipe: the reader went away); nothing more is written then.
    bool write(uint64_t ts_ns, std::span<const uint8_t> frame, std::optional<unsigned> channel);
    bool ok() const { return ok_; }

  private:
    bool put(const std::vector<uint8_t> &bytes);

    int  fd_ = -1;
    bool ok_ = true;
};

// A WifiAir that passes everything to another and records the frames that go
// either way: those the firmware transmits and those handed back to it.
class CapturingAir : public WifiAir {
  public:
    using Channel = std::function<std::optional<unsigned>()>;

    // `channel`, when set, tells the channel the firmware's radio is on.
    CapturingAir(WifiAir &inner, PcapWriter &pcap, Logger &log, Channel channel = {})
        : inner_(inner), pcap_(pcap), log_(log), channel_(std::move(channel)) {}

    void attach(Deliver deliver) override;
    void transmit(std::span<const uint8_t> frame) override;
    void tick(uint64_t now_ns) override {
        now_ = now_ns;
        inner_.tick(now_ns);
    }
    std::optional<uint64_t> next_event_ns() const override { return inner_.next_event_ns(); }
    void                    reset() override { inner_.reset(); }

  private:
    void record(std::span<const uint8_t> frame);

    WifiAir    &inner_;
    PcapWriter &pcap_;
    Logger     &log_;
    Channel     channel_;
    uint64_t    now_ = 0;
};

} // namespace bk
