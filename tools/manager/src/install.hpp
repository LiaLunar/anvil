#pragma once

#include "zip.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil::manager {

// The Anvil package the Manager carries (issue #59): version.dll and anvil/
// as `cmake --install --component anvil` lays them out, of the Manager's own
// version. Every file belongs to Anvil except anvil/anvil.toml, which is the
// player's once it exists.
struct Package {
    ZipArchive zip;
    std::string version;
    struct File {
        std::string path;    // as in the zip: "version.dll", "anvil/plugins/anvil-game/plugin.toml"
        std::string sha256;  // upper-case hex
    };
    std::vector<File> files;
    std::vector<std::string> standard_plugins;  // the folders of anvil/plugins/ in the package
};

inline constexpr const char* kPlayerToml = "anvil/anvil.toml";
inline constexpr const char* kManagerExe = "AnvilManager.exe";

// Checks that `zip` is a package (version.dll, anvil/anvil.toml, at least one
// standard plugin) and hashes its files.
std::optional<Package> load_package(ZipArchive zip, std::string version, std::string& error);

// ProductName and ProductVersion of a DLL's version resource; nullopt when
// it has none or cannot be read. Read as data: nothing in it runs.
struct DllProduct {
    std::string name;
    std::string version;
};
std::optional<DllProduct> read_dll_product(const std::filesystem::path& dll);

// The Machine of a PE file's header (0x014C: 32-bit x86, 0x8664: x64);
// nullopt when it is not a PE file.
std::optional<uint16_t> pe_machine(const std::filesystem::path& file);

// A legacy text patch (lab#104, GLOSSARY.md): EU4DLL or EU4 Unicode Patch, a
// version.dll loader with its files, which Anvil replaces. Recognized by its
// files, not by release hashes.
struct LegacyPatch {
    enum class Kind { Eu4dll, UnicodePatch };
    Kind kind = Kind::Eu4dll;
    // Its files and folders in the game folder other than version.dll,
    // '/'-separated: "plugins/plugin64.dll", "gfx/fonts/eu4-unicode".
    std::vector<std::string> files;
};
const char* legacy_patch_name(LegacyPatch::Kind kind);  // "EU4DLL", "EU4 Unicode Patch"

// Where a replace keeps what it moves out of the game folder: outside anvil/,
// so removing everything of Anvil keeps it.
inline constexpr const char* kLegacyBackup = "anvil-legacy-backup";

// A folder of anvil-legacy-backup/: what one replace moved there, laid out as
// in the game folder.
struct LegacyBackup {
    std::string folder;              // "anvil-legacy-backup/eu4dll-2026-11-09"
    std::string name;                // what it holds: "EU4DLL", "EU4DLL + EU4 Unicode Patch", "version.dll"
    std::vector<std::string> files;  // below the folder: "version.dll", "plugins/plugin64.dll"
    // Of `files`, those whose place in the game folder holds another file
    // (Anvil's version.dll not counted: uninstalling removes it).
    std::vector<std::string> taken;
};

// What a game folder holds, compared with the package.
struct Installation {
    enum class Dll { Missing, Anvil, Foreign };
    Dll dll = Dll::Missing;
    std::string dll_version;      // Anvil's version.dll: its ProductVersion
    // With a foreign version.dll: the legacy text patches its folder has
    // (EU4DLL first); empty for a proxy Anvil does not know.
    std::vector<LegacyPatch> legacy;
    // Files below <EU4>/plugins that no recognized patch has; they stay.
    std::vector<std::string> other_plugins;
    std::vector<std::string> backup_folders;  // the folders of anvil-legacy-backup/, by name
    std::optional<LegacyBackup> backup;       // the newest of them
    bool anvil_folder = false;    // <EU4>/anvil exists
    bool manager_copy = false;    // <EU4>/AnvilManager.exe exists
    bool manager_current = false;  // ... and has the bytes of the running Manager
    std::vector<std::string> missing;    // package files that are not there (anvil.toml included)
    std::vector<std::string> differing;  // package files with other bytes (not anvil.toml)
    std::vector<std::string> extra;      // files in a standard plugin's folder the package does not have

