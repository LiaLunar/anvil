// The files behind the script errors panel's messages: the layers of the
// playset the game started with (the game folder, then the enabled mods of
// dlc_load.json), the layer that has a file the game names, and the uses of
// a name in the playset's script. Reads files only.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "errors_model.hpp"

namespace script_errors {

struct Layer {
    std::string name;  // "game" or the mod's name
    std::filesystem::path path;
};

// EU4's user folder: -userdir=<path> on the command line, else Documents.
std::filesystem::path user_dir();
// The folder of the running EXE.
std::filesystem::path game_dir();

// The game folder, then the enabled mods of `dlc_load` in its order; a mod's
// relative path is under `user`.
std::vector<Layer> playset_layers(const std::filesystem::path& dlc_load, const std::filesystem::path& user,
                                  const std::filesystem::path& game);

// The last layer that has `file` (game-relative); false when none has it.
bool resolve(const std::vector<Layer>& layers, std::string_view file, std::string& path, std::string& layer);

// The uses of `token` as a whole word in the script folders of every layer
// (events, common, decisions, missions, history/countries; comments left
// out): the lines that save it as an event target first, then other uses.
struct Search {
    std::vector<Hit> hits;  // at most 10
    std::size_t total = 0;
    std::size_t files = 0;
};
Search search(const std::vector<Layer>& layers, std::string_view token);

// vscode://file/<path>:<line>, the path's characters escaped.
std::string vscode_uri(std::string_view utf8_path, int line);

std::filesystem::path path_of(std::string_view utf8);
std::string utf8_of(const std::filesystem::path& path);

}  // namespace script_errors
