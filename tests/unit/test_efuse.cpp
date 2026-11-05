#include <doctest/doctest.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/log.h"
#include "periph/efuse.h"

using namespace bk;

namespace {

constexpr uint32_t kEn = 1u << 0, kDir = 1u << 1, kValid = 1u << 8;

uint32_t ctrl(uint32_t addr, uint32_t data = 0, bool write = false) {
    return (data << 16) | (addr << 8) | (write ? kDir : 0) | kEn;
}

uint8_t fuse(Efuse &e, uint32_t addr) {
    e.write(Efuse::kCtrl, ctrl(addr));
    return static_cast<uint8_t>(e.read(Efuse::kOptr));
}

} // namespace

TEST_CASE("efuse starts blank: every byte reads 0 and is valid") {
    Efuse e;
    for (uint32_t addr = 0; addr < Efuse::kBytes; ++addr) {
        e.write(Efuse::kCtrl, ctrl(addr));
        CHECK(e.read(Efuse::kOptr) == kValid);
    }
}

TEST_CASE("efuse operation completes at once: oper_en reads 0, the other fields are kept") {
    Efuse e;
    e.write(Efuse::kCtrl, ctrl(5, 0xA5, true));
    CHECK(e.read(Efuse::kCtrl) == (ctrl(5, 0xA5, true) & ~kEn));
}

TEST_CASE("efuse write only sets bits") {
    Efuse e;
    e.write(Efuse::kCtrl, ctrl(3, 0x81, true));
    CHECK(fuse(e, 3) == 0x81);
    e.write(Efuse::kCtrl, ctrl(3, 0x18, true));
    CHECK(fuse(e, 3) == 0x99);
    e.write(Efuse::kCtrl, ctrl(3, 0x00, true));
    CHECK(fuse(e, 3) == 0x99);
    CHECK(fuse(e, 2) == 0);
    CHECK(fuse(e, 4) == 0);
}

TEST_CASE("efuse does nothing without oper_en") {
    Efuse e;
    e.write(Efuse::kCtrl, ctrl(7, 0xFF, true) & ~kEn);
    CHECK(e.read(Efuse::kOptr) == 0);
    CHECK(fuse(e, 7) == 0);
}

TEST_CASE("efuse address wraps to 5 bits") {
    Efuse e;
    e.write(Efuse::kCtrl, ctrl(1, 0x42, true));
    e.write(Efuse::kCtrl, (0x21u << 8) | kEn); // address field 0x21 selects byte 1
    CHECK(e.read(Efuse::kOptr) == (kValid | 0x42));
}

TEST_CASE("efuse read data register ignores writes") {
    Efuse e;
    e.write(Efuse::kOptr, 0x1FF);
    CHECK(e.read(Efuse::kOptr) == 0);
}

TEST_CASE("efuse reset clears the registers and keeps the fuses") {
    Efuse e;
    e.write(Efuse::kCtrl, ctrl(9, 0x3C, true));
    e.write(Efuse::kCtrl, ctrl(9));
    e.reset();
    CHECK(e.read(Efuse::kCtrl) == 0);
    CHECK(e.read(Efuse::kOptr) == 0);
    CHECK(fuse(e, 9) == 0x3C);
}

TEST_CASE("efuse preset sets the bytes from byte 0 and blanks the rest") {
    Efuse e;
    e.write(Efuse::kCtrl, ctrl(20, 0x11, true));
    const uint8_t bytes[] = {0x93, 0xB0, 0x0F};
    e.preset(bytes);
    CHECK(fuse(e, 0) == 0x93);
    CHECK(fuse(e, 2) == 0x0F);
    CHECK(fuse(e, 3) == 0);
    CHECK(fuse(e, 20) == 0);
    CHECK(e.fuses()[1] == 0xB0);
    const std::vector<uint8_t> too_many(33, 1);
    CHECK_THROWS_WITH_AS(e.preset(too_many), "the eFuse has 32 bytes, not 33", std::invalid_argument);
}

TEST_CASE("efuse write is a warning naming the byte, the bits and the CPU site") {
    std::ostringstream out;
    Logger::reset_for_tests();
    Logger::configure(out, false);
    Efuse e([] { return CodeSite{0x1234, 0x5678}; });
    e.write(Efuse::kCtrl, ctrl(4, 0x81, true));
    e.write(Efuse::kCtrl, ctrl(4, 0x18, true));
    e.write(Efuse::kCtrl, ctrl(4));
    Logger::reset_for_tests();
    const std::string log = out.str();
    CHECK(log.find("eFuse byte 4 burnt with 0x81: 0x00 -> 0x81 by pc 0x00001234, lr 0x00005678") != std::string::npos);
    CHECK(log.find("eFuse byte 4 burnt with 0x18: 0x81 -> 0x99") != std::string::npos);
    CHECK(log.find("[W]") != std::string::npos);
    // A read is not logged.
    CHECK(std::count(log.begin(), log.end(), '\n') == 2);
}
