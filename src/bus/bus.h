#pragma once

#include <unicorn/unicorn.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/module.h"
#include "cpu/cpu.h"

namespace bk {

class Logger;

// A RAM region mapped straight into the CPU.
struct MemRegion {
    std::string name;
    uint32_t    base, size;
    bool        executable;
};

// A module range claimed at a base address.
struct RangeInfo {
    std::string module, range;
    uint32_t    base, size;
};

// Connects the CPU to memory and devices.
//
// RAM regions are mapped directly. Module ranges are dispatched by address:
// the peripheral window is a single MMIO region, and ranges outside it are
// mapped as MMIO page by page (runs of adjacent pages merged). Demand ranges
// (a module's demand_ranges()) are mapped read+execute one 4 KiB page at a
// time on first use from the module's page() data; bytes in a page's bad spans
// abort when read or executed. Accesses nothing answers become aborts.
//
// Unmapped accesses go to one hook installed by the constructor, which offers
// them to the demand ranges first and only then reports a bus error, so the
// order of claims does not matter.
class Bus {
  public:
    explicit Bus(Cpu &cpu);
    ~Bus();
    Bus(const Bus &)            = delete;
    Bus &operator=(const Bus &) = delete;

    // Throws std::invalid_argument if the region overlaps RAM or a module range.
    void add_memory(std::string name, uint32_t base, uint32_t size, bool executable);
    // Places each of the module's ranges (and demand ranges) at bases[range
    // name]. Throws std::invalid_argument if a base is missing or a range
    // overlaps RAM or another range. Installs module.invalidate.
    void claim(Module &module, const std::map<std::string, uint32_t> &bases);
    // Maps the peripheral window [win_base, win_base + win_size) as one MMIO
    // region (addresses no range claims read 0 and ignore writes) and maps the
    // pages of ranges outside it. Ranges claimed later are mapped as they come.
    void claim_unclaimed(uint32_t win_base, uint32_t win_size);

    std::vector<MemRegion> memory_regions() const { return regions_; }
    std::vector<RangeInfo> module_ranges() const;
    // Writes straight into RAM; throws std::invalid_argument if any byte lies
    // outside the RAM regions.
    void write_memory(uint32_t addr, std::span<const uint8_t> data);

  private:
    struct Entry {
        uint32_t    base;
        uint64_t    end; // exclusive
        Module     *module;
        std::string module_name, range;
        bool        demand;
        std::string label() const { return module_name + "." + range; }
    };
    // Hot-path view of a non-demand entry, kept sorted by base.
    struct Slot {
        uint32_t           base;
        uint64_t           end;
        Module            *module;
        const std::string *range;
    };
    struct MmioMap {
        Bus     *bus;
        uint32_t base;
    };
    struct CachedPage;
    struct Demand {
        Bus                                            *bus;
        Module                                         *module;
        std::string                                     range;
        uint32_t                                        base;
        uint64_t                                        end;
        bool                                            warned = false;
        std::map<uint32_t, std::unique_ptr<CachedPage>> pages; // by page address
    };
    struct CachedPage {
        Demand              *demand;
        uint32_t             addr;
        std::vector<BadSpan> bad;
        uc_hook              read_hook = 0, code_hook = 0;
    };

    static uint64_t mmio_read(uc_engine *, uint64_t offset, unsigned size, void *user);
    static void     mmio_write(uc_engine *, uint64_t offset, unsigned size, uint64_t value, void *user);
    static bool     on_unmapped(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static bool     on_fetch_prot(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static bool     on_write_prot(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static void     on_page_read(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static void     on_page_code(uc_engine *, uint64_t addr, uint32_t size, void *user);

    uint32_t    read(uint32_t addr, unsigned size);
    void        write(uint32_t addr, unsigned size, uint32_t value);
    const Slot *find(uint32_t addr) const;
    Demand     *find_demand(uint32_t addr);
    bool        demand_access(Demand &d, uint32_t addr, uint32_t size, bool fetch);
    void        abort_in_page(Demand &d, uint32_t addr, uint32_t size, Fault::Kind kind);
    void        invalidate(Demand &d, uint32_t offset, uint32_t size);
    void        bus_error(uint32_t addr, bool fetch);
    void        check_overlap(const std::string &label, uint32_t base, uint32_t size) const;
    void        map_outside(const std::vector<const Entry *> &entries);
    bool        in_window(uint32_t base, uint64_t end) const;

    Cpu                                  &cpu_;
    Logger                               &log_;
    std::vector<MemRegion>                regions_;
    std::deque<Entry>                     entries_; // claim order; stable addresses for slots_
    std::vector<Slot>                     slots_;   // sorted by base
    std::vector<std::unique_ptr<Demand>>  demands_;
    std::vector<std::unique_ptr<MmioMap>> mmio_maps_;
    std::set<uint32_t>                    mmio_pages_; // pages mapped as MMIO outside the window
    bool                                  window_set_ = false;
    uint32_t                              win_base_ = 0, win_size_ = 0;
    std::vector<uc_hook>                  hooks_;
    std::unordered_set<uint32_t>          unclaimed_read_, unclaimed_write_;
    std::unordered_set<uint32_t>          bad_fetch_pages_, bad_access_pages_, noexec_pages_;
};

} // namespace bk