    bool installed() const { return dll == Dll::Anvil; }
    // A legacy text patch replaced Anvil's version.dll (its instructions may say
    // to reinstall it): the anvil folder with the player's plugins is still there.
    bool legacy_over_anvil() const { return dll == Dll::Foreign && !legacy.empty() && anvil_folder; }
    // "EU4DLL", "EU4 Unicode Patch" or both joined by " + "; empty without one.
    std::string legacy_name() const;
    // Anvil of the package's version with every file as the package has it.
    bool intact() const;
};

// `manager_exe` is the running AnvilManager.exe (empty: not compared).
Installation inspect_installation(const Package& package, const std::filesystem::path& game_dir,
                                  const std::filesystem::path& manager_exe = {});

struct Step {
    enum class Kind {
        Write,          // a package file
        CopyManager,    // the running AnvilManager.exe to <EU4>/AnvilManager.exe
        Remove,         // a file
        RemoveTree,     // a folder with everything in it
        RemoveIfEmpty,  // a folder, when nothing is left in it
        RemoveManager,  // <EU4>/AnvilManager.exe, possibly the running exe
        Move,           // a file or folder to `to`, creating its folder; never over something
    };
    Kind kind = Kind::Write;
    std::string path;  // relative to the game folder, '/'-separated
    std::string to;    // Move: where it goes, relative to the game folder
};

struct Plan {
    std::vector<Step> steps;
    std::vector<std::string> notes;  // what the player should know before it runs
    std::string refused;             // why the plan cannot run; steps are then empty
};

// Install, update and repair are one plan: every package file that is
// missing or differs is written (anvil.toml only when missing: the player's
// settings stay), files in a standard plugin's folder that the package does
// not have are removed, and the Manager copies itself into the folder when
// `manager_exe` is given, is not that file and the copy there differs. version.dll comes last,
// so the game stays vanilla until Anvil is complete. A version.dll that is
// not Anvil's (another mod's proxy) is refused unless `replace_foreign_dll`.
// A legacy text patch is replaced without it, the plan being the one
// confirmation (lab#104). Either way a foreign version.dll is not
// overwritten: it moves first, with the patch's files, to a new folder
// anvil-legacy-backup/<patch>-<date>, so the game is vanilla from the first
// step; a <EU4>/plugins left empty goes. `date` ("2026-11-09") names the
// folder; empty: today.
Plan plan_install(const Package& package, const Installation& installation, const std::filesystem::path& game_dir,
                  const std::filesystem::path& manager_exe, bool replace_foreign_dll, std::string date = {});

// Uninstall (issue #59): version.dll first (only Anvil's), so the game is
// vanilla at once, then the standard plugins, the package's other files
// except anvil.toml and AnvilManager.exe; the player's plugins, anvil/data,
// the crash reports, the logs and anvil.toml stay; an empty anvil/crashes
// goes. With `everything` the whole anvil/ folder goes too. With
// `restore_backup` and a backup, its files go back last (version.dll the
// very last), except those whose place is taken, and the emptied backup
// folder goes.
Plan plan_uninstall(const Package& package, const Installation& installation, bool everything,
                    bool restore_backup = false);

struct ApplyResult {
    bool ok = true;
    std::string error;           // the step that failed and why; the steps before it are done
    bool access_denied = false;  // Windows refused access: elevation may help
    // RemoveManager found the running exe: the caller removes it after exiting.
    bool manager_left = false;
    size_t done = 0;
};

// Runs the steps in order and stops at the first that fails. Files are
// written through a temporary file each; a folder is removed only below the
// game folder.
ApplyResult apply_plan(const Package& package, const Plan& plan, const std::filesystem::path& game_dir,
                       const std::filesystem::path& manager_exe);

}  // namespace anvil::manager
