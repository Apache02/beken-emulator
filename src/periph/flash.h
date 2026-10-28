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
#include "core/xip.h"

namespace bk {

// SPI flash controller with an execute-in-place window.
//
// The physical store is a sequence of 34-byte blocks: 32 data bytes followed by
// a big-endian CRC-16 of them. The `xip` range is a decoded view that shows only
// the data bytes (32 per block); while CRC checking is enabled, blocks whose CRC
// does not match cannot be read through it. The `regs` range is the command
// interface: status registers, a read FIFO, a write FIFO, page program and
// erases. Every command completes instantly.
class Flash : public Module, public XipMemory {
  public:
    static constexpr uint32_t kCrcEn = 1u << 26;

    // The controller's start state. The defaults are the reset state of the
    // design; a profile can give the state a running chip is measured in.
    struct Options {
        // Start values of controller words (offset -> value), which they return
        // to at reset. Besides the plain words, which have no function in the
        // model, read back what was written and read 0 when not given, two
        // modelled words take one:
        //  - `config` (0x1C), crc_en set by default;
        //  - `status` (0x14): the `m_value` field [29:22] and the position
        //    [18:16] of the read FIFO pointer, the next word a read of
        //    `data_flash_sw` returns; the other bits are fixed or come from the
        //    flash, and 0 by default.
        std::map<uint32_t, uint32_t> words;
        // The size of the XIP window in bytes; firmware does not read the flash
        // through XIP beyond the first 2 MiB. By default the window covers the
        // data of every block, up to 2 MiB.
        std::optional<uint32_t> xip_size;
    };

    static constexpr uint32_t kMaxXipSize = 0x200000;

    // Throws std::invalid_argument when `options.words` names an offset that is
    // neither `config`, `status` nor a word-aligned plain offset of the `regs`
    // range, or when `options.xip_size` is not a multiple of 4096 up to 2 MiB.
    Flash(std::string name, uint32_t size, uint32_t jedec, Options options);
    explicit Flash(std::string name, uint32_t size = 0x200000, uint32_t jedec = 0x1C7015)
        : Flash(std::move(name), size, jedec, Options{}) {}

    std::string        name() const override { return name_; }
    bool               loadable() const override { return true; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x28}  // keep in step with kRegsSize in flash.cpp
        };
    }

    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void     reset() override;

    // The XIP window: the data bytes of the first blocks, up to 2 MiB, or
    // `Options::xip_size`.
    uint32_t xip_size() const override { return window_; }
    uint32_t xip_read(uint32_t offset, uint32_t size) override;
    Page     xip_page(uint32_t page_offset) override;

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
    // The size of the flash in bytes.
    uint32_t size() const { return size_; }

  private:
    bool crc_en() const { return (config_ & kCrcEn) != 0; }
    // Whether XIP block `n` can be read (past the end never; CRC checked if enabled).
    bool readable(uint32_t n);
    bool block_valid(uint32_t n);
    void invalidate_window();
    void set_config(uint32_t value);
    // The start values of `config` and of the fields of `status`, from the options.
    uint32_t config_reset() const;
    void     reset_status();

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
    uint32_t                     read_ptr_ = 0; // set from the `status` start value at construction and reset
    std::deque<uint32_t>         write_fifo_;
};

} // namespace bk
