#include "catalog.hpp"

#include "core/game_build.hpp"
#include "core/log.hpp"
#include "core/manifest.hpp"
#include "core/semver.hpp"
#include "json.hpp"
#include "zip.hpp"

#include <algorithm>
#include <cctype>

namespace anvil::manager {
namespace {

// A link the Manager may open or fetch: https, the prefix, and nothing a
// shell or a browser would read differently.
bool safe_link(const std::string& url, const char* prefix) {
    if (!url.starts_with(prefix) || url.size() == std::string_view(prefix).size()) return false;
    return std::none_of(url.begin(), url.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u <= 0x20 || u >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`' || c == '^' ||
               c == '{' || c == '}' || c == '|';
    });
}

bool all_digits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool lower_hex(std::string_view text, size_t size) {
    return text.size() == size &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

const std::string* string_member(const Json& object, std::string_view key) {
    const Json* value = object.find(key);
    return value != nullptr && value->is_string() ? &value->as_string() : nullptr;
}

// A pinned zip: a release asset of a repository of the team, a size within
// the zip limits and a SHA-256.
std::string check_zip(const Json& zip, CatalogZip& out) {
    if (!zip.is_object()) return "zip is not an object";
    const std::string* url = string_member(zip, "url");
    const Json* size = zip.find("size");
    const std::string* sha256 = string_member(zip, "sha256");
    if (url == nullptr || !safe_link(*url, kCatalogDownloadPrefix)) return "zip.url is not a link to github.com/happy-dwarf-team/";
    const std::string_view rest = std::string_view(*url).substr(std::string_view(kCatalogDownloadPrefix).size());
    const size_t slash = rest.find('/');
    if (slash == 0 || slash == std::string_view::npos || !rest.substr(slash).starts_with("/releases/download/") ||
        !rest.ends_with(".zip") || rest.find("..") != std::string_view::npos || rest.find('?') != std::string_view::npos ||
        rest.find('#') != std::string_view::npos) {
        return "zip.url is not a release asset";
    }
    if (size == nullptr || !size->is_integer() || size->as_integer() <= 0 ||
        static_cast<uint64_t>(size->as_integer()) > kMaxZipTotalBytes) {
        return "zip.size is not a size up to 256 MB";
    }
    if (sha256 == nullptr || !lower_hex(*sha256, 64)) return "zip.sha256 is not a SHA-256 in lower-case hex";
    out.url = *url;
    out.size = static_cast<uint64_t>(size->as_integer());
    out.sha256 = *sha256;
    return {};
}

std::string check_plugin(const Json& entry, CatalogPlugin& out) {
    if (!entry.is_object()) return "not an object";
    const auto text = [&](std::string_view key, std::string& to, bool required) -> bool {
        const Json* value = entry.find(key);
        if (value == nullptr) return !required;
        if (!value->is_string() || (required && value->as_string().empty())) return false;
        to = value->as_string();
        return true;
    };
    if (!text("id", out.id, true) || !is_valid_plugin_id(out.id)) return "id is not a plugin id";
    if (!text("name", out.name, true)) return "name is missing";
    if (!text("summary", out.summary, false) || !text("note", out.note, false)) return "summary or note is not a string";
    if (!text("page", out.page, true) || !safe_link(out.page, kCatalogPagePrefix)) return "page is not a link to anvil.happydwarf.dev";
    if (!text("workshop", out.workshop, false) ||
        (!out.workshop.empty() &&
         (!out.workshop.starts_with(kCatalogWorkshopPrefix) ||
          !all_digits(std::string_view(out.workshop).substr(std::string_view(kCatalogWorkshopPrefix).size()))))) {
        return "workshop is not a link to a Workshop item";
    }
    if (!text("version", out.version, true) || !Version::parse(out.version)) return "version is not a version";
    if (!text("anvil", out.anvil, false) || (!out.anvil.empty() && !VersionRange::parse(out.anvil))) {
        return "anvil is not a version range";
    }
    if (const Json* gameplay = entry.find("affects_gameplay")) {
        if (!gameplay->is_bool()) return "affects_gameplay is not true or false";
        out.affects_gameplay = gameplay->as_bool();
    }
    if (const Json* builds = entry.find("builds")) {
        if (!builds->is_array()) return "builds is not a list";
        for (const Json& build : builds->items()) {
            if (!build.is_string() || build.as_string().empty()) return "builds holds something that is not a build id";
            out.builds.push_back(build.as_string());
        }
    }
    const Json* zip = entry.find("zip");
    if (zip == nullptr) return "zip is missing";
    return check_zip(*zip, out.zip);
}

}  // namespace

std::optional<Catalog> parse_catalog(std::string_view json, std::string& error) {
    if (json.size() > kMaxCatalogBytes) {
        error = "the list is too long";
        return std::nullopt;
    }
    const auto document = parse_json(json, error);
    if (!document) return std::nullopt;
    const Json* schema = document->find("schema");
    if (!document->is_object() || schema == nullptr || !schema->is_integer()) {
        error = "not a plugin catalog";
        return std::nullopt;
    }
    if (schema->as_integer() != kCatalogSchema) {
        error = schema->as_integer() > kCatalogSchema ? "the list needs a newer Manager" : "not a plugin catalog";
        return std::nullopt;
    }
    const Json* plugins = document->find("plugins");
    if (plugins == nullptr || !plugins->is_array()) {
        error = "plugins is not a list";
        return std::nullopt;
    }
    Catalog catalog;
    for (const Json& entry : plugins->items()) {
        CatalogPlugin plugin;
        if (std::string why = check_plugin(entry, plugin); !why.empty()) {
            error = (plugin.id.empty() ? "a plugin" : plugin.id) + ": " + why;
            return std::nullopt;
        }
        if (std::any_of(catalog.plugins.begin(), catalog.plugins.end(), [&](const CatalogPlugin& p) { return p.id == plugin.id; })) {
            error = plugin.id + ": listed twice";
            return std::nullopt;
        }
        catalog.plugins.push_back(std::move(plugin));
    }
    return catalog;
}

CatalogStatus catalog_status(const CatalogPlugin& plugin, std::string_view build, std::string_view anvil_version,
                             const std::vector<std::pair<std::string, std::string>>& local,
                             const std::vector<std::string>& mods) {
    CatalogStatus status;
    status.for_build = plugin.builds.empty() || std::find(plugin.builds.begin(), plugin.builds.end(), build) != plugin.builds.end();
    if (!plugin.anvil.empty()) {
        const auto range = VersionRange::parse(plugin.anvil);
        const auto version = Version::parse(anvil_version);
        status.anvil_ok = !range || !version || range->matches(*version);
    }
    for (const auto& [id, version] : local) {
        if (id == plugin.id) status.installed = version.empty() ? "?" : version;
    }
    status.from_mod = std::find(mods.begin(), mods.end(), plugin.id) != mods.end();
    return status;
}

std::string check_pinned_zip(const CatalogPlugin& plugin, const std::vector<uint8_t>& bytes) {
    if (bytes.size() != plugin.zip.size) {
        return "the download has " + std::to_string(bytes.size()) + " bytes, the catalog says " + std::to_string(plugin.zip.size);
    }
    std::string sha256 = sha256_bytes(bytes.data(), bytes.size());
    std::transform(sha256.begin(), sha256.end(), sha256.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (sha256 != plugin.zip.sha256) return "the download's SHA-256 is " + sha256 + ", the catalog says " + plugin.zip.sha256;
    return {};
}

std::string check_pinned_manifest(const CatalogPlugin& plugin, std::string_view id, std::string_view version) {
    if (id != plugin.id) return "the zip holds the plugin '" + std::string(id) + "', not '" + plugin.id + "'";
    if (version != plugin.version) return "the zip holds version " + std::string(version) + ", the catalog says " + plugin.version;
    return {};
}

std::pair<std::wstring, std::wstring> split_https_url(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) return {};
    url.remove_prefix(scheme.size());
    const size_t slash = url.find('/');
    if (slash == 0 || slash == std::string_view::npos) return {};
    return {widen(url.substr(0, slash)), widen(url.substr(slash))};
}

}  // namespace anvil::manager
