#pragma once

#include "changes.hpp"
#include "host.hpp"
#include "manifest.hpp"
#include "outcome.hpp"
#include "plugin_settings.hpp"
#include "plugin_symbols.hpp"
#include "resolver.hpp"
#include "settings.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {
class GameplayPolicy;
class PluginRecord;
}  // namespace anvil

// A handle (GLOSSARY: Handle), the `self` a plugin gets: one for each plugin
// Anvil loads and one for each of its modules that is on at this start (ADR
// 0011). A plugin's own handle and its modules' have the same shape: who asks
// is the handle, whose data it is, its plugin's record. Opaque to plugins
// (include/anvil/anvil_plugin.h).
struct AnvilPlugin {
    anvil::HostContext* context = nullptr;
    anvil::PluginRecord* record = nullptr;  // its plugin's record; null only for handles a test makes
    anvil::OwnerId owner = 0;               // its number in the change registry and the gameplay policy
    std::string name;                       // the plugin's id, or "<plugin>.<module>"
    const anvil::Module* module = nullptr;  // the declared module; null for the plugin's own handle
    bool affects_gameplay = false;          // the plugin's or the module's affects_gameplay
    // Inside anvil_plugin_load, on the plugin's own handle: the load itself is
    // the plugin's group (design §8.4).
    std::atomic<bool> loading{false};
};

namespace anvil {

// What Anvil knows of one plugin at this start: its plugin.toml, verified
// symbols, settings, folders, DLL and outcome, and its handles. The plugin
// set makes it and is the only one that changes it; the rest of the core
// reads it.
class PluginRecord {
public:
    // A module of the plugin as plugin.toml declares it, with its handle when
    // it is on at this start. A module that is off has no handle.
    struct ModuleSlot {
        const Module* declared = nullptr;
        std::string key;   // "<plugin>.<module>", as the log, the bus and anvil.toml name it
        std::string name;  // "<plugin name>: <module name>", as anvil.control shows it
        AnvilPlugin* handle = nullptr;
    };

    // Only the plugin set can make one.
    class Key {
        friend class PluginSet;
        Key() = default;
    };
    explicit PluginRecord(Key) {}
    PluginRecord(const PluginRecord&) = delete;
    PluginRecord& operator=(const PluginRecord&) = delete;

    const std::string& id() const { return manifest_.id; }
    // A skipped or disabled plugin's has only its id and folder (the id may be
    // the folder's name when its plugin.toml does not parse).
    const Manifest& manifest() const { return manifest_; }
    // The plugin's own handle; null for a skipped or disabled plugin.
    AnvilPlugin* handle() const { return handle_.get(); }
    // Null while the plugin's phase has not run. Readable from any thread:
    // once set it never changes.
    const Outcome* outcome() const { return has_outcome_.load(std::memory_order_acquire) ? &outcome_ : nullptr; }
    const std::vector<ModuleSlot>& modules() const { return modules_; }
    const PluginSymbols& symbols() const { return symbols_; }
    // Its values change through the player (anvil.control); PluginSettings
    // guards them itself.
    PluginSettings& settings() const { return settings_; }
    const std::wstring& plugin_dir() const { return plugin_dir_; }
    const std::wstring& data_dir() const { return data_dir_; }
    // A plugin from a mod: the hash its approved copy must have right before
    // LoadLibrary (design §14); "" for a local plugin.
    const std::string& approved_hash() const { return approved_hash_; }
    // The entry DLL while it is loaded; null before and after a load that
    // could not run anvil_plugin_load.
    HMODULE dll() const { return dll_.load(std::memory_order_acquire); }
    // A skipped plugin from a mod (design §14): the mod's name, and whether it
    // waits for the player's approval.
    const std::string& mod() const { return mod_; }
    bool awaiting_approval() const { return awaiting_approval_; }

private:
    friend class PluginSet;

