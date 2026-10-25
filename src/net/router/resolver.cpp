#include "net/router/resolver.h"

#include <arpa/inet.h>
#include <fstream>
#include <sstream>

namespace bk {

std::optional<uint32_t> host_resolver(const std::string &path) {
    std::ifstream in(path);
    std::string   line;
    while (std::getline(in, line)) {
        if (auto comment = line.find_first_of("#;"); comment != std::string::npos) line.resize(comment);
        std::istringstream words(line);
        std::string        keyword, address;
        if (!(words >> keyword >> address) || keyword != "nameserver") continue;
        in_addr a;
        if (::inet_pton(AF_INET, address.c_str(), &a) == 1) return ntohl(a.s_addr);
    }
    return std::nullopt;
}

} // namespace bk
