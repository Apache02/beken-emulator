#include "bus/bus.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kPage     = 0x1000;
constexpr uint32_t kPageMask = ~(kPage - 1);

std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *fmt, ...) {
    char    buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

void check(uc_err err, const char *what) {
    if (err != UC_ERR_OK) throw std::runtime_error(std::string(what) + ": " + uc_strerror(err));
}

bool overlap(uint64_t base_a, uint64_t size_a, uint64_t base_b, uint64_t size_b) {
    return base_a < base_b + size_b && base_b < base_a + size_a;
}

// Defined with the demand ranges below.
bool hits(const std::vector<BadSpan> &bad, uint32_t rel, uint32_t size);

} // namespace

Bus::Bus(Cpu &cpu) : cpu_(cpu), log_(Logger::get("bus")) {
    uc_engine *uc  = cpu_.engine();
    auto       add = [&](int type, void *cb) {
        uc_hook h;
        check(uc_hook_add(uc, &h, type, cb, this, 1, 0), "add bus hook");
        hooks_.push_back(h);
    };
    add(UC_HOOK_MEM_FETCH_PROT, reinterpret_cast<void *>(&Bus::on_fetch_prot));
    add(UC_HOOK_MEM_WRITE_PROT, reinterpret_cast<void *>(&Bus::on_write_prot));
    // Offers unmapped accesses to the demand ranges before reporting a bus error.
    add(UC_HOOK_MEM_UNMAPPED, reinterpret_cast<void *>(&Bus::on_unmapped));
}

Bus::~Bus() {
    uc_engine *uc = cpu_.engine();
    for (uc_hook h : hooks_) uc_hook_del(uc, h);
    for (auto &d : demands_)
        for (auto &[addr, page] : d->pages) {
            if (page->read_hook) uc_hook_del(uc, page->read_hook);
            if (page->code_hook) uc_hook_del(uc, page->code_hook);
        }
}

void Bus::check_overlap(const std::string &label, uint32_t base, uint32_t size) const {
    for (const MemRegion &r : regions_)
        if (overlap(base, size, r.base, r.span)) throw std::invalid_argument(label + " overlaps " + r.name);
    for (const Entry &e : entries_)
        if (overlap(base, size, e.base, e.end - e.base)) throw std::invalid_argument(label + " overlaps " + e.label());
}

void Bus::add_memory(std::string name, uint32_t base, uint32_t size, bool executable, uint32_t window) {
    const uint32_t span = window ? window : size;
    if (size == 0 || span % size != 0)
        throw std::invalid_argument(name + ": the window must be a multiple of the size");
    if (uint64_t{base} + span > 0x100000000ULL) throw std::invalid_argument(name + " passes the end of memory");
    if (span != size && ((base | size) & ~kPageMask))
        throw std::invalid_argument(name + ": a region with mirrors must be 4 KiB aligned");
    check_overlap(name, base, span);
    if (window_set_ && overlap(base, span, win_base_, win_size_))
        throw std::invalid_argument(name + " overlaps the peripheral window");
    cpu_.map_ram(base, size, executable);
    if (span != size) {
        auto *m = mirrors_.emplace_back(std::make_unique<Mirror>(Mirror{this, name, base, size, span})).get();
        check(
            uc_mmio_map(cpu_.engine(), uint64_t{base} + size, span - size, &Bus::mirror_read, m, &Bus::mirror_write, m),
            "map mirrors");
    }
    regions_.push_back({std::move(name), base, size, executable, span});
}

