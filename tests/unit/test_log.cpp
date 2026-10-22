#include <doctest/doctest.h>

#include <sstream>

#include "core/log.h"
#include "core/module.h"

namespace {
// Compile-time check that the module contract is usable.
struct NullModule : bk::Module {
    std::string            name() const override { return "null"; }
    std::vector<bk::Range> ranges() const override { return {}; }
    uint32_t               read(const std::string &, uint32_t, uint32_t) override { return 0; }
    void                   write(const std::string &, uint32_t, uint32_t, uint32_t) override {}
};
} // namespace

TEST_CASE("module defaults") {
    NullModule m;
    CHECK_FALSE(m.loadable());
    CHECK_FALSE(m.next_event_ns().has_value());
    CHECK_THROWS_AS(m.signal("x"), std::invalid_argument);
    CHECK_THROWS_AS(m.check_load(0, 1), std::invalid_argument);
}

namespace {

struct LogFixture {
    std::ostringstream out;
    LogFixture() {
        bk::Logger::reset_for_tests();
        bk::Logger::configure(out, false);
    }
    ~LogFixture() { bk::Logger::reset_for_tests(); }
};

} // namespace

TEST_CASE_FIXTURE(LogFixture, "line format") {
    bk::Logger::get("bus").log(bk::Level::Info, "x");
    CHECK(out.str() == "INFO emulator.bus: x\n");
}

TEST_CASE_FIXTURE(LogFixture, "debug csv") {
    bk::Logger::set_debug("wdt,flash");
    bk::Logger::get("wdt").log(bk::Level::Debug, "d");
    bk::Logger::get("bus").log(bk::Level::Debug, "hidden");
    bk::Logger::get("bus").log(bk::Level::Info, "shown");
    CHECK(out.str() == "DEBUG emulator.wdt: d\nINFO emulator.bus: shown\n");
}

TEST_CASE_FIXTURE(LogFixture, "debug all") {
    bk::Logger::set_debug("all");
    bk::Logger::get("bus").log(bk::Level::Debug, "d");
    CHECK(out.str() == "DEBUG emulator.bus: d\n");
}

TEST_CASE_FIXTURE(LogFixture, "colour lines") {
    bk::Logger::configure(out, true);
    bk::Logger::get("a").log(bk::Level::Error, "e");
    bk::Logger::get("a").log(bk::Level::Warning, "w");
    bk::Logger::get("a").log(bk::Level::Info, "i");
    CHECK(out.str() == "\x1b[31mERROR emulator.a: e\x1b[0m\n"
                       "\x1b[33mWARNING emulator.a: w\x1b[0m\n"
                       "\x1b[32mINFO emulator.a: i\x1b[0m\n");
}

TEST_CASE("colour rules") {
    CHECK_FALSE(bk::colour_enabled(true, "1", "1"));
    CHECK_FALSE(bk::colour_enabled(false, "1", nullptr));
    CHECK(bk::colour_enabled(false, nullptr, "1"));
    CHECK(bk::colour_enabled(false, "", "1"));
    CHECK(bk::colour_enabled(true, nullptr, nullptr));
    CHECK(bk::colour_enabled(true, "", ""));
    CHECK_FALSE(bk::colour_enabled(false, nullptr, nullptr));
}

TEST_CASE_FIXTURE(LogFixture, "warn_once") {
    auto &l = bk::Logger::get("bus");
    l.warn_once(1, "first");
    l.warn_once(1, "again");
    l.warn_once(2, "other");
    CHECK(out.str() == "WARNING emulator.bus: first\nWARNING emulator.bus: other\n");
}
