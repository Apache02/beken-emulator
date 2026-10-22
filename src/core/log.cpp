#include "core/log.h"

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

void Logger::set_debug(std::string_view csv) {
    if (csv == "all") {
        state().root = Level::Debug;
        return;
    }
    while (!csv.empty()) {
        size_t           comma = csv.find(',');
        std::string_view item  = csv.substr(0, comma);
        if (!item.empty()) get(item).level_ = Level::Debug;
        if (comma == std::string_view::npos) break;
        csv.remove_prefix(comma + 1);
    }
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
