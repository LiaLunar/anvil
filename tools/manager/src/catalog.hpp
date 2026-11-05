#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anvil::manager {

// The plugin catalog (ADR 0002, design §15): catalog/plugins.json of the
// anvil repository lists the team's plugins, each with one pinned release
// zip. The Manager carries the copy of its build (resource ANVIL_CATALOG)
// and fetches the one on main only on the player's click. A zip is installed
// only when its size and SHA-256 equal the pin; CI verified the pin's
// attestation when it was merged (catalog/update.py --check).
inline constexpr const wchar_t* kCatalogHost = L"raw.githubusercontent.com";
inline constexpr const wchar_t* kCatalogPath = L"/happy-dwarf-team/anvil/main/catalog/plugins.json";
inline constexpr int64_t kCatalogSchema = 1;
inline constexpr const char* kCatalogDownloadPrefix = "https://github.com/happy-dwarf-team/";
inline constexpr const char* kCatalogPagePrefix = "https://anvil.happydwarf.dev/";
inline constexpr const char* kCatalogWorkshopPrefix = "https://steamcommunity.com/sharedfiles/filedetails/?id=";
inline constexpr size_t kMaxCatalogBytes = 256u << 10;

struct CatalogZip {
    std::string url;     // https://github.com/happy-dwarf-team/<repo>/releases/download/<tag>/<name>.zip
    uint64_t size = 0;   // at most kMaxZipTotalBytes
    std::string sha256;  // lower-case hex
};

struct CatalogPlugin {
    std::string id;
    std::string name;
    std::string summary;
    std::string page;      // on anvil.happydwarf.dev
    std::string workshop;  // its Workshop item; "" when it has none
    std::string note;      // "" or a sentence the tab shows under the summary
    std::string version;   // of the pinned release
    std::string anvil;     // the Anvil range its plugin.toml declares; "" for any
    bool affects_gameplay = false;
    std::vector<std::string> builds;  // from its symbols.toml; empty: every build
    CatalogZip zip;
};

struct Catalog {
    std::vector<CatalogPlugin> plugins;
};

// catalog/plugins.json, validated: schema 1, every plugin with a valid id
// (once), a name, a version and Anvil range that parse, links with the
// prefixes above and a pinned zip of the team's releases. Unknown keys are
// ignored; a newer schema is refused. nullopt with `error` otherwise.
std::optional<Catalog> parse_catalog(std::string_view json, std::string& error);

// What the game folder has of a catalog plugin.
struct CatalogStatus {
    bool for_build = true;   // the game's build is in `builds`, or `builds` is empty
    bool anvil_ok = true;    // the installed Anvil is in the plugin's range (true when unknown)
    std::string installed;   // the version in anvil/plugins; "" when none
    bool from_mod = false;   // an enabled mod carries a plugin with this id
    // Installed at the pinned version.
    bool current(const CatalogPlugin& plugin) const { return !installed.empty() && installed == plugin.version; }
};

// `local` are the (id, version) of anvil/plugins, `mods` the ids of plugins
// from enabled mods; `build` is the game's ("" when unknown), `anvil_version`
// the installed Anvil's ("" when unknown).
CatalogStatus catalog_status(const CatalogPlugin& plugin, std::string_view build, std::string_view anvil_version,
                             const std::vector<std::pair<std::string, std::string>>& local,
                             const std::vector<std::string>& mods);

// Why a downloaded zip is not the pinned one (size, then SHA-256); empty when it is.
std::string check_pinned_zip(const CatalogPlugin& plugin, const std::vector<uint8_t>& bytes);

// Why a pinned zip's plugin.toml does not match the entry (another id or
// version); empty when it does.
std::string check_pinned_manifest(const CatalogPlugin& plugin, std::string_view id, std::string_view version);

// The catalog plugin that a plugin from a mod presents itself as, by its id,
// name or repository, when the mod is not that plugin's Workshop item; nullptr
// otherwise. A plugin.toml says what it likes, so this is what the approval
// dialog can check it against.
const CatalogPlugin* impersonated_plugin(const Catalog& catalog, std::string_view id, std::string_view name,
                                         std::string_view repository, std::string_view workshop_id);

// The host and path of the pinned zip's URL, for WinHTTP.
std::pair<std::wstring, std::wstring> split_https_url(std::string_view url);

}  // namespace anvil::manager
