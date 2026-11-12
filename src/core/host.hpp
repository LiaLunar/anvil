#pragma once

#include "changes.hpp"
#include "i18n/message.hpp"
#include "manifest.hpp"
#include "plugin_settings.hpp"
#include "plugin_symbols.hpp"
#include "seh_guard.hpp"
#include "services.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {
struct HostContext;
}

// The handle a plugin gets as `self`: one per resolved plugin, alive for the
// process.
struct AnvilPlugin {
    anvil::HostContext* context = nullptr;
    anvil::Manifest manifest;
    anvil::OwnerId owner = 0;      // in the change registry; ids follow the load order
    anvil::PluginSymbols symbols;  // verified symbols.toml, by AnvilSymbol
    anvil::PluginSettings settings;  // settings.toml and the player's values, by AnvilSetting
    std::wstring plugin_dir;
    // A plugin from a mod: the hash its approved copy (plugin_dir) must have
    // right before LoadLibrary (design §14); "" for a local plugin.
    std::string approved_hash;
    std::wstring data_dir;
    HMODULE module = nullptr;
    std::string state = "pending";  // pending | loaded | failed
    std::string reason;             // why it failed, for the overlay (anvil.control): the English words of `why`
    anvil::i18n::Message why;
    // Inside anvil_plugin_load the load itself is the plugin's group.
    std::atomic<bool> loading{false};

    // --- modules (ADR 0011) ---
    // A module's handle: `parent` is its plugin, whose symbols, settings,
    // folders and names it uses; its own are `owner` (the registry and the
    // gameplay policy) and `manifest`, which is module_manifest() of the
    // plugin's, so the log, the bus and anvil.toml name it "<plugin>.<module>".
    AnvilPlugin* parent = nullptr;
    std::string module_id;  // the [[modules]] id; "" for a plugin's own handle
    bool enabled = true;    // a module: on at this start (anvil.toml, `default`)
    // A plugin's modules in plugin.toml order, made with the plugin, before any
    // plugin loads, so their owners follow the plugin's in load order.
    std::vector<std::unique_ptr<AnvilPlugin>> modules;
};

namespace anvil {

// What the host table acts on. The game has one for the process
// (runtime.cpp); the tests build their own around a synthetic image.
struct HostContext {
    ChangeRegistry* changes = nullptr;
    ServiceBus* bus = nullptr;
    HMODULE image = nullptr;  // whose imports replace_import changes (eu4.exe)
    // Plugin log lines; empty: the process log (Log::instance()).
    std::function<void(const AnvilPlugin& plugin, AnvilLogLevel level, std::string_view message)> log;
};

// The API 2 host table: every field filled, `game` as given. The functions
// reach their context through `self`, so one table serves every plugin.
AnvilHost make_host(const AnvilGameInfo* game);

// The handles of `plugin`'s modules (ADR 0011), one per [[modules]] entry in
// file order: each gets its own owner in `changes`, right after the plugin's,
// and is on when `enabled(module key, default)` says so. Call it once, before
// the next plugin's owner is added.
using ModuleEnabled = std::function<bool(const std::string& key, bool enabled_by_default)>;
void add_modules(AnvilPlugin& plugin, ChangeRegistry& changes, const ModuleEnabled& enabled);

AnvilResult to_result(ChangeStatus status);
const char* result_name(AnvilResult result);

struct LoadOutcome {
    AnvilResult result = ANVIL_OK;
    unsigned long exception = 0;  // the structured exception that ended anvil_plugin_load, 0 if none
    size_t staged = 0;            // changes requested during the load
    bool commit_failed = false;   // ANVIL_OK, but the staged changes could not be enabled
};

// Calls anvil_plugin_load as its own group (design §8.4): the changes it
// requests are staged and enabled together when it returns ANVIL_OK;
// otherwise none are, and the plugin's services and subscriptions (events and
// settings) are dropped.
LoadOutcome run_plugin_load(AnvilPlugin& plugin, const AnvilHost& host, PluginLoadFn fn);

}  // namespace anvil
