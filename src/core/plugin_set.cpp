#include "plugin_set.hpp"

#include "gameplay_policy.hpp"
#include "log.hpp"
#include "plugin_hash.hpp"
#include "seh_guard.hpp"
#include "services.hpp"

#include <cstdio>

namespace anvil {

const char* outcome_name(Outcome::Kind kind) {
    switch (kind) {
    case Outcome::Kind::loaded: return "loaded";
    case Outcome::Kind::failed: return "failed";
    case Outcome::Kind::skipped: return "skipped";
    case Outcome::Kind::disabled: return "disabled";
    }
    return "pending";
}

const char* outcome_name(const Outcome* outcome) { return outcome != nullptr ? outcome_name(outcome->kind) : "pending"; }

namespace {

std::string fmt_ms(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

// Every handle of a plugin: its own, then its modules' that are on.
template <class Fn> void each_handle(const PluginRecord& plugin, Fn&& fn) {
    fn(*plugin.handle());
    for (const auto& m : plugin.modules()) {
        if (m.handle != nullptr) fn(*m.handle);
    }
}

struct LoadResult {
    AnvilResult result = ANVIL_OK;
    unsigned long exception = 0;  // the structured exception that ended anvil_plugin_load, 0 if none
    size_t staged = 0;            // changes requested during the load
    bool commit_failed = false;   // ANVIL_OK, but the staged changes could not be enabled
};

// Calls anvil_plugin_load as its own group (design §8.4): one group for each
// of the plugin's handles, so the changes requested through any of them are
// enabled when the load returns ANVIL_OK and none are when it fails (ADR
// 0011); then the plugin's services and subscriptions (events and settings)
// are dropped too.
LoadResult run_plugin_load(const PluginRecord& plugin, const AnvilHost& host, PluginLoadFn fn) {
    AnvilPlugin& self = *plugin.handle();
    ChangeRegistry& changes = *self.context->changes;
    ServiceBus& bus = *self.context->bus;
    LoadResult out;
    each_handle(plugin, [&](AnvilPlugin& h) { changes.begin_group(h.owner); });
    self.loading = true;
    out.result = call_plugin_load(fn, &host, &self, &out.exception);
    self.loading = false;
    each_handle(plugin, [&](AnvilPlugin& h) { out.staged += changes.staged_count(h.owner); });
    if (out.result == ANVIL_OK) {
        bool committed = true;
        each_handle(plugin, [&](AnvilPlugin& h) { committed = changes.commit_group(h.owner) == ChangeStatus::Ok && committed; });
        if (!committed) {
            out.result = ANVIL_ERROR_INTERNAL;
            out.commit_failed = true;
        }
    }
    if (out.result != ANVIL_OK) {
        each_handle(plugin, [&](AnvilPlugin& h) {
            changes.rollback(h.owner);
            bus.remove_owner(h.name);
        });
        plugin.settings().drop_subscriptions();
    }
    return out;
}

}  // namespace

PluginSet::PluginSet(std::vector<ResolvedPlugin> resolved, std::vector<Skipped> skipped, Dependencies dependencies)
    : deps_(std::move(dependencies)) {
    ChangeRegistry& changes = *deps_.context->changes;
    const auto add_owner = [&](AnvilPlugin& handle) {
        handle.owner = changes.add_owner(handle.name);
        if (owners_.size() <= handle.owner) owners_.resize(static_cast<size_t>(handle.owner) + 1, nullptr);
        owners_[handle.owner] = &handle;
    };
    for (ResolvedPlugin& r : resolved) {
        PluginRecord& p = records_.emplace_back(PluginRecord::Key{});
        p.manifest_ = std::move(r.manifest);
        p.symbols_ = std::move(r.symbols);
        p.plugin_dir_ = p.manifest_.folder.wstring();
        p.data_dir_ = r.data_dir.wstring();
        p.approved_hash_ = std::move(r.approved_hash);
        auto handle = std::make_unique<AnvilPlugin>();
        handle->context = deps_.context;
        handle->record = &p;
        handle->name = p.manifest_.id;
        handle->affects_gameplay = p.manifest_.affects_gameplay;
        add_owner(*handle);
        // The player's values and the ANVIL_<ID>_<NAME> overrides, before any plugin loads.
        if (r.settings) {
            const auto notes = p.settings_.init(std::move(*r.settings), r.data_dir / "settings.toml", p.manifest_.id,
                                                deps_.environment);
            for (const auto& note : notes) log(note.level, p.manifest_.id, note.message);
        }
        deps_.policy->add(p.manifest_, handle->owner);
        p.handle_ = std::move(handle);
        // Its modules (ADR 0011): an owner and a policy entry for each one
        // that is on at this start, right after the plugin's.
        const std::vector<Module>& declared = p.manifest_.modules;
        p.modules_.reserve(declared.size());
        for (size_t i = 0; i < declared.size(); ++i) {
            const Module& m = declared[i];
            PluginRecord::ModuleSlot slot;
            slot.declared = &m;
            slot.key = module_key(p.manifest_, m);
            slot.name = p.manifest_.name + ": " + m.name;
            if (i < r.modules_on.size() ? r.modules_on[i] : m.enabled_by_default) {
                auto module = std::make_unique<AnvilPlugin>();
                module->context = deps_.context;
                module->record = &p;
                module->name = slot.key;
                module->module = &m;
                module->affects_gameplay = m.affects_gameplay;
                add_owner(*module);
                deps_.policy->add(module_manifest(p.manifest_, m), module->owner);
                slot.handle = module.get();
                p.module_handles_.push_back(std::move(module));
            } else {
                log(ANVIL_LOG_INFO, "anvil", slot.key + ": module off at this start (anvil.toml)");
            }
            p.modules_.push_back(std::move(slot));
        }
    }
    resolved_ = records_.size();
    for (Skipped& s : skipped) {
        PluginRecord& p = records_.emplace_back(PluginRecord::Key{});
        p.manifest_.id = std::move(s.id);
        p.manifest_.folder = s.folder;
        p.plugin_dir_ = s.folder.wstring();
        p.mod_ = std::move(s.mod);
        p.awaiting_approval_ = s.awaiting_approval;
        Outcome o;
        o.kind = s.disabled ? Outcome::Kind::disabled : Outcome::Kind::skipped;
        o.why = std::move(s.why);
        o.reason = std::move(s.reason);
        o.skip = s.kind;
        o.by_benchmark_arm = s.benchmark_arm;
        set_outcome(p, std::move(o));
    }
    // The core's own changes: its owner comes after every plugin's, so each
    // is the innermost link of its chain.
    core_owner_ = changes.add_owner("anvil");
}

void PluginSet::log(AnvilLogLevel level, const std::string& id, const std::string& message) const {
    if (deps_.log) {
        deps_.log(level, id, message);
    } else {
        Log::instance().write(level, id, message);
    }
}

double PluginSet::now_ms() const {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart ? frequency.QuadPart : 1);
}

void PluginSet::set_outcome(PluginRecord& plugin, Outcome outcome) {
    plugin.outcome_ = std::move(outcome);
    plugin.has_outcome_.store(true, std::memory_order_release);
}

// What a failed load leaves: the outcome failed with its reason, and no
// gameplay policy entry for any of the plugin's handles. Its changes are
// already rolled back (or there never were any).
void PluginSet::fail(PluginRecord& plugin, i18n::Message why) {
    each_handle(plugin, [&](AnvilPlugin& h) { deps_.policy->forget(h.owner); });
    Outcome o;
    o.kind = Outcome::Kind::failed;
    o.reason = i18n::english(why);
    o.why = std::move(why);
    set_outcome(plugin, std::move(o));
}

void PluginSet::load(Phase phase) {
    for (size_t i = 0; i < resolved_; ++i) {
        if (records_[i].manifest().phase == phase && records_[i].outcome() == nullptr) load(records_[i]);
    }
}

void PluginSet::load(const PluginRecord& record) {
    PluginRecord& plugin = const_cast<PluginRecord&>(record);  // one of records_
    if (plugin.handle() == nullptr || plugin.outcome() != nullptr) return;
    const double begin = now_ms();
    const Manifest& manifest = plugin.manifest();
    const std::filesystem::path dll = std::filesystem::path(plugin.plugin_dir()) / manifest.entry;
    const std::string label = manifest.id + " " + manifest.version.to_string();
    // Every way a load fails ends here: the outcome, then its line in the
    // log (`line`, or "not loaded; <the reason>").
    const auto failed = [&](i18n::Message why, std::string line = {}) {
        fail(plugin, std::move(why));
        log(ANVIL_LOG_ERROR, "anvil", label + (line.empty() ? ": not loaded; " + plugin.outcome()->reason : line));
    };
    // Every required dependency loaded (they come earlier in the load order).
    for (const auto& d : manifest.dependencies) {
        if (d.optional) continue;
        const PluginRecord* dependency = find(d.id);
        if (dependency == nullptr || dependency->handle() == nullptr) continue;  // the resolver skipped this plugin then
        const Outcome* o = dependency->outcome();
        if (o == nullptr || o->kind != Outcome::Kind::loaded) return failed(i18n::Message("dependency_failed", {d.id}));
    }
    // A plugin from a mod: its approved copy must still have the approved
    // hash. Its files stay open without write or delete sharing until the
    // DLL is mapped, so nothing can change between the check and the load.
    FileLocks locks;
    if (!plugin.approved_hash().empty()) {
        const FolderHash now = hash_plugin_folder(plugin.plugin_dir(), &locks);
        if (now.sha256 != plugin.approved_hash()) {
            return failed(i18n::Message("copy_changed", {now.error.empty() ? i18n::Message("hash", {short_hash(now.sha256)})
                                                                          : i18n::Message(now.error),
                                                        short_hash(plugin.approved_hash())}));
        }
    }
    const HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    locks.release();
    if (module == nullptr) {
        const DWORD error = GetLastError();
        return failed(i18n::Message("load_error", {manifest.entry, std::to_string(error)}),
                      ": cannot load " + narrow(dll.wstring()) + " (error " + std::to_string(error) + ")");
    }
    plugin.dll_.store(module, std::memory_order_release);
    const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(module, "anvil_plugin_load"));
    if (fn == nullptr) {
        plugin.dll_.store(nullptr, std::memory_order_release);
        FreeLibrary(module);
        return failed(i18n::Message("no_load_export", {}), ": entry DLL does not export anvil_plugin_load");
    }
    // Changes requested during anvil_plugin_load are staged and enabled together
    // after it returns ANVIL_OK; on failure none are (design §8.4).
    ChangeRegistry& changes = *deps_.context->changes;
    const LoadResult result = run_plugin_load(plugin, *deps_.host, fn);
    if (result.commit_failed) {
        log(ANVIL_LOG_ERROR, "anvil", label + ": its " + std::to_string(result.staged) + " change(s) could not be enabled");
    }
    if (result.result != ANVIL_OK) {
        char code[32] = {};
        if (result.exception != 0) std::snprintf(code, sizeof(code), "0x%08lX", result.exception);
        i18n::Message why = result.exception != 0 ? i18n::Message("load_exception", {code})
                                                  : i18n::Message("load_returned", {result_name(result.result)});
        const std::string reason = i18n::english(why);
        // The DLL is not unloaded: threads it may have started could still run.
        return failed(std::move(why), ": " + reason + "; " + std::to_string(result.staged) +
                                          " change(s) rolled back; plugin stays inactive");
    }
    set_outcome(plugin, Outcome{});
    const AnvilPlugin& self = *plugin.handle();
    std::string summary = label + ": loaded (" + (manifest.phase == Phase::Early ? "early" : "main") + ", " +
                          fmt_ms(now_ms() - begin) + ", " + std::to_string(changes.count(self.owner)) + " change(s))";
    if (manifest.affects_gameplay) summary += " — declares gameplay changes";
    log(ANVIL_LOG_INFO, "anvil", summary);
    for (const auto& line : changes.describe(self.owner)) log(ANVIL_LOG_DEBUG, self.name, line);
    for (const auto& m : plugin.modules()) {
        if (m.handle == nullptr) continue;
        log(ANVIL_LOG_INFO, "anvil",
            m.key + ": module " + std::to_string(changes.count(m.handle->owner)) + " change(s)" +
                (m.handle->affects_gameplay ? " — declares gameplay changes" : ""));
        for (const auto& line : changes.describe(m.handle->owner)) log(ANVIL_LOG_DEBUG, m.key, line);
    }
}

