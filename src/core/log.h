#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace bk {

// None is above Error so that a logger set to it drops every message.
enum class Level { Debug, Info, Warning, Error, None };

// Whether coloured output should be used. A non-empty NO_COLOR always wins,
// then a non-empty FORCE_COLOR, otherwise colour follows the tty state.
bool colour_enabled(bool is_tty, const char *no_color, const char *force_color);

// Named logger; every logger is called "emulator.<suffix>" and inherits its
// level from the "emulator" root unless it has been set explicitly.
class Logger {
  public:
    static Logger &get(std::string_view suffix);

    void log(Level level, std::string_view msg);
    // Logs a warning the first time `key` is seen on this logger.
    void warn_once(uint64_t key, std::string_view msg);

    // Sets the stream shared by all loggers (default std::cerr, no colour).
    static void configure(std::ostream &out, bool colour);
    // Sets the level of the logger with this suffix; "all" sets the root, which
    // every logger without a level of its own inherits.
    static void set_level(std::string_view suffix, Level level);
    // "debug", "info", "warning" or "error" in any letter case.
    static std::optional<Level> parse_level(std::string_view text);
    // Restores default levels, stream and warn_once state. Tests only.
    static void reset_for_tests();

    const std::string &name() const { return name_; }

  private:
    explicit Logger(std::string name) : name_(std::move(name)) {}
    Level effective_level() const;

    std::string                  name_;
    std::optional<Level>         level_;
    std::unordered_set<uint64_t> seen_;
};

} // namespace bk
