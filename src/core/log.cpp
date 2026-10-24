#include "core/log.h"

#include <cctype>
#include <iostream>
#include <map>
#include <memory>

namespace bk {

namespace {

struct State {
    std::ostream                                               *out    = &std::cerr;
    bool                                                        colour = false;
    Level                                                       root   = Level::Info;
    std::map<std::string, std::unique_ptr<Logger>, std::less<>> loggers;
};

State &state() {
    static State s;
    return s;
}

const char *level_name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error: return "ERROR";
    }
    return "?";
}

const char *level_colour(Level l) {
    switch (l) {
        case Level::Error: return "\x1b[31m";
        case Level::Warning: return "\x1b[33m";
        case Level::Info: return "\x1b[32m";
        case Level::Debug: return "";
    }
    return "";
}

} // namespace

bool colour_enabled(bool is_tty, const char *no_color, const char *force_color) {
    if (no_color && *no_color) return false;
    if (force_color && *force_color) return true;
    return is_tty;
}

Logger &Logger::get(std::string_view suffix) {
    auto &loggers = state().loggers;
    auto  it      = loggers.find(suffix);
    if (it == loggers.end()) {
        std::string name = "emulator.";
        name += suffix;
        it = loggers.emplace(std::string(suffix), std::unique_ptr<Logger>(new Logger(std::move(name)))).first;
    }
    return *it->second;
}

Level Logger::effective_level() const { return level_.value_or(state().root); }

void Logger::log(Level level, std::string_view msg) {
    if (level < effective_level()) return;
    State      &s      = state();
    const char *colour = s.colour ? level_colour(level) : "";
    *s.out << colour << level_name(level) << ' ' << name_ << ": " << msg << (*colour ? "\x1b[0m" : "") << '\n';
}

void Logger::warn_once(uint64_t key, std::string_view msg) {
    if (!seen_.insert(key).second) return;
    log(Level::Warning, msg);
}

void Logger::configure(std::ostream &out, bool colour) {
    state().out    = &out;
    state().colour = colour;
}

void Logger::set_level(std::string_view suffix, Level level) {
    if (suffix == "all")
        state().root = level;
    else
        get(suffix).level_ = level;
}

std::optional<Level> Logger::parse_level(std::string_view text) {
    std::string lower(text);
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "debug") return Level::Debug;
    if (lower == "info") return Level::Info;
    if (lower == "warning") return Level::Warning;
    if (lower == "error") return Level::Error;
    return std::nullopt;
}

void Logger::reset_for_tests() {
    State &s = state();
    s.out    = &std::cerr;
    s.colour = false;
    s.root   = Level::Info;
    for (auto &[key, logger] : s.loggers) {
        logger->level_.reset();
        logger->seen_.clear();
    }
}

} // namespace bk