void Bus::claim(Module &module, const std::map<std::string, uint32_t> &bases) {
    const std::string  mod_name = module.name();
    XipMemory         *xip      = dynamic_cast<XipMemory *>(&module);
    std::vector<Range> ranges   = module.ranges();
    if (xip) ranges.push_back({"xip", xip->xip_size()});

    std::vector<Entry> fresh;
    for (const Range &r : ranges) {
        auto base = bases.find(r.name);
        if (base == bases.end()) throw std::invalid_argument(mod_name + ": no base for range " + r.name);
        const bool is_demand = xip && r.name == "xip";
        fresh.push_back({base->second, uint64_t{base->second} + r.size, &module, mod_name, r.name, is_demand});
    }
    for (size_t i = 0; i < fresh.size(); ++i) {
        const Entry      &e     = fresh[i];
        const std::string label = e.label();
        check_overlap(label, e.base, static_cast<uint32_t>(e.end - e.base));
        for (size_t j = 0; j < i; ++j)
            if (overlap(e.base, e.end - e.base, fresh[j].base, fresh[j].end - fresh[j].base))
                throw std::invalid_argument(label + " overlaps " + fresh[j].label());
        if (e.demand && (e.base & ~kPageMask)) throw std::invalid_argument(label + " must start on a 4 KiB boundary");
    }

    std::vector<const Entry *> outside;
    for (Entry &e : fresh) {
        const Entry &stored = entries_.emplace_back(std::move(e));
        if (stored.demand) {
            demands_.push_back(std::make_unique<Demand>(
                Demand{this, stored.module, xip, stored.range, stored.base, stored.end, false, {}}));
            continue;
        }
        Slot slot{stored.base, stored.end, stored.module, &stored.range};
        slots_.insert(
            std::upper_bound(
                slots_.begin(), slots_.end(), slot, [](const Slot &a, const Slot &b) { return a.base < b.base; }),
            slot);
        if (window_set_ && !in_window(stored.base, stored.end)) outside.push_back(&stored);
    }
    map_outside(outside);

    if (xip) {
        xip->xip_invalidate = [this, xip](uint32_t offset, uint32_t size) {
            for (auto &d : demands_)
                if (d->xip == xip) invalidate(*d, offset, size);
        };
    }
}

bool Bus::in_window(uint32_t base, uint64_t end) const {
    return window_set_ && base >= win_base_ && end <= uint64_t{win_base_} + win_size_;
}

void Bus::claim_unclaimed(uint32_t win_base, uint32_t win_size) {
    if (window_set_) throw std::logic_error("peripheral window already set");
    if ((win_base | win_size) & ~kPageMask) throw std::invalid_argument("peripheral window must be 4 KiB aligned");
    for (const MemRegion &r : regions_)
        if (overlap(win_base, win_size, r.base, r.span))
            throw std::invalid_argument("peripheral window overlaps " + r.name);
    auto *map = mmio_maps_.emplace_back(std::make_unique<MmioMap>(MmioMap{this, win_base})).get();
    check(uc_mmio_map(cpu_.engine(), win_base, win_size, &Bus::mmio_read, map, &Bus::mmio_write, map),
          "map peripheral window");
    window_set_ = true;
    win_base_   = win_base;
    win_size_   = win_size;

    std::vector<const Entry *> outside;
    for (const Entry &e : entries_)
        if (!e.demand && !in_window(e.base, e.end)) outside.push_back(&e);
    map_outside(outside);
}

void Bus::map_outside(const std::vector<const Entry *> &entries) {
    std::set<uint64_t> pages;
    for (const Entry *e : entries)
        for (uint64_t p = e->base & kPageMask; p < e->end; p += kPage) {
            bool in_win = p >= win_base_ && p < uint64_t{win_base_} + win_size_;
            if (!in_win && !mmio_pages_.count(static_cast<uint32_t>(p))) pages.insert(p);
        }
    // Map each run of adjacent pages as one region.
    for (auto it = pages.begin(); it != pages.end();) {
        const uint64_t start = *it;
        uint64_t       end   = start + kPage;
        for (++it; it != pages.end() && *it == end; ++it) end += kPage;
        auto *map =
            mmio_maps_.emplace_back(std::make_unique<MmioMap>(MmioMap{this, static_cast<uint32_t>(start)})).get();
        check(uc_mmio_map(cpu_.engine(), start, end - start, &Bus::mmio_read, map, &Bus::mmio_write, map),
              "map module pages");
        for (uint64_t p = start; p < end; p += kPage) mmio_pages_.insert(static_cast<uint32_t>(p));
    }
}

std::vector<RangeInfo> Bus::module_ranges() const {
    std::vector<RangeInfo> out;
    out.reserve(entries_.size());
    for (const Entry &e : entries_)
        out.push_back({e.module_name, e.range, e.base, static_cast<uint32_t>(e.end - e.base)});
    return out;
}

