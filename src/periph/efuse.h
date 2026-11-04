#pragma once

#include <array>
#include <cstdint>

namespace bk {

// The eFuse: 32 one-time programmable bytes behind two SCTRL words, served by
// Sctrl. Writing the control word with oper_en set runs the operation at once
// and leaves oper_en clear: a read puts the byte and the valid bit into the
// read data word, a write sets the given bits of the byte (bits never clear).
// The fuses start blank (every byte 0) and keep their values over a reset;
// the 2.5 V supply bit in SCTRL's control word is not needed for a write.
class Efuse {
  public:
    static constexpr uint32_t kCtrl = 0x074, kOptr = 0x078;
    static constexpr uint32_t kBytes = 32;

    uint32_t read(uint32_t offset) const;
    // Writes to the read data word are ignored.
    void write(uint32_t offset, uint32_t value);
    // Clears both words; the fuses are kept.
    void reset();

  private:
    std::array<uint8_t, kBytes> fuses_{};
    uint32_t                    ctrl_ = 0, optr_ = 0;
};

} // namespace bk
