#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bk {

// A byte stream a device talks to (host console, socket, pty, ...). Implemented
// by the application layer; devices hold a non-owning pointer.
struct ByteEndpoint {
    virtual void write(std::span<const uint8_t> data) = 0;
    // Returns whatever bytes are available now, possibly none; never blocks.
    virtual std::vector<uint8_t> read_nowait()       = 0;
    virtual std::string          description() const = 0;
    virtual ~ByteEndpoint()                          = default;
};

} // namespace bk
