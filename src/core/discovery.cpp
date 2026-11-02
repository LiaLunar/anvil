#include "discovery.hpp"

#include <toml++/toml.hpp>

#include <algorithm>

namespace anvil {

namespace {

void read_ids(const toml::table& t, const char* key, std::vector<std::string>& out) {
    if (const auto* list = t.get_as<toml::array>(key)) {
        for (const auto& item : *list) {
            if (auto v = item.value<std::string>()) out.push_back(*v);
        }
    }
}

}  // namespace

AnvilSettings read_anvil_settings(const std::filesystem::path& file) {
    AnvilSettings s;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return s;
    try {
        const toml::table t = toml::parse_file(file.wstring());
        s.enabled = t["enabled"].value_or(true);
        read_ids(t, "disabled_plugins", s.disabled_plugins);
        read_ids(t, "allow_ironman", s.allow_ironman);
        read_ids(t, "allow_multiplayer", s.allow_multiplayer);
        const std::string level = t["log_level"].value_or<std::string>("info");
        if (level == "debug") s.log_level = ANVIL_LOG_DEBUG;
        else if (level == "warning") s.log_level = ANVIL_LOG_WARNING;
        else if (level == "error") s.log_level = ANVIL_LOG_ERROR;
        s.profile = t["profile"].value_or(false);
    } catch (const toml::parse_error& e) {
        s = AnvilSettings{};
        s.error = std::string(e.description());
        s.line = static_cast<uint32_t>(e.source().begin.line);
    }
    return s;
}

std::vector<std::filesystem::path> plugin_folders(const std::filesystem::path& plugins_dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(plugins_dir, ec)) {
        if (entry.is_directory(ec)) folders.push_back(entry.path());
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

i18n::Message missing_entry_reason(const Manifest& manifest) {
    std::error_code ec;
    if (std::filesystem::exists(manifest.folder / manifest.entry, ec)) return {};
    return i18n::Message("entry_missing", {manifest.entry});
}

}  // namespace anvil
