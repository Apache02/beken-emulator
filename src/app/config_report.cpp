#include "app/config_report.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <variant>

#include "core/log.h"
#include "core/machine.h"
#include "periph/flash.h"
#include "net/forwarder.h"

namespace bk {

namespace {

using std::string;
using std::vector;

// One line of a block: the cells of its columns (a cell may hold several
// lines) and the rows that hang below it.
struct Row {
    vector<string> cells;
    vector<Row>    children;
};

struct Block {
    string         title, note;
    vector<string> header;
    vector<Row>    rows;
};

string upper(string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

string size_text(uint64_t bytes) {
    if (bytes >= 1024 * 1024 && bytes % (1024 * 1024) == 0) return std::to_string(bytes / (1024 * 1024)) + " MiB";
    if (bytes >= 1024 && bytes % 1024 == 0) return std::to_string(bytes / 1024) + " KiB";
    if (bytes >= 1024) return std::to_string((bytes + 1023) / 1024) + " KiB";
    return std::to_string(bytes) + " B";
}

string level_text(Level level) {
    switch (level) {
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warning: return "warning";
        case Level::Error: return "error";
        case Level::None: return "none";
    }
    return "";
}

vector<string> lines_of(const string &cell) {
    vector<string>     out;
    std::istringstream in(cell);
    for (string line; std::getline(in, line);) out.push_back(line);
    if (out.empty()) out.emplace_back();
    return out;
}

// The width of one branch mark, which pushes a nested row to the right.
constexpr size_t kBranch = 3;

// The widest line of each column over the header and every row, children
// included. The first column of a nested row starts `depth` branches later.
void measure(const Row &row, vector<size_t> &widths, size_t depth = 0) {
    for (size_t c = 0; c < row.cells.size(); ++c) {
        for (const string &line : lines_of(row.cells[c]))
            widths[c] = std::max(widths[c], line.size() + (c == 0 ? depth * kBranch : 0));
    }
    for (const Row &child : row.children) measure(child, widths, depth + 1);
}

// The cells joined into columns; the first one is `shift` wide less because
// the branch marks in front of it took that much.
string padded(const vector<string> &cells, const vector<size_t> &widths, size_t shift = 0) {
    string out;
    for (size_t c = 0; c < cells.size(); ++c) {
        out += cells[c];
        const size_t width = widths[c] - (c == 0 ? shift : 0);
        if (c + 1 < cells.size()) out += string(width - cells[c].size() + 2, ' ');
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// `indent` is the text in front of the branch marks of this level.
void render(
    const vector<Row> &rows, const vector<size_t> &widths, const string &indent, size_t depth, std::ostream &out) {
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool             last   = i + 1 == rows.size();
        const Row             &row    = rows[i];
        size_t                 height = 1;
        vector<vector<string>> cells;
        for (const string &cell : row.cells) {
            cells.push_back(lines_of(cell));
            height = std::max(height, cells.back().size());
        }
        for (size_t l = 0; l < height; ++l) {
            vector<string> line;
            for (const auto &c : cells) line.push_back(l < c.size() ? c[l] : string());
            out << indent << (l == 0 ? (last ? "└─ " : "├─ ") : (last ? "   " : "│  "))
                << padded(line, widths, depth * kBranch) << '\n';
        }
        render(row.children, widths, indent + (last ? "   " : "│  "), depth + 1, out);
    }
}

void render(const Block &block, std::ostream &out) {
    out << block.title;
    if (!block.note.empty()) out << string(block.title.size() < 10 ? 10 - block.title.size() : 2, ' ') << block.note;
    out << '\n';
    if (block.rows.empty()) return;
    vector<size_t> widths(block.header.size(), 0);
    for (size_t c = 0; c < block.header.size(); ++c) widths[c] = block.header[c].size();
    for (const Row &row : block.rows) measure(row, widths);
    out << "│  " << padded(block.header, widths) << '\n';
    render(block.rows, widths, "", 0, out);
}

// A load target as a number (0x hex, 0o octal, 0b binary or decimal).
std::optional<uint32_t> parse_number(const string &text) {
    int         base = 10;
    const char *p    = text.data();
    const char *end  = p + text.size();
    if (text.size() > 2 && text[0] == '0') {
        char k = static_cast<char>(std::tolower(static_cast<unsigned char>(text[1])));
        if (k == 'x') base = 16;
        if (k == 'o') base = 8;
        if (k == 'b') base = 2;
        if (base != 10) p += 2;
    }
    uint32_t value  = 0;
    auto [stop, ec] = std::from_chars(p, end, value, base);
    if (ec != std::errc() || stop != end) return std::nullopt;
    return value;
}

// A load resolved against the profile: where it goes and what it is called.
struct PlannedLoad {
    string   alias; // as given on the command line
    string   file;
    string   size;   // text; "missing" when the file cannot be read
    string   flash;  // the flash module, empty for a bus address
    uint32_t at = 0; // the offset in the flash or the bus address
};

vector<PlannedLoad> plan(const CliOptions &options, const Profile &profile) {
    vector<PlannedLoad> out;
    for (const LoadSpec &spec : options.loads) {
        PlannedLoad load;
        load.alias = spec.target;
        load.file  = spec.file.string();
        std::error_code ec;
        auto            bytes = std::filesystem::file_size(spec.file, ec);
        load.size             = ec ? "missing" : size_text(bytes);
        bool found            = false;
        for (const auto &[name, alias] : profile.alias) {
            if (upper(name) != upper(spec.target)) continue;
            found = true;
            if (const auto *flash = std::get_if<FlashAlias>(&alias.target)) {
                load.flash = flash->module;
                load.at    = flash->offset;
            } else {
                load.at = std::get<uint32_t>(alias.target);
            }
        }
        if (!found) {
            auto number = parse_number(spec.target);
            if (!number)
                throw std::invalid_argument("unknown load target '" + spec.target + "' (not an alias or a number)");
            load.at = *number;
        }
        out.push_back(std::move(load));
    }
    return out;
}

bool is_flash(const ModuleSpec &m) { return m.type == "flash"; }

// The logger a module type writes to; empty when it has none of its own.
string logger_of(const string &type) {
    if (type == "flash") return "flash";
    if (type == "gdma") return "gdma";
    if (type == "i2c1") return "i2c";
    if (type == "i2c2") return "i2c2";
    if (type == "jpeg") return "jpeg";
    if (type == "wdt") return "wdt";
    if (type == "wifi_mac") return "wifi.mac_pl";
    return {};
}

string module_level(const ModuleSpec &m) {
    // The stand-ins log their accesses at the level of their log_level parameter.
    if (m.type == "unknown" || m.type == "sdhost") {
        const ParamValue *v = m.params.find("log_level");
        if (v && v->string()) {
            if (auto level = Logger::parse_level(*v->string())) return level_text(*level);
        }
        return "info";
    }
    string logger = logger_of(m.type);
    return logger.empty() ? "-" : level_text(Logger::get(logger).level());
}

// The register ranges of a module with their addresses, in the order the module
// lists them. The XIP window of the flash is memory, not registers, and is not
// among them.
struct Window {
    string   name;
    uint32_t base, size;
};

vector<Window> register_windows(const ModuleSpec &m, Machine &machine) {
    vector<Window> out;
    Module        *module = machine.module(m.name);
    if (!module) return out;
    for (const Range &r : module->ranges()) {
        auto base = m.bases.find(r.name);
        if (base == m.bases.end()) continue;
        out.push_back({r.name, base->second, r.size});
    }
    return out;
}

// The address and the size of each register range, one line per range; a module
// with several ranges names them.
struct RangeText {
    string regs, sizes;
};

RangeText ranges_text(const vector<Window> &windows) {
    size_t name_width = 0;
    for (const Window &w : windows) name_width = std::max(name_width, w.name.size());
    RangeText out;
    for (const Window &w : windows) {
        char size[16];
        std::snprintf(size, sizeof size, "0x%X", w.size);
        const string sep = out.regs.empty() ? "" : "\n";
        out.regs +=
            sep + (windows.size() == 1 ? "" : w.name + string(name_width - w.name.size() + 1, ' ')) + hex8(w.base);
        out.sizes += sep + size;
    }
    return out;
}

string join(const vector<string> &items) {
    string out;
    for (const string &s : items) out += (out.empty() ? "" : ", ") + s;
    return out;
}

} // namespace

std::string config_report(const CliOptions &options, const Profile &profile, Machine &machine) {
    auto has_module = [&](const string &name, const char *type) {
        return std::any_of(profile.modules.begin(), profile.modules.end(), [&](const ModuleSpec &m) {
            return m.name == name && m.type == type;
        });
    };
    for (const auto &[name, spec] : options.uart) {
        if (!has_module(name, "uart"))
            throw std::invalid_argument("no UART named " + name + " in profile " + profile.name);
    }
    for (const auto &[bus, spec] : options.i2c) {
        if (!has_module(bus, "i2c1") && !has_module(bus, "i2c2"))
            throw std::invalid_argument("no I2C bus named " + bus + " in profile " + profile.name);
    }

    vector<const ModuleSpec *> flashes;
    for (const ModuleSpec &m : profile.modules) {
        if (is_flash(m)) flashes.push_back(&m);
    }
    // The file each flash module keeps its contents in.
    std::map<string, string> mounts;
    for (const auto &[name, file] : options.flash_mounts) {
        string target = name;
        if (target.empty()) {
            if (flashes.size() > 1)
                throw std::invalid_argument("profile " + profile.name +
                                            " has more than one flash; use --flash-mount NAME=FILE");
            if (flashes.empty()) throw std::invalid_argument("no flash in profile " + profile.name);
            target = flashes[0]->name;
        } else if (!has_module(target, "flash")) {
            throw std::invalid_argument("no flash named " + target + " in profile " + profile.name);
        }
        mounts[target] = file;
    }
    const vector<PlannedLoad> loads = plan(options, profile);

    std::ostringstream out;
    out << "profile   " << profile.name << '\n';
    out << "cpu       " << profile.insns_per_us << " insns/us\n";

    for (const ModuleSpec *flash : flashes) {
        Block block;
        block.title = "flash";
        // The module is named only when the profile has several.
        if (flashes.size() > 1) block.note = flash->name;
        block.header = {"OP", "OFFSET", "SIZE", "FILE"};
        // A mounted file is the flash, from its first byte; without one the
        // flash is in memory and there is nothing to list for it.
        if (auto mount = mounts.find(flash->name); mount != mounts.end()) {
            uint32_t size = 0;
            if (auto *module = dynamic_cast<Flash *>(machine.module(flash->name))) size = module->size();
            block.rows.push_back({
                {"mount", hex8(0), size_text(size), mount->second},
                {}
            });
        }
        for (const PlannedLoad &load : loads) {
            if (load.flash == flash->name)
                block.rows.push_back({
                    {"load", hex8(load.at), load.size, load.file},
                    {}
                });
        }
        out << '\n';
        render(block, out);
    }

    {
        Block block;
        block.title  = "memory";
        block.header = {"NAME", "ADDRESS", "SIZE", "NOTE"};
        for (const MemSpec &mem : profile.memory) {
            vector<string> notes;
            if (mem.reset_fill) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "fill 0x%02X", *mem.reset_fill);
                notes.emplace_back(buf);
            }
            if (mem.window > mem.size) notes.push_back("window " + size_text(mem.window));
            if (!mem.executable) notes.emplace_back("no exec");
            Row row{
                {mem.name, hex8(mem.base), size_text(mem.size), join(notes)},
                {}
            };
            for (const PlannedLoad &load : loads) {
                if (load.flash.empty() && load.at >= mem.base && load.at - mem.base < mem.size)
                    row.children.push_back({
                        {load.alias, hex8(load.at), load.size, load.file},
                        {}
                    });
            }
            block.rows.push_back(std::move(row));
        }
        out << '\n';
        render(block, out);
    }

