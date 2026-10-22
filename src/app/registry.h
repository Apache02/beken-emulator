#pragma once

#include "core/machine.h"

namespace bk {

// Factories for every built-in module type: flash, intc, timer, uart, wdt.
//
// Parameters each type reads (anything else is ignored):
//   flash: size (default 0x200000), jedec_id (default 0x1C7015)
//   intc:  reset_words (table of "offset" -> value, offsets written as
//          decimal or 0x hex strings), signals (table of name ->
//          [offset, bit] or { offset, shift = 0, width = 1, equals = 1 })
//   timer: irq, hz (default 26000000)
//   uart:  irq; attached to the endpoint given for its name, if any
//   wdt:   clock_off (from clock_off_signal); expiry resets the chip
ModuleRegistry builtin_registry();

} // namespace bk
