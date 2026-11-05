#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// EU4's user folder: the path in <EU4>/userdir.txt when that file names one,
// otherwise Documents\Paradox Interactive\Europa Universalis IV (the folder
// the game finds with SHGetFolderPathW, as eu4.exe 1.37.5 imports it), or
// Europa Universalis IV GOG in a GOG install, whose eu4.exe adds " GOG" to the
// name (design §14). Empty when Documents cannot be found.
std::filesystem::path eu4_user_dir(const std::filesystem::path& game_dir);

// The folder `-userdir=<path>` names on eu4.exe's command line, which the game
// prefers to userdir.txt; nullopt without one. The whole argument may be in
// quotes, as Node's spawn passes a path with a space, or only the path, as a
// player writes it in Steam's launch options. Without quotes it ends
// where the game ends an argument, at a space before -, + or /, or at a bare
// ` continue=`.
std::optional<std::filesystem::path> user_dir_argument(std::wstring_view command_line);

// One mod of the playset, from its descriptor (`<user dir>/mod/<x>.mod`).
struct PlaysetMod {
    std::string descriptor;  // as dlc_load.json lists it, e.g. "mod/ugc_123.mod": the mod's identity in trust.toml
    std::string name;        // `name`; the descriptor's file name when it has none
    // The Steam Workshop item, when the mod's folder is the Workshop's
    // content/236850/<id>; "" for a local mod (also one whose descriptor got
    // a remote_file_id from an upload).
    std::string workshop_id;
    std::filesystem::path folder;  // `path`, absolute (a relative one is in the user folder)
    std::string error;             // why the mod is not read: no descriptor, no `path`, a zipped mod
};

struct Playset {
    std::filesystem::path user_dir;
    bool found = false;      // dlc_load.json exists
    std::string error;       // dlc_load.json does not parse: no mod counts
    std::vector<PlaysetMod> mods;  // enabled, in the file's order
};

// The descriptors of dlc_load.json's `enabled_mods`, in order. nullopt with
// `error` when the text is not a JSON object whose enabled_mods (if present)
// is an array of strings.
std::optional<std::vector<std::string>> parse_dlc_load(std::string_view json, std::string& error);

// `name`, `path` and `archive` of a .mod descriptor (Paradox script: `key =
// "value"` pairs, blocks such as tags = { ... } skipped, # comments). The
// folder is left relative; read_playset resolves it.
PlaysetMod parse_mod_descriptor(std::string_view text);

// The enabled mods of the playset the game starts with (design §14):
// <user dir>/dlc_load.json -> each .mod -> its `path`. A missing
// dlc_load.json is an empty playset.
Playset read_playset(const std::filesystem::path& user_dir);

}  // namespace anvil
