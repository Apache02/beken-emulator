#include <doctest/doctest.h>

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
