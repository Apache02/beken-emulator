#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/module.h"
#include "periph/unknown.h"
#include "periph/wifi/air.h"
#include "periph/wifi/frame_engine.h"

namespace bk {

// The WiFi MAC (core bank 0xC0000000, platform bank 0xC0008000): the registers
// the firmware uses to run the MAC state machine, its interrupts and timers,
// and the DMA control command that starts the frame engine. Registers that only store a value are stand-ins
// (plain memory that logs its accesses, see Unknown); the ones with behaviour
// are served here:
//
// - counters 0x11C / 0x120 / 0x124 and the TSF 0x0A4 / 0x0A8, which count from
//   0 while the MAC is powered (the `mac_powered` signal, always powered
//   without one) and read 0 while it is powered down;
// - the state machine 0x038, the version 0x004, error recovery 0x054 and doze
//   control 0x048, with TBTT detection in tick();
// - general and TX/RX interrupts of the platform bank, driving six ICU lines;
// - the ten ABS timers, the event registers and the key storage RAM;
// - time on air (platform 0x160..0x168) and the self-clearing soft reset.
class WifiMac : public Module {
  public:
    static constexpr uint32_t kCoreSize = 0x1580, kPlSize = 0x57C;

    // Interrupt lines; any of them may be empty.
    struct Lines {
        Line tx_rx_timer, tx_rx_misc, rx_trigger, tx_trigger, prot_trigger, general;
    };

    // `powered` may be empty. `instruction` and `bank_log_level` (the level of
    // their access log lines) are passed to the stand-in banks. `memory` is the
    // bus master the frame engine reads descriptors through.
    WifiMac(std::string                name,
            const Clock               &clock,
            Signal                     powered,
            Lines                      lines,
            Unknown::InstructionSource instruction,
            BusMemory                 &memory,
            Level                      bank_log_level = Level::Info);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"core", kCoreSize},
            {"pl",   kPlSize  }
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    // Notices power changes (the counters restart on every power-up), fires
    // ABS timers, detects TBTT and drives the interrupt lines.
    void tick(uint64_t now_ns) override;
    // The earliest of the frame engine, the air, the armed ABS timers and the
    // next TBTT.
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;

    // The 48-bit microsecond counter, or nothing while powered down.
    std::optional<uint64_t> microseconds();

    // Connects the far end of the air (not owned); null disconnects it. With
    // no air, transmitted frames are dropped.
    void attach(WifiAir *air);
    // A frame from the air; ignored unless the MAC state is ACTIVE.
    void receive(std::span<const uint8_t> frame);

    // For the frame engine: sets TX/RX interrupt bits and updates the lines.
    void raise_txrx(uint32_t bits);

    // The key storage index of the entry whose address is the transmitter
    // address of `frame` (bytes 10..15), looked up in the range of core 0x0D8.
    std::optional<uint32_t> station_key_index(std::span<const uint8_t> frame) const;

  private:
    struct KeyEntry {
        uint32_t                index;
        uint64_t                address;
        std::array<uint32_t, 4> words;
        uint32_t                attributes;
    };

    std::optional<uint64_t> elapsed_ns();
    std::optional<uint64_t> peek_microseconds() const;
    uint32_t                counter(uint32_t word);
    void                    software_update(uint32_t value);

    uint32_t core_write_rule(uint32_t word, uint32_t value);
    uint32_t pl_write_rule(uint32_t word, uint32_t value);
    void     refresh_core(uint32_t word);
    void     refresh_pl(uint32_t word);

    uint64_t tsf();
    void     write_tsf(bool high, uint32_t value);
    void     arm_timer(uint32_t index, uint32_t target);
    void     fire_timers();
    void     detect_tbtt();
    uint64_t beacon_period_us() const;
    bool     beacons_active() const { return state_ == kActive || state_ == kDoze; }
    uint32_t key_command(uint32_t value);
    uint32_t time_on_air(uint32_t value);
    void     dma_command(uint32_t value);
    void     update_lines();

    static constexpr uint32_t kIdle = 0, kDoze = 2, kActive = 3;

    std::string             name_;
    const Clock            &clock_;
    Signal                  powered_;
    Lines                   lines_;
    Unknown                 core_, pl_;
    std::optional<uint64_t> start_;
    uint64_t                offset_us_  = 0;
    uint32_t                lo_written_ = 0;

    uint32_t                 state_ = kIdle, next_state_ = kIdle;
    uint32_t                 general_ = 0, txrx_ = 0, events_ = 0;
    std::array<uint32_t, 10> targets_{};
    uint32_t                 armed_      = 0;
    uint64_t                 tsf_offset_ = 0;
    std::optional<uint64_t>  last_beacon_;
    std::vector<KeyEntry>    keys_;
    bool                     soft_reset_pending_ = false;
    // Last level written to each ICU line (-1: never written since reset).
    std::array<int, 6> levels_{};

    WifiAir        *air_ = nullptr;
    WifiFrameEngine engine_;
};

} // namespace bk
