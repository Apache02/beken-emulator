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
        if (overlap(base, size, r.base, r.size)) throw std::invalid_argument(label + " overlaps " + r.name);
    for (const Entry &e : entries_)
        if (overlap(base, size, e.base, e.end - e.base)) throw std::invalid_argument(label + " overlaps " + e.label());
}

void Bus::add_memory(std::string name, uint32_t base, uint32_t size, bool executable) {
    check_overlap(name, base, size);
    if (window_set_ && overlap(base, size, win_base_, win_size_))
        throw std::invalid_argument(name + " overlaps the peripheral window");
    cpu_.map_ram(base, size, executable);
    regions_.push_back({std::move(name), base, size, executable});
}

void Bus::claim(Module &module, const std::map<std::string, uint32_t> &bases) {
    const std::string        mod_name = module.name();
    const std::vector<Range> demand   = module.demand_ranges();
    std::vector<Range>       ranges   = module.ranges();
    for (const Range &d : demand)
        if (std::none_of(ranges.begin(), ranges.end(), [&](const Range &r) { return r.name == d.name; }))
            ranges.push_back(d);

    std::vector<Entry> fresh;
    for (const Range &r : ranges) {
        auto base = bases.find(r.name);
        if (base == bases.end()) throw std::invalid_argument(mod_name + ": no base for range " + r.name);
        bool is_demand = std::any_of(demand.begin(), demand.end(), [&](const Range &d) { return d.name == r.name; });
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
                Demand{this, stored.module, stored.range, stored.base, stored.end, false, {}}));
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

    Module *m         = &module;
    module.invalidate = [this, m](const std::string &range, uint32_t offset, uint32_t size) {
        for (auto &d : demands_)
            if (d->module == m && d->range == range) invalidate(*d, offset, size);
    };
}

bool Bus::in_window(uint32_t base, uint64_t end) const {
    return window_set_ && base >= win_base_ && end <= uint64_t{win_base_} + win_size_;
}

void Bus::claim_unclaimed(uint32_t win_base, uint32_t win_size) {
    if (window_set_) throw std::logic_error("peripheral window already set");
    if ((win_base | win_size) & ~kPageMask) throw std::invalid_argument("peripheral window must be 4 KiB aligned");
    for (const MemRegion &r : regions_)
        if (overlap(win_base, win_size, r.base, r.size))
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
    try {
        return s->module->read(*s->range, addr - s->base, size);
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
    try {
        s->module->write(*s->range, addr - s->base, size, value);
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
        d.module->read(d.range, addr - d.base, size);
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
        page = d.module->page(d.range, page_addr - d.base);
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
