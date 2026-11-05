#include "mods.hpp"

#include "core/mod_plugins.hpp"
#include "i18n/translate.hpp"

#include <set>

namespace anvil::manager {

namespace fs = std::filesystem;

bool can_approve_shown(const cli::GameModPlugin& shown) {
    const bool approved_here = !shown.approved_hash.empty() && shown.approved_hash == shown.hash;
    return shown.state != "invalid" && shown.state != "shadowed" && shown.problem.empty() && !shown.hash.empty() &&
           shown.offered().manifest && !approved_here;
}

bool approve_shown_plugin(const fs::path& game_dir, const fs::path& user_dir, const cli::GameReport& report,
                          const cli::GameModPlugin& shown, std::string& error) {
    std::set<std::string> local_ids;
    for (const cli::GamePlugin& p : report.plugins) local_ids.insert(p.report.manifest ? p.report.manifest->id : p.folder);
    const fs::path anvil_dir = game_dir / "anvil";
    const ModDiscovery now = discover_mod_plugins(anvil_dir, user_dir, local_ids);
    for (const ModPlugin& p : now.plugins) {
        if (p.id != shown.id || p.mod.descriptor != shown.descriptor) continue;
        if (p.hash != shown.hash) {
            error = i18n::tr("the plugin's files changed since they were shown; check again");
            return false;
        }
        return approve_mod_plugin(anvil_dir, p, shown.hash, "", error);
    }
    error = i18n::tr("the mod no longer has this plugin, or it is not enabled; check again");
    return false;
}

bool revoke_shown_plugin(const fs::path& game_dir, const cli::GameModPlugin& shown, std::string& error) {
    const fs::path anvil_dir = game_dir / "anvil";
    const TrustFile trust = read_trust(anvil_dir / "trust.toml");
    const TrustEntry* entry = trust.find(shown.id);
    if (trust.error.empty() && (entry == nullptr || entry->mod != shown.descriptor)) {
        error = i18n::trf("no approval binds {0} to mod '{1}'", {shown.id, shown.mod});
        return false;
    }
    return revoke_mod_plugin(anvil_dir, shown.id, "", error);
}

}  // namespace anvil::manager