const PluginRecord* PluginSet::find(std::string_view id) const {
    for (const PluginRecord& p : records_) {
        if (p.id() == id) return &p;
    }
    return nullptr;
}

const AnvilPlugin* PluginSet::handle_of(OwnerId owner) const {
    return owner < owners_.size() ? owners_[owner] : nullptr;
}

const PluginRecord* PluginSet::plugin_with_dll(uintptr_t base) const {
    if (base == 0) return nullptr;
    for (size_t i = 0; i < resolved_; ++i) {
        if (reinterpret_cast<uintptr_t>(records_[i].dll()) == base) return &records_[i];
    }
    return nullptr;
}

NearestSymbol PluginSet::nearest_symbol(uintptr_t address) const {
    NearestSymbol out;
    for (size_t i = 0; i < resolved_; ++i) {
        const PluginRecord& p = records_[i];
        const PluginSymbols& symbols = p.symbols();
        for (uint32_t id = 0; id < symbols.size(); ++id) {
            const PluginSymbol* symbol = symbols.get(id);
            if (symbol == nullptr || symbol->address == 0 || symbol->address > address) continue;
            if (out.symbol != nullptr && symbol->address < out.symbol->address) continue;
            if (out.symbol != nullptr && symbol->address == out.symbol->address) {
                if (out.plugin != &p) ++out.also_declared;
                continue;
            }
            out.symbol = symbol;
            out.plugin = &p;
            out.also_declared = 0;
        }
    }
    return out;
}

size_t PluginSet::count(Outcome::Kind kind) const {
    size_t n = 0;
    for (const PluginRecord& p : records_) {
        const Outcome* o = p.outcome();
        n += o != nullptr && o->kind == kind ? 1 : 0;
    }
    return n;
}

}  // namespace anvil