    {
        Block block;
        block.title  = "peripherals";
        block.header = {"NAME", "TYPE", "ADDRESS", "SIZE", "LOG", "ATTACHED"};
        // By the lowest register address of a module; the profile order breaks a tie.
        struct Entry {
            const ModuleSpec *module;
            vector<Window>    windows;
            uint32_t          lowest = UINT32_MAX;
        };
        vector<Entry> sorted;
        for (const ModuleSpec &m : profile.modules) {
            Entry entry{&m, register_windows(m, machine)};
            for (const Window &w : entry.windows) entry.lowest = std::min(entry.lowest, w.base);
            sorted.push_back(std::move(entry));
        }
        std::stable_sort(
            sorted.begin(), sorted.end(), [](const Entry &a, const Entry &b) { return a.lowest < b.lowest; });
        for (const Entry &entry : sorted) {
            const ModuleSpec &m = *entry.module;
            vector<string>    attached;
            if (m.type == "uart") {
                auto given = options.uart.find(m.name);
                attached.push_back(given == options.uart.end() ? "pty" : given->second);
            }
            for (const auto &[bus, spec] : options.i2c) {
                if (bus == m.name) attached.push_back(spec);
            }
            const RangeText ranges = ranges_text(entry.windows);
            block.rows.push_back({
                {m.name, m.type, ranges.regs, ranges.sizes, module_level(m), attached.empty() ? "-" : join(attached)},
                {}
            });
        }
        out << '\n';
        render(block, out);
    }

    {
        Block block;
        block.title  = "network";
        block.header = {"KEY", "VALUE"};
        string wifi  = "off";
        if (options.wifi_ap) {
            wifi = "access point \"" + *options.wifi_ap + "\", channel " + std::to_string(options.wifi_channel) +
                   (options.wifi_password ? ", wpa2" : ", open") +
                   (options.wifi_internet ? ", internet" : ", no internet");
        } else if (options.ssid) {
            wifi = "station, joins only \"" + *options.ssid + "\"";
        } else if (!options.publish.empty()) {
            wifi = "station, joins any open network";
        }
        block.rows.push_back({
            {"wifi", wifi},
            {}
        });
        if (options.pcap)
            block.rows.push_back({
                {"pcap", *options.pcap},
                {}
            });
        for (const Publish &p : options.publish)
            block.rows.push_back({
                {"publish", describe(p)},
                {}
            });
        out << '\n';
        render(block, out);
    }
    return out.str();
}

} // namespace bk
