#pragma once

#include <memory>
#include <string_view>

#include "core/endpoint.h"

namespace bk {

// The host end of a UART, by command-line spec:
//
//   pty    a new pseudo-terminal. The slave is put in raw mode and kept open,
//          so a terminal program can close and reopen it; the master is
//          non-blocking. description() is the slave path. write() drops data
//          when the buffer is full or nobody reads. read_nowait() returns up
//          to 4096 bytes per call.
//   stdio  stdin and stdout. A terminal is switched to raw mode until the
//          endpoint is destroyed; Ctrl-C still raises SIGINT (it is not passed to
//          the firmware) so the emulator can be stopped. read_nowait() polls with a zero timeout and
//          returns up to 4096 bytes per call. description() is "stdio".
//
//   other  a host serial device path (for example /dev/ttyUSB0), opened raw at
//          115200 baud and non-blocking; errors on reading and writing are
//          swallowed. description() is the path.
//
// Throws std::runtime_error when the pty cannot be created or the serial
// device cannot be opened or is not a terminal.
std::unique_ptr<ByteEndpoint> make_endpoint(std::string_view spec);

// The stdio endpoint over the given descriptors (not owned, not closed).
// Raw mode is applied to `in_fd` only when it is a terminal.
std::unique_ptr<ByteEndpoint> make_stdio_endpoint(int in_fd, int out_fd);

} // namespace bk
