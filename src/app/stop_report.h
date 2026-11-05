#pragma once

#include <string>

#include "core/machine.h"
#include "core/profile.h"

namespace bk {

// The state of the machine where a run stopped, as text for the end of a run:
// the device time, the CPU (pc with the instruction there, mode, interrupt
// masks and lines, the registers), the words at sp and every interrupt
// controller of the profile (enabled, raised and pending sources, each with the
// modules the profile wires to it). It starts
// with an empty line, so it stands apart from the firmware's output, and ends
// with a newline. Reading it changes nothing.
std::string stop_report(const Profile &profile, Machine &machine);

} // namespace bk