void Bus::write_memory(uint32_t addr, std::span<const uint8_t> data) {
    // Validate the whole span first so a failed write changes nothing.
    for (int pass = 0; pass < 2; ++pass) {
        uint64_t a    = addr;
        size_t   done = 0;
        while (done < data.size()) {
            auto r = std::find_if(regions_.begin(), regions_.end(), [&](const MemRegion &m) {
                return a >= m.base && a < uint64_t{m.base} + m.size;
            });
            if (r == regions_.end())
                throw std::invalid_argument(
                    format("write of %zu bytes at 0x%08X reaches outside RAM", data.size(), addr));
            size_t chunk = std::min<uint64_t>(data.size() - done, uint64_t{r->base} + r->size - a);
            if (pass == 1) {
                cpu_.mem_write(static_cast<uint32_t>(a), data.subspan(done, chunk));
                // Code translated from the old bytes must not run again.
                check(uc_ctl_remove_cache(cpu_.engine(), a, a + chunk), "drop translations");
            }
            done += chunk;
            a += chunk;
        }
    }
}

// ---- Bus master access ----

BusError::BusError(uint32_t a, const std::string &reason)
    : std::runtime_error(format("bus error at 0x%08X: ", a) + reason), addr(a) {}

std::vector<uint8_t> Bus::read_chunk(uint32_t addr, uint32_t size) {
    for (const MemRegion &r : regions_) {
        if (addr >= r.base && addr < uint64_t{r.base} + r.size) {
            uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(size, uint64_t{r.base} + r.size - addr));
            return cpu_.mem_read(addr, n);
        }
    }
    if (const Mirror *m = find_mirror(addr)) {
        mirror_warning(*m, addr, size, false, true);
        // Up to the end of this copy; the next chunk starts the next one.
        const uint32_t target = unmirror(*m, addr);
        return cpu_.mem_read(target, std::min<uint32_t>(size, m->base + m->size - target));
    }
    Demand *d = find_demand(addr);
    if (!d) throw BusError(addr, "nothing readable there");
    const uint32_t offset   = addr - d->base;
    const uint32_t page_off = offset & kPageMask;
    const uint32_t n        = std::min<uint32_t>(size, page_off + kPage - offset);
    Page           page     = d->xip->xip_page(page_off);
    if (hits(page.bad, offset - page_off, n)) {
        try {
            d->xip->xip_read(offset, n); // logs the details and raises the fault
        } catch (const Fault &) {
        }
        throw BusError(addr, "unreadable block");
    }
    return std::vector<uint8_t>(page.data.begin() + (offset - page_off), page.data.begin() + (offset - page_off) + n);
}

size_t Bus::write_chunk(uint32_t addr, std::span<const uint8_t> data) {
    for (const MemRegion &r : regions_) {
        if (addr >= r.base && addr < uint64_t{r.base} + r.size) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(data.size(), uint64_t{r.base} + r.size - addr));
            cpu_.mem_write(addr, data.first(n));
            // Code translated from the old bytes must not run again.
            check(uc_ctl_remove_cache(cpu_.engine(), addr, uint64_t{addr} + n), "drop translations");
            return n;
        }
    }
    if (const Mirror *m = find_mirror(addr)) {
        mirror_warning(*m, addr, static_cast<unsigned>(data.size()), true, true);
        const uint32_t target = unmirror(*m, addr);
        const size_t   n      = std::min<size_t>(data.size(), m->base + m->size - target);
        return write_chunk(target, data.first(n));
    }
    throw BusError(addr, "nothing writable there");
}

std::vector<uint8_t> Bus::Master::read(uint32_t addr, uint32_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    uint64_t a = addr;
    while (size) {
        std::vector<uint8_t> chunk = bus_.read_chunk(static_cast<uint32_t>(a), size);
        out.insert(out.end(), chunk.begin(), chunk.end());
        a += chunk.size();
        size -= static_cast<uint32_t>(chunk.size());
    }
    return out;
}

