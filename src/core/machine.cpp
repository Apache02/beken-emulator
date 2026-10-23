#include "core/machine.h"

#include <stdexcept>
#include <utility>

namespace bk {

namespace {
constexpr std::string_view kSignalSuffix = "_signal";

uint32_t source_bit(const std::string &module, const ParamValue &v) {
    const int64_t *bit = v.integer();
    if (!bit || *bit < 0 || *bit > 31)
        throw std::invalid_argument("module " + module + ": irq must be a source bit 0-31");
    return static_cast<uint32_t>(*bit);
}
} // namespace

int64_t ModuleContext::integer(const std::string &key, int64_t fallback) const {
    const ParamValue *v = params.find(key);
    if (!v) return fallback;
    if (!v->integer()) throw std::invalid_argument("module " + name + ": parameter '" + key + "' must be an integer");
    return *v->integer();
}

Signal ModuleContext::signal(const std::string &key) const {
    auto it = signals.find(key);
    return it == signals.end() ? Signal{} : it->second;
}

Machine::Machine(const Profile &profile, const ModuleRegistry &registry, EndpointMap endpoints)
    : memory_(profile.memory), aliases_(profile.alias), bus_(cpu_), scheduler_(cpu_, profile.insns_per_us),
      cpu_irq_([this](bool level) { cpu_.raise_irq(level); }), cpu_fiq_([this](bool level) { cpu_.raise_fiq(level); }) {
    for (const auto &m : profile.memory) bus_.add_memory(m.name, m.base, m.size, m.executable);
    for (const auto &spec : profile.modules) add_module(spec, registry, endpoints);
    bus_.claim_unclaimed(profile.win_base, profile.win_size);
    // A defined power-on state for the core; modules start in their own
    // initial state.
    cpu_.reset(0);
}

Module &Machine::earlier(const std::string &user, const std::string &owner) const {
    auto it = by_name_.find(owner);
    if (it == by_name_.end())
        throw std::invalid_argument("module " + user + " needs " + owner + " listed before it in the profile");
    return *it->second;
}

void Machine::add_module(const ModuleSpec &spec, const ModuleRegistry &registry, const EndpointMap &endpoints) {
    auto factory = registry.find(spec.type);
    if (factory == registry.end()) {
        std::string known;
        for (const auto &[type, f] : registry) known += (known.empty() ? "" : ", ") + type;
        throw std::invalid_argument("unknown module type '" + spec.type + "' (known: " + known + ")");
    }
    if (by_name_.count(spec.name)) throw std::invalid_argument("module " + spec.name + " is listed twice");

    ModuleContext ctx{spec.name,
                      spec.params,
                      scheduler_,
                      cpu_irq_,
                      cpu_fiq_,
                      [this] { reset(); },
                      {},
                      {},
                      {},
                      nullptr,
                      nullptr,
                      {},
                      {}};
    ctx.cpu_instruction = [this] { return cpu_.instruction(); };
    ctx.memory          = &bus_.memory();
    if (auto e = endpoints.find(spec.name); e != endpoints.end()) ctx.endpoint = e->second;

    if (const ParamValue *intc = ctx.params.find("intc")) {
        if (!intc->string()) throw std::invalid_argument("module " + spec.name + ": intc must be a module name");
        Module           &controller = earlier(spec.name, *intc->string());
        const ParamValue *irq        = ctx.params.find("irq");
        if (!irq) throw std::invalid_argument("module " + spec.name + ": intc given without irq");
        if (const ParamTable *named = irq->table()) {
            for (const auto &[line_name, bit] : named->entries)
                ctx.irqs[line_name] = controller.line(source_bit(spec.name, bit));
        } else {
            ctx.irq = controller.line(source_bit(spec.name, *irq));
        }
        ctx.params.erase("intc");
        ctx.params.erase("irq");
    } else if (ctx.params.find("irq")) {
        throw std::invalid_argument("module " + spec.name + " has irq but no intc");
    }

    if (const ParamValue *dma = ctx.params.find("dma")) {
        if (!dma->string()) throw std::invalid_argument("module " + spec.name + ": dma must be a module name");
        Module           &engine  = earlier(spec.name, *dma->string());
        const ParamValue *request = ctx.params.find("dma_request");
        if (!request || !request->integer() || *request->integer() < 0 || *request->integer() > 15)
            throw std::invalid_argument("module " + spec.name + ": dma needs dma_request 0-15");
        ctx.dma = engine.request_line(static_cast<unsigned>(*request->integer()));
        ctx.params.erase("dma");
        ctx.params.erase("dma_request");
    }

    std::vector<std::string> signal_keys;
    for (const auto &[key, value] : ctx.params.entries) {
        if (key.size() > kSignalSuffix.size() && key.ends_with(kSignalSuffix)) signal_keys.push_back(key);
    }
    for (const auto &key : signal_keys) {
        const ParamValue  *value = ctx.params.find(key);
        const std::string *ref   = value->string();
        auto               dot   = ref ? ref->find('.') : std::string::npos;
        if (dot == std::string::npos)
            throw std::invalid_argument("module " + spec.name + ": " + key + " must be \"Owner.signal\"");
        Module &owner                                                 = earlier(spec.name, ref->substr(0, dot));
        ctx.signals[key.substr(0, key.size() - kSignalSuffix.size())] = owner.signal(ref->substr(dot + 1));
        ctx.params.erase(key);
    }

    std::unique_ptr<Module> module = factory->second(ctx);
    if (!module) throw std::invalid_argument("module " + spec.name + ": factory produced nothing");
    bus_.claim(*module, spec.bases);
    by_name_[spec.name] = module.get();
    scheduler_.add(module.get());
    modules_.push_back(std::move(module));
}

Module *Machine::module(const std::string &name) {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : it->second;
}

void Machine::reset() {
    for (const auto &m : memory_) {
        if (m.reset_fill) cpu_.mem_write(m.base, std::vector<uint8_t>(m.size, *m.reset_fill));
    }
    for (auto &m : modules_) m->reset();
    cpu_.reset(0);
}

} // namespace bk
