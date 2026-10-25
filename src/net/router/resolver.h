#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace bk {

// The first IPv4 nameserver of a resolv.conf, or nothing: no file, or no
// `nameserver` line with an IPv4 address. Comments start with '#' or ';'.
std::optional<uint32_t> host_resolver(const std::string &path = "/etc/resolv.conf");

} // namespace bk