void Bus::Master::write(uint32_t addr, std::span<const uint8_t> data) {
    uint64_t a = addr;
    while (!data.empty()) {
        size_t n = bus_.write_chunk(static_cast<uint32_t>(a), data);
        a += n;
        data = data.subspan(n);
    }
}

// ---- MMIO dispatch (hot path) ----

const Bus::Slot *Bus::find(uint32_t addr) const {
    auto it =
        std::upper_bound(slots_.begin(), slots_.end(), addr, [](uint32_t a, const Slot &s) { return a < s.base; });
    if (it == slots_.begin()) return nullptr;
    --it;
    return addr < it->end ? &*it : nullptr;
}

uint32_t Bus::read(uint32_t addr, unsigned size) {
    const Slot *s = find(addr);
    if (!s) {
        if (unclaimed_read_.insert(addr).second)
            log_.log(Level::Warning, format("unclaimed peripheral read at 0x%08X", addr));
        return 0;
    }
    // A register answers every read with its whole word and the core picks the bytes it asked for
    // (measured on the board, for every offset in the word).
    const unsigned lane = addr & 3;
    try {
        const uint32_t word = s->module->read(*s->range, (addr & ~3u) - s->base, 4);
        if (size >= 4) return word;
        return (word >> (8 * lane)) & ((1u << (8 * size)) - 1);
    } catch (const Fault &f) {
        cpu_.post_fault(f.kind);
    } catch (const std::exception &e) {
        // Exceptions must not unwind through Unicorn: report and halt.
        log_.log(Level::Error, *s->range + " read failed: " + e.what());
        cpu_.stop();
    }
    return 0;
}

void Bus::write(uint32_t addr, unsigned size, uint32_t value) {
    const Slot *s = find(addr);
    if (!s) {
        if (unclaimed_write_.insert(addr).second)
            log_.log(Level::Warning, format("unclaimed peripheral write at 0x%08X", addr));
        return;
    }
    // A register is written as a whole word with no byte enables: the core repeats a byte or
    // halfword on every byte lane, and the address only picks the lane inside the core (measured on
    // the board: a store at +1, +2 or +3 reaches the same register).
    if (size == 1)
        value = (value & 0xFF) * 0x01010101u;
    else if (size == 2)
        value = (value & 0xFFFF) * 0x00010001u;
    try {
        s->module->write(*s->range, (addr & ~3u) - s->base, 4, value);
    } catch (const Fault &f) {
        cpu_.post_fault(f.kind);
    } catch (const std::exception &e) {
        log_.log(Level::Error, *s->range + " write failed: " + e.what());
        cpu_.stop();
    }
}

uint64_t Bus::mmio_read(uc_engine *, uint64_t offset, unsigned size, void *user) {
    auto *m = static_cast<MmioMap *>(user);
    return m->bus->read(m->base + static_cast<uint32_t>(offset), size);
}

void Bus::mmio_write(uc_engine *, uint64_t offset, unsigned size, uint64_t value, void *user) {
    auto          *m    = static_cast<MmioMap *>(user);
    const uint32_t mask = size >= 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
    m->bus->write(m->base + static_cast<uint32_t>(offset), size, static_cast<uint32_t>(value) & mask);
}

// ---- Mirrors ----

const Bus::Mirror *Bus::find_mirror(uint32_t addr) const {
    for (const auto &m : mirrors_)
        if (addr >= uint64_t{m->base} + m->size && addr < uint64_t{m->base} + m->span) return m.get();
    return nullptr;
}

void Bus::mirror_warning(const Mirror &m, uint32_t addr, unsigned size, bool write, bool master) {
    const uint32_t pc  = cpu_.reg(Reg::PC);
    const uint64_t key = uint64_t{master ? addr >> 12 : pc} << 2 | uint64_t{master} << 1 | write;
    if (!mirror_warned_.insert(key).second) return;
    const uint32_t target = unmirror(m, addr);
    const char    *what   = write ? "write" : "read";
    if (master)
        log_.log(Level::Warning,
                 format("bus master %s of %u bytes at 0x%08X is in a mirror of %s: it reaches 0x%08X",
                        what,
                        size,
                        addr,
                        m.name.c_str(),
                        target));
    else
        log_.log(Level::Warning,
                 format("%s of %u bytes at 0x%08X is in a mirror of %s: it reaches 0x%08X; pc 0x%08X",
                        what,
                        size,
                        addr,
                        m.name.c_str(),
                        target,
                        pc));
}

