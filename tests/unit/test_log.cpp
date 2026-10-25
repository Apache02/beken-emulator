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
    CHECK(out.str() == "[I] emulator.bus: x\n");
}

TEST_CASE_FIXTURE(LogFixture, "a named logger gets its own level") {
    bk::Logger::set_level("wdt", bk::Level::Debug);
    bk::Logger::set_level("flash", bk::Level::Error);
    bk::Logger::get("wdt").log(bk::Level::Debug, "d");
    bk::Logger::get("flash").log(bk::Level::Warning, "hidden");
    bk::Logger::get("bus").log(bk::Level::Debug, "hidden");
    bk::Logger::get("bus").log(bk::Level::Info, "shown");
    CHECK(out.str() == "[D] emulator.wdt: d\n[I] emulator.bus: shown\n");
}

TEST_CASE_FIXTURE(LogFixture, "all sets the level every other logger inherits") {
    bk::Logger::set_level("all", bk::Level::Debug);
    bk::Logger::get("bus").log(bk::Level::Debug, "d");
    CHECK(out.str() == "[D] emulator.bus: d\n");
    out.str("");
    bk::Logger::set_level("unknown", bk::Level::Warning); // its own level wins over all
    bk::Logger::get("unknown").log(bk::Level::Info, "hidden");
    bk::Logger::get("bus").log(bk::Level::Debug, "d");
    CHECK(out.str() == "[D] emulator.bus: d\n");
    bk::Logger::set_level("all", bk::Level::Error);
    bk::Logger::get("bus").log(bk::Level::Warning, "hidden");
    CHECK(out.str() == "[D] emulator.bus: d\n");
}

TEST_CASE_FIXTURE(LogFixture, "level names in any letter case") {
    CHECK(bk::Logger::parse_level("debug") == bk::Level::Debug);
    CHECK(bk::Logger::parse_level("DEBUG") == bk::Level::Debug);
    CHECK(bk::Logger::parse_level("Info") == bk::Level::Info);
    CHECK(bk::Logger::parse_level("WARNING") == bk::Level::Warning);
    CHECK(bk::Logger::parse_level("error") == bk::Level::Error);
    CHECK(bk::Logger::parse_level("None") == bk::Level::None);
    CHECK_FALSE(bk::Logger::parse_level("warn").has_value());
    CHECK_FALSE(bk::Logger::parse_level("").has_value());
}

TEST_CASE_FIXTURE(LogFixture, "level none silences a logger") {
    bk::Logger::set_level("bus", bk::Level::None);
    bk::Logger::get("bus").log(bk::Level::Error, "hidden");
    CHECK(out.str().empty());
    bk::Logger::set_level("all", bk::Level::None);
    bk::Logger::get("other").log(bk::Level::Error, "hidden");
    CHECK(out.str().empty());
}

TEST_CASE_FIXTURE(LogFixture, "colour lines") {
    bk::Logger::configure(out, true);
    bk::Logger::get("a").log(bk::Level::Error, "e");
    bk::Logger::get("a").log(bk::Level::Warning, "w");
    bk::Logger::get("a").log(bk::Level::Info, "i");
    CHECK(out.str() == "\x1b[31m[E] emulator.a: e\x1b[0m\n"
                       "\x1b[33m[W] emulator.a: w\x1b[0m\n"
                       "\x1b[32m[I] emulator.a: i\x1b[0m\n");
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
    CHECK(out.str() == "[W] emulator.bus: first\n[W] emulator.bus: other\n");
}
