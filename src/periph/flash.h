#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/mapped_file.h"
#include "core/module.h"

namespace bk {

// SPI flash controller with an execute-in-place window.
//
// The physical store is a sequence of 34-byte blocks: 32 data bytes followed by
// a big-endian CRC-16 of them. The `xip` range is a decoded view that shows only
// the data bytes (32 per block); while CRC checking is enabled, blocks whose CRC
// does not match cannot be read through it. The `ctrl` range is the command
// interface: status registers, a read FIFO, a write FIFO, page program and
// erases. Every command completes instantly.
class Flash : public Module {
  public:
    static constexpr uint32_t kCrcEn = 1u << 26;

    // The controller's start state. The defaults are the reset state of the
    // design; a profile can give the state a running chip is measured in.
    struct Options {
        // Reset value of `config`.
        uint32_t config_reset = kCrcEn;
        // Reset value of the `m_value` field of `status`.
        uint32_t m_value_reset = 0;
        // Reset position (0-7) of the read FIFO pointer, the next word a read of
        // `data_flash_sw` returns and the field [18:16] of `status`.
        uint32_t read_ptr_reset = 0;
        // Start values of controller words that have no function in the model
        // (offset -> value): they read back what was written and return to
        // these at reset. Any other unused offset reads 0 and ignores writes.
        std::map<uint32_t, uint32_t> words;
    };

    // Throws std::invalid_argument when `options.words` names an offset that is
    // not a word-aligned plain offset of the `ctrl` range.
    Flash(std::string name, uint32_t size, uint32_t jedec, Options options);
    explicit Flash(std::string name, uint32_t size = 0x200000, uint32_t jedec = 0x1C7015)
        : Flash(std::move(name), size, jedec, Options{}) {}

    std::string        name() const override { return name_; }
    bool               loadable() const override { return true; }
    std::vector<Range> demand_ranges() const override {
        return {
            {"xip", window_}
        };
    }
    std::vector<Range> ranges() const override {
        return {
            {"xip",  window_},
            {"ctrl", 0x28   }, // keep in step with kCtrlSize in flash.cpp
        };
    }

    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    Page     page(const std::string &range, uint32_t page_offset) override;
    void     reset() override;

    void check_load(uint32_t offset, uint32_t size) const override;
    void load(uint32_t offset, std::span<const uint8_t> data) override;

    // Physical contents, CRC bytes included.
    std::vector<uint8_t> raw() const { return {raw_.begin(), raw_.end()}; }

    // Keeps the physical contents in the file `path` from now on: every load,
    // program and erase is written to it at once. A new file is created with
    // the present contents; an existing one must be the size of the flash and
    // replaces them. Throws std::runtime_error when the file cannot be used and
    // std::logic_error when a file is mounted already.
    void mount(const std::string &path);
    // The mounted file, or nullptr.
    const MappedFile *mounted() const { return file_.get(); }

  private:
    bool crc_en() const { return (config_ & kCrcEn) != 0; }
    // Whether XIP block `n` can be read (past the end never; CRC checked if enabled).
    bool readable(uint32_t n);
    bool block_valid(uint32_t n);
    void invalidate_window();
    void set_config(uint32_t value);

    void run(uint32_t opcode, uint32_t addr, bool wp);
    void write_command(uint32_t opcode, uint32_t addr, bool wp);
    void program(uint32_t addr);
    void erase(uint32_t opcode, uint32_t addr);
    // Forgets verdicts of the blocks behind raw [start, end) and invalidates the
    // matching part of the XIP window.
    void touched(uint32_t start, uint32_t end);

    std::string                 name_;
    uint32_t                    size_, jedec_, blocks_, window_;
    std::vector<uint8_t>        memory_; // the store while no file is mounted
    std::unique_ptr<MappedFile> file_;
    std::span<uint8_t>          raw_; // the store: memory_ or the mounted file
    // Cached CRC verdict per block: unknown until first checked.
    std::vector<std::optional<bool>> verdicts_;
    std::optional<uint32_t>          last_alert_block_;

    Options                      options_;
    std::map<uint32_t, uint32_t> words_; // the plain controller words
    uint32_t                     operate_ = 0, config_ = kCrcEn, m_value_ = 0;
    uint32_t                     sr_ = 0, sr2_ = 0, sr_out_ = 0;
    bool                         wel_ = false;
    std::array<uint32_t, 8>      read_fifo_{};
    uint32_t                     read_ptr_ = 0; // set from options_ at construction and reset
    std::deque<uint32_t>         write_fifo_;
};

} // namespace bk
