#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::manager {

inline constexpr const char* kSteamAppId = "236850";  // Europa Universalis IV
inline constexpr const wchar_t* kSteamLaunchUrl = L"steam://rungameid/236850";

// Valve's KeyValues text (libraryfolders.vdf, appmanifest_*.acf): a key with
// a quoted string value or a { } block of further keys. Keys keep their
// order; lookups ignore case, as Steam does.
struct VdfNode {
    std::string value;
    std::vector<std::pair<std::string, VdfNode>> children;

    const VdfNode* child(std::string_view key) const;
};

// nullopt with `error` when the text is not KeyValues. Escapes \\ \" \n \t
// are read in quoted strings, // starts a comment.
std::optional<VdfNode> parse_vdf(std::string_view text, std::string& error);

// The library folders of steamapps/libraryfolders.vdf: the current format
// ("libraryfolders" { "0" { "path" "…" "apps" { … } } }) and the old one
// ("LibraryFolders" { "1" "D:\\SteamLibrary" }). Libraries that list the game
// among their apps come first.
std::vector<std::filesystem::path> steam_libraries(const VdfNode& libraryfolders);

// The game folders Steam has: for every library (and Steam's own folder),
// steamapps/appmanifest_236850.acf names the folder in steamapps/common/
// ("installdir"); a folder with eu4.exe counts. `steam_root` is Steam's
// folder.
std::vector<std::filesystem::path> find_steam_games(const std::filesystem::path& steam_root);

// Steam's folder from the registry (HKCU Software\Valve\Steam SteamPath,
// then HKLM's InstallPath); empty when Steam is not installed.
std::filesystem::path steam_root_from_registry();

// The game folder GOG has: HKLM SOFTWARE\GOG.com\Games\2057001589 (Galaxy and
// GOG's installers write it, both as 32-bit keys) when the `path` it names has
// eu4.exe; nullopt otherwise.
std::optional<std::filesystem::path> find_gog_game();

// What starts a GOG install as the player's shortcut does: dowser.exe, the
// Paradox launcher's start, or eu4.exe in a folder without it.
std::filesystem::path gog_program(const std::filesystem::path& game_dir);

// What the Manager knows about a game folder before it acts on it.
struct GameFolder {
    std::filesystem::path dir;
    bool has_exe = false;     // eu4.exe is a file in it
    std::string exe_sha256;   // upper-case hex
    std::string build;        // a build Anvil knows, or empty
    std::string error;        // why it is not a game folder
};

// Checks `dir`: eu4.exe present and hashed, its build looked up in the
// table compiled into Anvil (design §6). An unknown build is not an error:
// Anvil starts there with only the plugins that run on every build.
GameFolder inspect_game_folder(const std::filesystem::path& dir);

// The same file, whatever the path's spelling (case, short names, links):
// compares the volume and the file id.
bool same_file(const std::filesystem::path& a, const std::filesystem::path& b);

// True when an eu4.exe process runs from `game_dir`, or when an eu4.exe
// runs whose path cannot be read (it may be this one). While the game runs,
// the Manager changes nothing in its folder (design §5.3).
bool game_running(const std::filesystem::path& game_dir);

// When AnvilManager.exe runs from inside `game_dir` (the copy install puts
// there, design §3).
bool runs_from(const std::filesystem::path& exe, const std::filesystem::path& game_dir);

}  // namespace anvil::manager
