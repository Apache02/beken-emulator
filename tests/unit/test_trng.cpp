#include <doctest/doctest.h>

#include "periph/trng.h"

using namespace bk;

namespace {
constexpr uint32_t kCtrl = 0, kData = 4;
uint32_t           r(Trng &t, uint32_t off) { return t.read("regs", off, 4); }
void               w(Trng &t, uint32_t off, uint32_t v) { t.write("regs", off, 4, v); }
} // namespace

TEST_CASE("trng has one range of 8 bytes and starts enabled") {
    Trng t("trng");
    REQUIRE(t.ranges().size() == 1);
    CHECK(t.ranges()[0].name == "regs");
    CHECK(t.ranges()[0].size == 8);
    CHECK(r(t, kCtrl) == 1);
}

TEST_CASE("trng enabled data reads count up from 0") {
    Trng t("trng");
    for (uint32_t i = 0; i < 5; ++i) CHECK(r(t, kData) == i);
}

TEST_CASE("trng ctrl keeps only bit 0") {
    Trng t("trng");
    w(t, kCtrl, 0xFFFFFFFE);
    CHECK(r(t, kCtrl) == 0);
    w(t, kCtrl, 3);
    CHECK(r(t, kCtrl) == 1);
}

TEST_CASE("trng disabled data is a plain latch, writes accepted only then") {
    Trng t("trng");
    w(t, kData, 0x1234); // enabled: ignored
    CHECK(r(t, kData) == 0);
    w(t, kCtrl, 0);
    CHECK(r(t, kData) == 0); // latch still 0
    w(t, kData, 0xDEADBEEF);
    CHECK(r(t, kData) == 0xDEADBEEF);
    CHECK(r(t, kData) == 0xDEADBEEF); // reads do not change it
    w(t, kCtrl, 1);
    CHECK(r(t, kData) == 1); // the counter did not move while disabled; the read above was number 0
}

TEST_CASE("trng other offsets read 0") {
    Trng t("trng");
    CHECK(r(t, 0x08) == 0);
    CHECK(r(t, 0x01) == 0);
}

TEST_CASE("trng reset enables it and clears counter and latch") {
    Trng t("trng");
    r(t, kData);
    r(t, kData);
    w(t, kCtrl, 0);
    w(t, kData, 7);
    t.reset();
    CHECK(r(t, kCtrl) == 1);
    CHECK(r(t, kData) == 0);
    w(t, kCtrl, 0);
    CHECK(r(t, kData) == 0);
}
