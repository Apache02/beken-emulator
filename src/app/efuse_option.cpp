#include "app/efuse_option.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "core/log.h"
#include "periph/efuse.h"
#include "periph/flash.h"
#include "periph/sctrl.h"

namespace bk {

namespace {

bool is_hex(const std::string &s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

FlashKey parse_key(const std::string &text) {
    const std::invalid_argument bad(
        "--efuse key: needs four hex words, for example key:510fb093,a3cbeadc,5993a17e,c7adeb03");
    std::vector<std::string> words;
    std::string              word;
    for (char c : text + ',') {
        if (c == ',' || c == ' ') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    if (words.size() == 1 && words[0].size() == 32)
        words = {words[0].substr(0, 8), words[0].substr(8, 8), words[0].substr(16, 8), words[0].substr(24, 8)};
    if (words.size() != 4) throw bad;
    FlashKey key;
    for (size_t i = 0; i < key.size(); ++i) {
        std::string w = words[i];
        if (w.size() > 2 && w[0] == '0' && (w[1] == 'x' || w[1] == 'X')) w = w.substr(2);
        if (!is_hex(w) || w.size() > 8) throw bad;
        key[i] = static_cast<uint32_t>(std::stoul(w, nullptr, 16));
    }
    return key;
}

std::vector<uint8_t> read_file(const std::string &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::invalid_argument("cannot read eFuse file " + file);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > Efuse::kBytes)
        throw std::invalid_argument("eFuse file " + file + " has " + std::to_string(bytes.size()) +
                                    " bytes, the eFuse 32");
    return bytes;
}

std::string hex_bytes(const std::vector<uint8_t> &bytes) {
    std::string out;
    char        buf[3];
    for (uint8_t b : bytes) {
        std::snprintf(buf, sizeof buf, "%02x", b);
        out += buf;
    }
    return out;
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

Logger &log() { return Logger::get("efuse"); }

} // namespace

EfuseSpec parse_efuse(const std::string &value) {
    EfuseSpec spec;
    if (value.starts_with("key:")) {
        spec.kind = EfuseSpec::Kind::Key;
        spec.key  = parse_key(value.substr(4));
    } else if (is_hex(value)) {
        if (value.size() % 2 != 0 || value.size() > 2 * Efuse::kBytes)
            throw std::invalid_argument("--efuse HEX needs an even number of hex digits, up to 64");
        spec.kind = EfuseSpec::Kind::Bytes;
        for (size_t i = 0; i < value.size(); i += 2)
            spec.bytes.push_back(static_cast<uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
    } else {
        if (value.empty()) throw std::invalid_argument("--efuse needs a file, HEX or key:W0,W1,W2,W3");
        spec.kind = EfuseSpec::Kind::File;
        spec.file = value;
    }
    return spec;
}

std::string describe(const EfuseSpec &spec) {
    switch (spec.kind) {
        case EfuseSpec::Kind::File: return "file " + spec.file;
        case EfuseSpec::Kind::Bytes: return "bytes " + hex_bytes(spec.bytes);
        case EfuseSpec::Kind::Key: return "key " + key_text(spec.key);
    }
    return "";
}

void setup_efuse(Machine &machine, const Profile &profile, const std::optional<EfuseSpec> &spec) {
    auto *sctrl = dynamic_cast<Sctrl *>(machine.module("SCTRL"));
    if (spec && !sctrl) throw std::invalid_argument("--efuse needs a profile with an SCTRL module");
    std::vector<Flash *> flashes;
    for (const ModuleSpec &m : profile.modules)
        if (auto *flash = dynamic_cast<Flash *>(machine.module(m.name))) flashes.push_back(flash);
    if (!sctrl) return;

    Efuse &efuse = sctrl->efuse();
    if (spec) {
        switch (spec->kind) {
            case EfuseSpec::Kind::File: efuse.preset(read_file(spec->file)); break;
            case EfuseSpec::Kind::Bytes: efuse.preset(spec->bytes); break;
            case EfuseSpec::Kind::Key: efuse.preset(fuses_from_key(spec->key)); break;
        }
        const FlashCipher cipher(key_from_fuses(efuse.fuses()));
        for (Flash *flash : flashes) {
            const std::vector<uint8_t> raw = flash->raw();
            if (find_vector_table(raw, cipher) || detect_key(raw).kind == KeyDetection::Kind::Blank) continue;
            log().log(Level::Warning,
                      flash->name() + ": no image in the XIP window starts with a vector table under the eFuse key " +
                          key_text(cipher.key()) + "; the firmware may not run");
        }
    } else {
        for (Flash *flash : flashes) {
            const KeyDetection found = detect_key(flash->raw());
            if (found.kind == KeyDetection::Kind::Blank) continue;
            if (found.kind == KeyDetection::Kind::Plain) {
                log().log(Level::Debug, flash->name() + ": the image at " + hex8(found.addr) + " is not encrypted");
            } else if (found.kind == KeyDetection::Kind::Known) {
                efuse.preset(fuses_from_key(found.key));
                log().log(Level::Info,
                          flash->name() + ": the image at " + hex8(found.addr) + " is encrypted with the " +
                              found.name + " key " + key_text(found.key) + "; eFuse bytes 0..15 set to it");
            } else {
                log().log(Level::Warning,
                          flash->name() + ": the XIP window from " + hex8(found.addr) +
                              " has no vector table, plain or under a known key; if the image is encrypted, give "
                              "its key with --efuse key:W0,W1,W2,W3");
            }
            break;
        }
    }
    for (Flash *flash : flashes) flash->set_key_source([sctrl] { return key_from_fuses(sctrl->efuse().fuses()); });
}

} // namespace bk
