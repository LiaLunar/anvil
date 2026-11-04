#pragma once

#include "benchmark_report.hpp"
#include "cli/game.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::manager {

// What the Manager reads before and during a start-up benchmark series
// (docs/spec/benchmark-v1.md §3, §7): the plugins it can compare, the
// preconditions, the environment that must not change, and the system,
// mods and plugin copies the report describes.

// The first Anvil with the game side of the benchmark (the arm, the start
// record, the quit after ready).
inline constexpr const char* kBenchmarkAnvil = "1.4.0";

// A plugin the series can compare: enabled and loading, from anvil/plugins
// or approved from a mod.
struct ComparablePlugin {
    std::string id;
    std::string version;
    std::string source = "local";  // local | workshop
    std::string workshop_item;
    std::filesystem::path dll;     // the entry DLL of the copy that loads
    bool update_pending = false;   // a mod has a newer copy that waits for approval
};

// Every plugin that loads in a normal start, but anvil's standard plugins
// (anvil-game, anvil-overlay), which are not compared. In load order.
std::vector<ComparablePlugin> comparable_plugins(const cli::GameReport& report);

// Where every plugin that may load came from, with the SHA-256 of its
// loaded copy's entry DLL, for the report's versions.
std::vector<ReportPluginSource> plugin_sources(const cli::GameReport& report);

// ---- preconditions --------------------------------------------------------------------

struct SetupFacts {
    bool needs_steam = true;            // false in a GOG install, which starts without Steam
    bool steam_running = false;
    bool eu4_running = false;           // any eu4.exe
    unsigned unfinished_starts = 0;     // anvil/data/anvil/unfinished-starts.txt
    bool on_battery = false;
    std::string installed_anvil;        // the version of the installed version.dll; "" when none
    bool anvil_game_loads = false;
    std::string plugin;                 // the chosen plugin's id
    std::optional<ComparablePlugin> comparable;  // it, when it is one the series can compare
};

struct SetupCheck {
    std::string text;  // what holds, or what does not
    bool ok = false;
    bool blocks = true;  // false: a warning only
    std::string fix;     // in words, when not ok
};

SetupFacts read_setup_facts(const std::filesystem::path& game_dir, const cli::GameReport& report,
                            const std::string& installed_anvil, const std::string& plugin);
// The ✓ / ✕ list of the setup screen, in its order.
std::vector<SetupCheck> setup_checks(const SetupFacts& facts);
bool checks_pass(const std::vector<SetupCheck>& checks);

bool steam_running();
bool any_eu4_running();

// ---- the environment of a series ------------------------------------------------------

// Recorded when the series starts and read again before and after every
// start; any change stops the series. Kept locally, never in a report.
struct SeriesEnvironment {
    bool steam_running = false;
    std::string dlc_load_sha256;  // the mod set; "" without dlc_load.json
    std::string plugin_version;   // the compared plugin's plugin.toml
    std::string plugin_sha256;    // its entry DLL
    bool operator==(const SeriesEnvironment&) const = default;
};

// `steam`: whether Steam counts; a GOG install starts without it, so Steam's
// comings and goings do not end a series there.
SeriesEnvironment read_environment(const std::filesystem::path& user_dir, const ComparablePlugin& plugin, bool steam = true);
// Which part changed, in words; "" when nothing did.
std::string environment_change(const SeriesEnvironment& before, const SeriesEnvironment& now);

// ---- the system and the mods (§7) ---------------------------------------------------

// nvme | sata_ssd | hdd | other | unknown, for the drive that holds `path`.
std::string drive_kind(const std::filesystem::path& path);

// The game's Workshop folder: the folder of an enabled Workshop mod's item,
// or <library>/steamapps/workshop/content/236850 beside the game.
std::filesystem::path workshop_folder(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir);

ReportSystem read_system(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir);

// The enabled mods of the playset: Workshop ids, local mods as a count, and
// their folders' files and size, rounded (files to 100, size to 100 MB).
ReportMods read_mods(const std::filesystem::path& user_dir);

}  // namespace anvil::manager
