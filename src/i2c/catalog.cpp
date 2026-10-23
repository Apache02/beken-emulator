#include "i2c/catalog.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <stdexcept>

#include "i2c/hi704.h"

namespace bk {

namespace {

using Make = std::function<std::unique_ptr<I2cDevice>(uint8_t)>;

// Type name -> factory and default address.
const std::map<std::string, std::pair<Make, uint8_t>> &devices() {
    static const std::map<std::string, std::pair<Make, uint8_t>> table = {
        {"hi704",
         {[](uint8_t address) -> std::unique_ptr<I2cDevice> { return std::make_unique<Hi704>(address); },
          Hi704::kDefaultAddress}},
    };
    return table;
}

} // namespace

std::unique_ptr<I2cDevice> parse_device_spec(const std::string &text) {
    const auto  at    = text.find('@');
    std::string name  = text.substr(0, at);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    auto it = devices().find(lower);
    if (it == devices().end()) {
        std::string known;
        for (const auto &[type, entry] : devices()) known += (known.empty() ? "" : ", ") + type;
        throw std::invalid_argument("unknown I2C device '" + name + "' (known: " + known + ")");
    }
    if (at == std::string::npos) return it->second.first(it->second.second);

    const std::string usage   = "bad I2C device '" + text + "' (want TYPE[@ADDRESS], ADDRESS a 7-bit number 0x08-0x77)";
    const std::string number  = text.substr(at + 1);
    size_t            used    = 0;
    unsigned long     address = 0;
    try {
        // Automatic radix, but no octal: a leading zero must be followed by x.
        if (number.empty() || !std::isalnum(static_cast<unsigned char>(number[0])) ||
            (number.size() > 1 && number[0] == '0' && std::isdigit(static_cast<unsigned char>(number[1]))))
            throw std::invalid_argument("");
        address = std::stoul(number, &used, 0);
    } catch (const std::exception &) {
        throw std::invalid_argument(usage);
    }
    if (used != number.size() || address < 0x08 || address > 0x77) throw std::invalid_argument(usage);
    return it->second.first(static_cast<uint8_t>(address));
}

} // namespace bk