    Manifest manifest_;
    std::unique_ptr<AnvilPlugin> handle_;
    std::vector<std::unique_ptr<AnvilPlugin>> module_handles_;  // the modules that are on
    std::vector<ModuleSlot> modules_;                           // every declared module, in plugin.toml order
    PluginSymbols symbols_;
    mutable PluginSettings settings_;
    std::wstring plugin_dir_;
    std::wstring data_dir_;
    std::string approved_hash_;
    std::atomic<HMODULE> dll_{nullptr};
    std::string mod_;
    bool awaiting_approval_ = false;
    Outcome outcome_;  // written once, before has_outcome_
    std::atomic<bool> has_outcome_{false};
};

// A plugin the start resolved to load, with what the start found out about
// it: the plugin set discovers and decides nothing itself.
struct ResolvedPlugin {
    Manifest manifest;                     // its folder is the plugin's folder
    PluginSymbols symbols;                 // symbols.toml, verified against the game
    std::optional<SettingsFile> settings;  // settings.toml, parsed
    std::filesystem::path data_dir;        // anvil/data/<id>; its settings.toml holds the player's values
    std::string approved_hash;             // a plugin from a mod: its approved copy's hash
    // On or off at this start, by [[modules]] entry (anvil.toml, `default`);
    // a missing entry takes the module's default.
    std::vector<bool> modules_on;
};

// The nearest symbol a plugin declared at or below an address of the game.
struct NearestSymbol {
    const PluginSymbol* symbol = nullptr;
    const PluginRecord* plugin = nullptr;  // the first plugin, in load order, declaring it
    int also_declared = 0;                 // further plugins declaring the same address
};

// The plugin set (GLOSSARY): every plugin Anvil found at one start, the ones
// to load in load order followed by the skipped and disabled ones, each with
// its handles and its outcome. It makes the records and handles, loads a
// phase, and answers the rest of the core about them.
//
// The set is complete when the constructor returns, before any plugin's code
// runs: from then on no record is added or removed and no handle changes its
// record, so the questions the crash report asks need no lock and allocate
// nothing (crash_report.hpp). Only outcomes and DLLs are set later, by load.
class PluginSet {
public:
    // The core's own lines about loading (level, id, message).
    using LogSink = std::function<void(AnvilLogLevel level, const std::string& id, const std::string& message)>;

    struct Dependencies {
        HostContext* context = nullptr;    // the change registry and the bus the handles act on
        GameplayPolicy* policy = nullptr;  // design §13
        const AnvilHost* host = nullptr;   // the table anvil_plugin_load gets
        LogSink log;
        // For ANVIL_<ID>_<NAME> setting overrides; empty: the process's environment.
        PluginSettings::Environment environment;
    };

    // Makes a record and its handles for each plugin, in this order: the
    // plugin's owner in the change registry and its gameplay policy entry,
    // then its modules that are on, each the same, then the next plugin; the
    // core's own owner ("anvil") comes last. So an earlier plugin is outermost
    // in every chain, and the core's changes innermost. The settings get the
    // player's values here. Then a record for each skipped plugin.
    PluginSet(std::vector<ResolvedPlugin> resolved, std::vector<Skipped> skipped, Dependencies dependencies);
    PluginSet(const PluginSet&) = delete;
    PluginSet& operator=(const PluginSet&) = delete;

    // Loads every plugin of `phase` in load order: a required dependency
    // that did not load, a changed approved copy, a DLL that does not load or
    // has no anvil_plugin_load, or a load that fails each give the plugin the
    // outcome failed, with the reason in the log; a failed plugin keeps no
    // changes and leaves the gameplay policy.
    void load(Phase phase);
    // The same for one plugin that has no outcome yet. The tests load one
    // plugin at a time; the order must still be the load order.
    void load(const PluginRecord& plugin);

    // ---- answers ----
    size_t size() const { return records_.size(); }
    // Position `index` of the whole list: the plugins to load come first.
    const PluginRecord& at(size_t index) const { return records_[index]; }
    std::deque<PluginRecord>::const_iterator begin() const { return records_.begin(); }
    std::deque<PluginRecord>::const_iterator end() const { return records_.end(); }
    // How many plugins of the list are to load (they have handles).
    size_t resolved_count() const { return resolved_; }
    const PluginRecord* find(std::string_view id) const;
    // The handle with this owner in the change registry; null for the core's
    // own owner and any other.
    const AnvilPlugin* handle_of(OwnerId owner) const;
    // The plugin whose entry DLL is mapped at `base`.
    const PluginRecord* plugin_with_dll(uintptr_t base) const;
    NearestSymbol nearest_symbol(uintptr_t address) const;
    // Plugins with this outcome now.
    size_t count(Outcome::Kind kind) const;
    OwnerId core_owner() const { return core_owner_; }

private:
    void fail(PluginRecord& plugin, i18n::Message why);
    void set_outcome(PluginRecord& plugin, Outcome outcome);
    void log(AnvilLogLevel level, const std::string& id, const std::string& message) const;
    double now_ms() const;

    Dependencies deps_;
    std::deque<PluginRecord> records_;
    size_t resolved_ = 0;
    std::vector<const AnvilPlugin*> owners_;  // by OwnerId
    OwnerId core_owner_ = 0;
};

}  // namespace anvil