// The bytes of one access may run past the end of a copy into the next one,
// which starts over at the region's base.
uint64_t Bus::mirror_read(uc_engine *, uint64_t offset, unsigned size, void *user) {
    auto          *m    = static_cast<Mirror *>(user);
    const uint32_t addr = m->base + m->size + static_cast<uint32_t>(offset);
    m->bus->mirror_warning(*m, addr, size, false, false);
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= uint64_t{m->bus->cpu_.mem_read(unmirror(*m, addr + i), 1)[0]} << (8 * i);
    return value;
}

void Bus::mirror_write(uc_engine *, uint64_t offset, unsigned size, uint64_t value, void *user) {
    auto          *m    = static_cast<Mirror *>(user);
    const uint32_t addr = m->base + m->size + static_cast<uint32_t>(offset);
    m->bus->mirror_warning(*m, addr, size, true, false);
    for (unsigned i = 0; i < size; ++i) {
        const uint32_t target = unmirror(*m, addr + i);
        const uint8_t  byte   = static_cast<uint8_t>(value >> (8 * i));
        m->bus->cpu_.mem_write(target, std::span<const uint8_t>(&byte, 1));
        // Code translated from the old byte must not run again. Nothing may
        // throw through Unicorn, and the region is mapped, so no check.
        uc_ctl_remove_cache(m->bus->cpu_.engine(), target, uint64_t{target} + 1);
    }
}

// ---- Faults ----

void Bus::bus_error(uint32_t addr, bool fetch) {
    auto &seen = fetch ? bad_fetch_pages_ : bad_access_pages_;
    if (seen.insert(addr >> 12).second)
        log_.log(Level::Error,
                 format(fetch ? "bus error: unmapped fetch at 0x%08X" : "bus error: unmapped access at 0x%08X", addr));
    cpu_.post_fault(fetch ? Fault::Kind::Pabt : Fault::Kind::Dabt);
}

bool Bus::on_unmapped(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t, void *user) {
    auto      *bus   = static_cast<Bus *>(user);
    const auto a     = static_cast<uint32_t>(addr);
    const bool fetch = type == UC_MEM_FETCH_UNMAPPED;
    if (Demand *d = bus->find_demand(a)) return bus->demand_access(*d, a, static_cast<uint32_t>(size), fetch);
    bus->bus_error(a, fetch);
    return false;
}

bool Bus::on_fetch_prot(uc_engine *, uc_mem_type, uint64_t addr, int, int64_t, void *user) {
    auto      *bus = static_cast<Bus *>(user);
    const auto a   = static_cast<uint32_t>(addr);
    if (bus->noexec_pages_.insert(a >> 12).second)
        bus->log_.log(Level::Error, format("bus error: fetch from non-executable memory at 0x%08X", a));
    bus->cpu_.post_fault(Fault::Kind::Pabt);
    return false;
}

bool Bus::on_write_prot(uc_engine *, uc_mem_type, uint64_t addr, int, int64_t, void *user) {
    auto      *bus = static_cast<Bus *>(user);
    const auto a   = static_cast<uint32_t>(addr);
    Demand    *d   = bus->find_demand(a);
    if (!d) return false;
    if (!d->warned) {
        d->warned = true;
        bus->log_.log(
            Level::Warning,
            format("write to read-only %s.%s ignored at 0x%08X", d->module->name().c_str(), d->range.c_str(), a));
    }
    return true; // swallowed
}

// ---- Demand ranges ----

namespace {

bool hits(const std::vector<BadSpan> &bad, uint32_t rel, uint32_t size) {
    for (const BadSpan &s : bad)
        if (rel < s.end && s.start < rel + size) return true;
    return false;
}

} // namespace

