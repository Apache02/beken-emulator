#pragma once

#include "core/machine.h"

namespace bk {

// Factories for every built-in module type: flash, gdma, gpio, i2c1, intc,
// jpeg, saradc, sctrl, timer, trng, uart, unknown, wdt, wifi_mac.
//
// Parameters each type reads (anything else is ignored):
//   flash: size (default 0x200000), jedec_id (default 0x1C7015), config_reset
//          (reset value of config, default crc_en only), m_value_reset (reset
//          value of the m_value field, a byte, default 0), read_ptr_reset (reset
//          position 0-7 of the read FIFO pointer, default 0), reset_words
//          (start values of unused ctrl words, as for intc)
//   gdma:  irq, bus_hz (clock the transfer times were measured at, default
//          120000000)
//   gpio:  irq, pin_reset (reset value of every pin word, default 0),
//          reset_words (as for intc)
//   i2c1:  irq, ref_hz (clock the bus divider divides, default 26000000);
//          devices are attached by the caller
//   intc:  reset_words (table of "offset" -> value, offsets written as
//          decimal or 0x hex strings), signals (table of name ->
//          [offset, bit] or { offset, shift = 0, width = 1, equals = 1 })
//   jpeg:  irq, dma + dma_request (the GDMA request line), frame_interval_ms
//          (default 1000), encode_ms (default 150), picture (path of the JPEG
//          to show; default: the embedded data/picture.jpg)
//   saradc: irq, sample (value every sample reads, default 330)
//   sctrl: chip_id, device_id, reset_words (cold values), warm_reset_words
//          (overrides after a chip reset), signals (as for intc),
//          read_adjust (list of { offset, shift, width, add })
//   sdhost: size (default 0x100), reset_words (as for intc); an SD host with no
//          card in the slot, see SdHost
//   timer: irq, hz (default 26000000)
//   trng:  no parameters
//   unknown: size (default 0x100), reset_words (as for intc)
//   wifi_mac: mac_powered (signal; always powered without one); only the
//          monotonic counters are emulated, the other registers are stand-ins
//   uart:  irq; attached to the endpoint given for its name, if any
//   wdt:   clock_off (from clock_off_signal); expiry resets the chip
ModuleRegistry builtin_registry();

} // namespace bk
