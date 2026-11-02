#pragma once

#include "manifest.hpp"

#include "i18n/message.hpp"
#include <anvil/anvil_plugin.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace anvil {

// anvil/anvil.toml (design §11). Every key is optional; a missing file gives
// the defaults. A file that does not parse is ignored as a whole: `error` says
// why (and on which `line`) while the other fields keep their defaults.
struct AnvilSettings {
    bool enabled = true;
    std::vector<std::string> disabled_plugins;
    AnvilLogLevel log_level = ANVIL_LOG_INFO;
    bool profile = false;  // count calls per change, reported at exit (design §17)
    // Plugins the player allows in ironman / multiplayer sessions (design §13);
    // only plugins whose plugin.toml offers "opt-in" for that mode can use it.
    std::vector<std::string> allow_ironman;
    std::vector<std::string> allow_multiplayer;
    std::string error;
    uint32_t line = 0;
};

AnvilSettings read_anvil_settings(const std::filesystem::path& file);

// The plugin folders of anvil/plugins in the order the loader discovers them:
// every sub-folder, sorted by path. A missing folder gives none.
std::vector<std::filesystem::path> plugin_folders(const std::filesystem::path& plugins_dir);

// Empty when the plugin's entry DLL is in its folder, otherwise why the
// loader skips the plugin.
i18n::Message missing_entry_reason(const Manifest& manifest);

}  // namespace anvil