Bus::Demand *Bus::find_demand(uint32_t addr) {
    for (auto &d : demands_)
        if (addr >= d->base && addr < d->end) return d.get();
    return nullptr;
}

void Bus::abort_in_page(Demand &d, uint32_t addr, uint32_t size, Fault::Kind kind) {
    // The module logs the details and raises the fault; only its kind is replaced.
    try {
        d.xip->xip_read(addr - d.base, size);
    } catch (const Fault &) {
    } catch (const std::exception &e) {
        log_.log(Level::Error, d.range + " read failed: " + e.what());
    }
    cpu_.post_fault(kind);
}

bool Bus::demand_access(Demand &d, uint32_t addr, uint32_t size, bool fetch) {
    const Fault::Kind kind      = fetch ? Fault::Kind::Pabt : Fault::Kind::Dabt;
    const uint32_t    page_addr = addr & kPageMask;
    Page              page;
    try {
        page = d.xip->xip_page(page_addr - d.base);
    } catch (const Fault &) {
        cpu_.post_fault(kind);
        return false;
    } catch (const std::exception &e) {
        log_.log(Level::Error, d.range + " page failed: " + e.what());
        cpu_.stop();
        return false;
    }
    if (hits(page.bad, addr - page_addr, size ? size : 1)) {
        abort_in_page(d, addr, size, kind);
        return false;
    }

    uc_engine *uc = cpu_.engine();
    if (uc_mem_map(uc, page_addr, kPage, UC_PROT_READ | UC_PROT_EXEC) != UC_ERR_OK ||
        uc_mem_write(uc, page_addr, page.data.data(), kPage) != UC_ERR_OK) {
        bus_error(addr, fetch);
        return false;
    }
    auto cached = std::make_unique<CachedPage>(CachedPage{&d, page_addr, std::move(page.bad)});
    if (!cached->bad.empty()) {
        const uint64_t last = page_addr + kPage - 1;
        uc_hook_add(uc,
                    &cached->read_hook,
                    UC_HOOK_MEM_READ,
                    reinterpret_cast<void *>(&Bus::on_page_read),
                    cached.get(),
                    page_addr,
                    last);
        uc_hook_add(uc,
                    &cached->code_hook,
                    UC_HOOK_CODE,
                    reinterpret_cast<void *>(&Bus::on_page_code),
                    cached.get(),
                    page_addr,
                    last);
        // Blocks translated before these hooks existed would not call them.
        cpu_.request_restart();
    }
    d.pages[page_addr] = std::move(cached);
    return true;
}

void Bus::on_page_read(uc_engine *, uc_mem_type, uint64_t addr, int size, int64_t, void *user) {
    auto      *p = static_cast<CachedPage *>(user);
    const auto a = static_cast<uint32_t>(addr);
    if (hits(p->bad, a - p->addr, static_cast<uint32_t>(size)))
        p->demand->bus->abort_in_page(*p->demand, a, static_cast<uint32_t>(size), Fault::Kind::Dabt);
}

void Bus::on_page_code(uc_engine *, uint64_t addr, uint32_t size, void *user) {
    auto      *p = static_cast<CachedPage *>(user);
    const auto a = static_cast<uint32_t>(addr);
    if (hits(p->bad, a - p->addr, size)) p->demand->bus->abort_in_page(*p->demand, a, size, Fault::Kind::Pabt);
}

void Bus::invalidate(Demand &d, uint32_t offset, uint32_t size) {
    if (size == 0) return;
    const uint64_t start = (uint64_t{d.base} + offset) & kPageMask;
    const uint64_t end   = (uint64_t{d.base} + offset + size + kPage - 1) & ~uint64_t{kPage - 1};
    uc_engine     *uc    = cpu_.engine();
    for (auto it = d.pages.lower_bound(static_cast<uint32_t>(start)); it != d.pages.end() && it->first < end;) {
        CachedPage &p = *it->second;
        if (p.read_hook) uc_hook_del(uc, p.read_hook);
        if (p.code_hook) uc_hook_del(uc, p.code_hook);
        check(uc_mem_unmap(uc, p.addr, kPage), "unmap demand page");
        it = d.pages.erase(it);
    }
}

} // namespace bk
