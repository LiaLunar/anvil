#pragma once

#include "cli/game.hpp"

#include <filesystem>
#include <string>

namespace anvil::manager {

// Whether the Manager offers to approve the shown plugin: only what
// approve_shown_plugin would take, and not what this mod already has approved.
bool can_approve_shown(const cli::GameModPlugin& shown);

// Approves the plugin from a mod that the Manager showed (an entry of
// `report`, which check_game made for `game_dir` and `user_dir`): the
// playset is read again, and the approval goes through only when the same
// mod still has that plugin with the hash that was shown (design §14). The
// game is not running, so no approved copy is in use. False with `error`.
bool approve_shown_plugin(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir,
                          const cli::GameReport& report, const cli::GameModPlugin& shown, std::string& error);

// Removes the approval of the shown plugin's id, when it binds the id to
// that plugin's mod, and its approved copies.
bool revoke_shown_plugin(const std::filesystem::path& game_dir, const cli::GameModPlugin& shown, std::string& error);

}  // namespace anvil::manager
