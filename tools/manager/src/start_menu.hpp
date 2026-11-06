// The Manager's entry in the player's Start menu (design §15): a shortcut the
// player asks for, to the copy in the game folder. These calls need COM on the
// thread, as the window has it.
#pragma once

#include <filesystem>
#include <string>

namespace anvil::manager {

// %APPDATA%\Microsoft\Windows\Start Menu\Programs\Anvil Manager.lnk, this
// Windows user's own; empty when Windows does not name the folder.
std::filesystem::path start_menu_link();

// What the shortcut `link` starts; empty when there is none or it cannot be read.
std::filesystem::path link_target(const std::filesystem::path& link);

// Whether the shortcut `link` starts `exe`.
bool link_starts(const std::filesystem::path& link, const std::filesystem::path& exe);

// Writes `link` to start `exe` from its folder, replacing what is there.
bool write_link(const std::filesystem::path& link, const std::filesystem::path& exe, std::string& error);

// Removes `link` when it starts `exe`; a shortcut to another game's copy stays.
void remove_link(const std::filesystem::path& link, const std::filesystem::path& exe);

}  // namespace anvil::manager
