#pragma once

#include "cli/check.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::cli {

struct GameOptions {
    std::filesystem::path game_dir;  // the EU4 folder: eu4.exe and anvil/
    // EU4's user folder with dlc_load.json and mod/; empty: the one the game
    // uses (eu4_user_dir).
    std::filesystem::path user_dir;
    // The build id of an eu4.exe by its SHA-256, or nullptr when it is unknown.
    // Empty: the table compiled into Anvil.
    std::function<const char*(std::string_view sha256)> build_of;
};

// One folder of anvil/plugins.
struct GamePlugin {
    std::string folder;  // the folder's name
    CheckReport report;  // the plugin as `anvil check <folder> --exe <eu4.exe>` sees it
    // Disabled in anvil.toml (or all plugins are): its problems are listed but
    // do not count towards the exit code.
    bool disabled = false;
    std::string status;  // "loaded", "skipped" or "disabled"
};

struct LoadedPlugin {
    std::string id;
    std::string version;
    std::string phase;   // "early" or "main"
    std::string folder;  // in anvil/plugins; "" for a plugin from a mod
    std::string mod;     // the mod's name for a plugin from a mod
};

struct SkippedPlugin {
    std::string id;      // the folder's name when plugin.toml is broken
    std::string folder;
    std::string reason;
    bool disabled = false;  // by the player in anvil.toml: not a fault
    std::string mod;        // the mod's name for a plugin from a mod (folder is then "")
    i18n::Message why;      // `reason` by code, for the player's language (#158)
};

// A plugin in <mod>/anvil/<id>/ of a mod enabled in the playset (design §14).
struct GameModPlugin {
    std::string id;
    std::string mod;          // its name
    std::string descriptor;   // as dlc_load.json lists it
    std::string workshop_id;  // "" for a local mod
    std::string folder;       // <mod>/anvil/<id>
    std::string state;        // approved | update-pending | pending | conflict | shadowed | invalid
    std::string reason;
    i18n::Message why;        // `reason` by code, for the player's language (#158)
    std::string hash;         // of the folder in the mod; "" when it cannot be hashed
    std::string version;      // its plugin.toml's; "" when that is broken
    std::string problem;      // why the folder in the mod cannot be approved; "" when it can
    // trust.toml's approval of the id for this mod; empty when there is none.
    std::string approved_version, approved_hash, approved_date;
    std::string loads_from;   // the approved copy for approved and update-pending; "" otherwise
    bool copy_missing = false;  // approved, and the copy is made at the next start
    std::vector<std::string> others;  // conflict, shadowed: the other mods with this id
    std::string status;       // "loaded" or "skipped" (by the resolver, or because of `state`)
    std::string skip_reason;  // the resolver's, for an approved plugin that does not load
    i18n::Message skip_why;
    // The folder that loads (the approved copy, or the mod's folder while the
    // copy is not made), or for the others the mod's folder, as `anvil check
    // <folder> --exe <eu4.exe>` sees it.
    CheckReport report;
    // update-pending: the folder in the mod, checked the same way, since `report`
    // is the approved copy that still loads.
    std::optional<CheckReport> update;

    // The files an approval would approve.
    const CheckReport& offered() const { return update ? *update : report; }
};

// Two plugins that will load together declare code ranges that overlap.
struct OverlapWarning {
    std::string plugin;        // the one that loads first
    std::string other;
    std::string file;          // plugin's symbols.toml
    uint32_t line = 0;         // of the overlapping symbol
    std::string message;
};

struct GameReport {
    int exit_code = kExitOk;
    std::string cannot_check;  // why, with kExitCannotCheck for the whole folder
    std::string game_dir;      // as passed
    std::string exe_name = "eu4.exe";
    std::string exe_sha256;
    std::string exe_build;     // empty: not a game build this Anvil knows
    bool enabled = true;       // anvil.toml
    std::vector<std::string> disabled_plugins;
    std::vector<Problem> problems;       // anvil.toml
    std::vector<GamePlugin> plugins;     // in discovery order
    std::vector<LoadedPlugin> load_order;
    std::vector<SkippedPlugin> skipped;  // discovery first, then the resolver, as in the log
    std::vector<OverlapWarning> warnings;
    // The playset (design §14): its user folder, whether it has dlc_load.json,
    // its enabled mods, what could not be read, and the plugins of those mods.
    std::string user_dir;
    bool playset_found = false;
    size_t enabled_mods = 0;
    std::vector<std::string> mod_warnings;
    std::vector<GameModPlugin> mod_plugins;
};

// `anvil check --game <folder>` (design §16): every plugin in anvil/plugins
// checked against the folder's eu4.exe (without writing symbols.lock), then
// resolved with the loader's own rules (anvil.toml, API, Anvil range, build
// and symbols, duplicates, conflicts, dependencies, cycles), so the report
// says what loads, in which order, and what is skipped and why. The plugins
// of the playset's mods are decided as the loader does (approved, pending,
// conflicting, shadowed; design §14), checked too, and the approved ones take
// part in the resolution. Nothing is written: a missing approved copy is
// reported, not made.
GameReport check_game(const GameOptions& options);

std::string format_game_report(const GameReport& report, Format format);

}  // namespace anvil::cli
