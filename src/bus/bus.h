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
#include "core/xip.h"
#include "cpu/cpu.h"

namespace bk {

class Logger;

// A RAM region mapped straight into the CPU, and its mirrors up to `span`
// bytes from `base` (`span` is `size` when it has none).
struct MemRegion {
    std::string name;
    uint32_t    base, size;
    bool        executable;
    uint32_t    span;
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
// mapped as MMIO page by page (runs of adjacent pages merged). The window of a
// module that implements XipMemory is mapped read+execute one 4 KiB page at a
// time on first use from its xip_page() data; bytes in a page's bad spans
// abort when read or executed. Accesses nothing answers become aborts.
//
// Unmapped accesses go to one hook installed by the constructor, which offers
// them to the demand ranges first and only then reports a bus error, so the
// order of claims does not matter.
//
// A RAM region with a window wider than itself repeats over it, as a chip
// that decodes fewer address lines than its memory map has: the copies after
// the first are mirrors, mapped as MMIO that reaches the same bytes. A
// mirror access is most likely a firmware bug, so each instruction that
// makes one is reported once, as a warning.
class Bus {
  public:
    explicit Bus(Cpu &cpu);
    ~Bus();
    Bus(const Bus &)            = delete;
    Bus &operator=(const Bus &) = delete;

    // `window` (0 for `size`) is the address space the region repeats over, a
    // multiple of `size`; with mirrors, `base` and `size` must be 4 KiB
    // aligned. Throws std::invalid_argument if the window overlaps RAM or a
    // module range, or breaks those rules.
    void add_memory(std::string name, uint32_t base, uint32_t size, bool executable, uint32_t window = 0);
    // Places each of the module's ranges at bases[range name], and its XIP
    // window, when it implements XipMemory, at bases["xip"]. Throws
    // std::invalid_argument if a base is missing or a range overlaps RAM or
    // another range. Installs the module's xip_invalidate.
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

    // What a DMA engine can reach: RAM regions read and write, demand ranges
    // read-only. Accesses work in chunks that stop at the end of a region or
    // page; anything else throws BusError. The reference stays valid for the
    // lifetime of the bus.
    BusMemory &memory() { return master_; }

  private:
    class Master final : public BusMemory {
      public:
        explicit Master(Bus &bus) : bus_(bus) {}
        std::vector<uint8_t> read(uint32_t addr, uint32_t size) override;
        void                 write(uint32_t addr, std::span<const uint8_t> data) override;

      private:
        Bus &bus_;
    };
    friend class Master;

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
    // The mirrors of one RAM region: [base + size, base + span).
    struct Mirror {
        Bus        *bus;
        std::string name;
        uint32_t    base, size, span;
    };
    struct CachedPage;
    struct Demand {
        Bus                                            *bus;
        Module                                         *module;
        XipMemory                                      *xip;
        std::string                                     range; // always "xip"
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
    static uint64_t mirror_read(uc_engine *, uint64_t offset, unsigned size, void *user);
    static void     mirror_write(uc_engine *, uint64_t offset, unsigned size, uint64_t value, void *user);
    static bool     on_unmapped(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static bool     on_fetch_prot(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static bool     on_write_prot(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static void     on_page_read(uc_engine *, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user);
    static void     on_page_code(uc_engine *, uint64_t addr, uint32_t size, void *user);

    // One chunk of a bus-master access: up to `size` bytes starting at `addr`
    // that stay inside one RAM region or one demand page.
    std::vector<uint8_t> read_chunk(uint32_t addr, uint32_t size);
    size_t               write_chunk(uint32_t addr, std::span<const uint8_t> data);

    uint32_t    read(uint32_t addr, unsigned size);
    void        write(uint32_t addr, unsigned size, uint32_t value);
    const Slot *find(uint32_t addr) const;
    // The mirror holding `addr`, or nullptr.
    const Mirror *find_mirror(uint32_t addr) const;
    // The address in the region itself that mirror address `addr` reaches.
    static uint32_t unmirror(const Mirror &m, uint32_t addr) { return m.base + (addr - m.base) % m.size; }
    // Reports a mirror access of the current instruction (or of a bus master
    // when `master`) once.
    void    mirror_warning(const Mirror &m, uint32_t addr, unsigned size, bool write, bool master);
    Demand *find_demand(uint32_t addr);
    bool    demand_access(Demand &d, uint32_t addr, uint32_t size, bool fetch);
    void    abort_in_page(Demand &d, uint32_t addr, uint32_t size, Fault::Kind kind);
    void    invalidate(Demand &d, uint32_t offset, uint32_t size);
    void    bus_error(uint32_t addr, bool fetch);
    void    check_overlap(const std::string &label, uint32_t base, uint32_t size) const;
    void    map_outside(const std::vector<const Entry *> &entries);
    bool    in_window(uint32_t base, uint64_t end) const;

    Cpu                                  &cpu_;
    Logger                               &log_;
    Master                                master_{*this};
    std::vector<MemRegion>                regions_;
    std::deque<Entry>                     entries_; // claim order; stable addresses for slots_
    std::vector<Slot>                     slots_;   // sorted by base
    std::vector<std::unique_ptr<Demand>>  demands_;
    std::vector<std::unique_ptr<MmioMap>> mmio_maps_;
    std::vector<std::unique_ptr<Mirror>>  mirrors_;
    std::unordered_set<uint64_t>          mirror_warned_; // instruction or bus master page, and direction
    std::set<uint32_t>                    mmio_pages_;    // pages mapped as MMIO outside the window
    bool                                  window_set_ = false;
    uint32_t                              win_base_ = 0, win_size_ = 0;
    std::vector<uc_hook>                  hooks_;
    std::unordered_set<uint32_t>          unclaimed_read_, unclaimed_write_;
    std::unordered_set<uint32_t>          bad_fetch_pages_, bad_access_pages_, noexec_pages_;
};

} // namespace bk
