#include "manifest.hpp"

#include <anvil/anvil_plugin.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace anvil {
namespace {

// plugin.toml v2 (design §5.1). Anything else is an error, so a typo in a key
// cannot silently change what the plugin declares.
const std::set<std::string> kKnownKeys = {
    "id", "name", "version", "authors", "description", "api", "anvil", "entry", "phase", "symbols", "settings",
    "affects_gameplay", "allow_ironman", "allow_multiplayer", "load_after", "issues", "repository",
    "dependencies", "conflicts", "modules",
};
const std::set<std::string> kKnownDependencyKeys = {"id", "version", "optional"};
const std::set<std::string> kKnownModuleKeys = {"id",           "name",           "description",      "affects_gameplay",
                                                "allow_ironman", "allow_multiplayer", "default"};

// A module id: [a-z][a-z0-9_]*, at most 32 characters (ADR 0011).
bool is_valid_module_id(std::string_view id) {
    if (id.empty() || id.size() > 32 || id.front() < 'a' || id.front() > 'z') return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}
const std::set<std::string> kKnownConflictKeys = {"id", "version"};

// A schema error and the line of plugin.toml it is about (0 = the whole file).
struct Error {
    std::string text;
    uint32_t line = 0;

    bool empty() const { return text.empty(); }
    void set(std::string message, const toml::node* node) {
        text = std::move(message);
        line = node != nullptr ? static_cast<uint32_t>(node->source().begin.line) : 0;
    }
};

size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = up;
        }
    }
    return row[b.size()];
}

// "unknown key 'x'" for the first key of `table` not in `known`, with the
// closest known key as a hint; empty when every key is known.
Error unknown_key(const toml::table& table, const std::set<std::string>& known) {
    for (const auto& [key, value] : table) {
        const std::string name(key.str());
        if (known.contains(name)) continue;
        std::string message = "unknown key '" + name + "'";
        std::string best;
        size_t bestDistance = 3;  // suggest only close matches
        for (const auto& candidate : known) {
            const size_t d = edit_distance(name, candidate);
            if (d < bestDistance) {
                bestDistance = d;
                best = candidate;
            }
        }
        if (!best.empty()) message += " (did you mean '" + best + "'?)";
        return {message, static_cast<uint32_t>(key.source().begin.line)};
    }
    return {};
}

// Optional string key; a value of another type is an error.
bool read_string(const toml::table& table, std::string_view key, std::string& out, Error& error) {
    const toml::node* node = table.get(key);
    if (node == nullptr) return true;
    const auto value = node->value_exact<std::string>();
    if (!value) {
        error.set(std::string(key) + " must be a string", node);
        return false;
    }
    out = *value;
    return true;
}

bool read_bool(const toml::table& table, std::string_view key, bool& out, Error& error) {
    const toml::node* node = table.get(key);
    if (node == nullptr) return true;
    const auto value = node->value_exact<bool>();
    if (!value) {
        error.set(std::string(key) + " must be true or false", node);
        return false;
    }
    out = *value;
    return true;
}

// allow_ironman / allow_multiplayer: false | "opt-in"; absent = false.
bool read_allow(const toml::table& table, std::string_view key, Allow& out, Error& error) {
    const toml::node* node = table.get(key);
    if (node == nullptr) return true;
    if (const auto flag = node->value_exact<bool>(); flag && !*flag) {
        out = Allow::No;
        return true;
    }
    if (const auto text = node->value_exact<std::string>(); text && *text == "opt-in") {
        out = Allow::OptIn;
        return true;
    }
    error.set(std::string(key) + " must be false or \"opt-in\"", node);
    return false;
}

// symbols / settings: a relative .toml path that stays inside the plugin folder.
bool read_plugin_file(const toml::table& table, std::string_view key, std::string& out, Error& error) {
    if (!read_string(table, key, out, error)) return false;
    if (!table.contains(key)) return true;
    bool inside = !out.empty() && out.find(':') == std::string::npos && out.front() != '/' && out.front() != '\\';
    for (size_t begin = 0; inside && begin <= out.size();) {
        const size_t end = std::min(out.find_first_of("/\\", begin), out.size());
        const std::string_view part(out.data() + begin, end - begin);
        if (part.empty() || part == "." || part == "..") inside = false;
        begin = end + 1;
    }
    if (!inside || !out.ends_with(".toml")) {
        error.set(std::string(key) + " must be a .toml file inside the plugin folder", table.get(key));
        return false;
    }
    return true;
}

// issues / repository: an http(s) URL without spaces.
bool read_url(const toml::table& table, std::string_view key, std::string& out, Error& error) {
    if (!read_string(table, key, out, error)) return false;
    if (!table.contains(key)) return true;
    const size_t scheme = out.starts_with("https://") ? 8 : out.starts_with("http://") ? 7 : 0;
    const bool clean = std::none_of(out.begin(), out.end(), [](char c) { return static_cast<unsigned char>(c) <= ' '; });
    if (scheme == 0 || out.size() == scheme || !clean) {
        error.set(std::string(key) + " must be an http(s) URL", table.get(key));
        return false;
    }
    return true;
}

// [[dependencies]] / [[conflicts]] entry: id (required) and a version range (default "*").
bool read_plugin_ref(const toml::node& node, const std::set<std::string>& known, std::string& id, VersionRange& range,
                     Error& error) {
    const auto* entry = node.as_table();
    if (entry == nullptr) {
        error.set("must be a table", &node);
        return false;
    }
    if (error = unknown_key(*entry, known); !error.empty()) return false;
    std::string rangeText = "*";
    if (!read_string(*entry, "id", id, error) || !read_string(*entry, "version", rangeText, error)) return false;
    if (!is_valid_plugin_id(id)) {
        error.set("id '" + id + "' must match [a-z0-9-]+ (max 64 characters)", entry->contains("id") ? entry->get("id") : &node);
        return false;
    }
    const auto parsed = VersionRange::parse(rangeText);
    if (!parsed) {
        error.set("version '" + rangeText + "' is not a SemVer range", entry->get("version"));
        return false;
    }
    range = *parsed;
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool read_string_array(const toml::node* node, std::vector<std::string>& out, Error& error, std::string_view key) {
    if (node == nullptr) return true;
    const auto* array = node->as_array();
    if (array == nullptr) {
        error.set(std::string(key) + " must be an array of strings", node);
        return false;
    }
    for (const auto& item : *array) {
        const auto value = item.value<std::string>();
        if (!value) {
            error.set(std::string(key) + " must be an array of strings", &item);
            return false;
        }
        out.push_back(*value);
    }
    return true;
}

}  // namespace

bool affects_gameplay_anywhere(const Manifest& manifest) {
    return manifest.affects_gameplay || std::any_of(manifest.modules.begin(), manifest.modules.end(),
                                                    [](const Module& m) { return m.affects_gameplay; });
}

std::string module_key(const Manifest& manifest, const Module& module) { return manifest.id + "." + module.id; }

Manifest module_manifest(const Manifest& manifest, const Module& module) {
    Manifest m = manifest;
    m.id = module_key(manifest, module);
    m.name = manifest.name + ": " + module.name;
    if (!module.description.empty()) m.description = module.description;
    m.affects_gameplay = module.affects_gameplay;
    m.allow_ironman = module.allow_ironman;
    m.allow_multiplayer = module.allow_multiplayer;
    m.modules.clear();
    return m;
}

bool is_valid_plugin_id(std::string_view id) {
    if (id.empty() || id.size() > 64 || id.front() == '-' || id.back() == '-') return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

bool parse_byte_pattern(std::string_view text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask,
                        std::string& error) {
    bytes.clear();
    mask.clear();
    std::string digits;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') digits.push_back(c);
    }
    if (digits.empty() || digits.size() % 2 != 0) {
        error = "bytes must be pairs of hex digits or \"??\"";
        return false;
    }
    if (digits.size() / 2 > kMaxPatternBytes) {
        error = "bytes longer than " + std::to_string(kMaxPatternBytes) + " bytes";
        return false;
    }
    bool any = false;
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (digits[i] == '?' && digits[i + 1] == '?') {
            bytes.push_back(0);
            mask.push_back(0);
            continue;
        }
        const int high = hex_digit(digits[i]), low = hex_digit(digits[i + 1]);
        if (high < 0 || low < 0) {
            error = "'" + digits.substr(i, 2) + "' is not a hex byte or \"??\"";
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(high * 16 + low));
        mask.push_back(0xFF);
        any = true;
    }
    if (!any) {
        error = "bytes must contain at least one byte that is not \"??\"";
        return false;
    }
    return true;
}

namespace {

// The lines of the `symbols` and `settings` keys, for load_manifest's errors.
struct FileKeyLines {
    uint32_t symbols = 0;
    uint32_t settings = 0;
};

uint32_t line_of(const toml::node* node) {
    return node != nullptr ? static_cast<uint32_t>(node->source().begin.line) : 0;
}

ManifestResult parse_manifest(std::string_view text, const std::filesystem::path& folder, FileKeyLines& lines) {
    ManifestResult result;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "plugin.toml: " << e.description() << " (line " << e.source().begin.line << ")";
        result.error = message.str();
        result.line = static_cast<uint32_t>(e.source().begin.line);
        return result;
    }
    auto fail = [&](const std::string& message, uint32_t line) {
        result.error = "plugin.toml: " + message;
        result.line = line;
        return result;
    };

    // The API comes first: an API 1 manifest fails the v2 schema anyway, and
    // its author needs to know why.
    const uint32_t api_line = line_of(table.get("api"));
    const auto api = table["api"].value_exact<int64_t>();
    if (!api || *api <= 0) return fail("missing or invalid 'api'", api_line);
    if (*api == 1) {
        result.error = "plugin API 1: update the plugin";
        result.line = api_line;
        return result;
    }
    if (*api > kManifestApi) {
        return fail("plugin API " + std::to_string(*api) + " is newer than this Anvil supports (API " +
                        std::to_string(kManifestApi) + "): update Anvil",
                    api_line);
    }
    if (const Error unknown = unknown_key(table, kKnownKeys); !unknown.empty()) return fail(unknown.text, unknown.line);

    Manifest m;
    m.folder = folder;
    m.api = static_cast<unsigned>(*api);
    Error error;
    for (const auto* key : {"id", "name", "version", "entry"}) {
        if (!table.contains(key)) return fail("missing '" + std::string(key) + "'", 0);
    }
    std::string versionText, phase = "main";
    if (!read_string(table, "id", m.id, error) || !read_string(table, "name", m.name, error) ||
        !read_string(table, "version", versionText, error) || !read_string(table, "entry", m.entry, error) ||
        !read_string(table, "description", m.description, error) || !read_string(table, "phase", phase, error)) {
        return fail(error.text, error.line);
    }
    if (m.name.empty()) return fail("name must not be empty", line_of(table.get("name")));
    if (!is_valid_plugin_id(m.id)) {
        return fail("id '" + m.id + "' must match [a-z0-9-]+ (max 64 characters)", line_of(table.get("id")));
    }
    const auto version = Version::parse(versionText);
    if (!version) {
        return fail("version '" + versionText + "' is not SemVer (MAJOR.MINOR.PATCH)", line_of(table.get("version")));
    }
    m.version = *version;
    if (m.entry.find_first_of("/\\:") != std::string::npos || !m.entry.ends_with(".dll")) {
        return fail("entry must be a .dll file name inside the plugin folder", line_of(table.get("entry")));
    }
    if (phase == "early") m.phase = Phase::Early;
    else if (phase == "main") m.phase = Phase::Main;
    else return fail("phase must be \"early\" or \"main\"", line_of(table.get("phase")));
    if (table.contains("anvil")) {
        const auto rangeText = table["anvil"].value_exact<std::string>();
        const auto range = rangeText ? VersionRange::parse(*rangeText) : std::nullopt;
        if (!range) return fail("anvil must be a SemVer range string, e.g. \">=1.0.0\"", line_of(table.get("anvil")));
        m.anvil = *range;
    }
    // affects_gameplay has no default (design §13): the author must decide.
    if (!table.contains("affects_gameplay")) return fail("missing 'affects_gameplay' (true or false)", 0);
    if (!read_bool(table, "affects_gameplay", m.affects_gameplay, error) ||
        !read_allow(table, "allow_ironman", m.allow_ironman, error) ||
        !read_allow(table, "allow_multiplayer", m.allow_multiplayer, error) ||
        !read_plugin_file(table, "symbols", m.symbols, error) ||
        !read_plugin_file(table, "settings", m.settings, error) || !read_url(table, "issues", m.issues, error) ||
        !read_url(table, "repository", m.repository, error) ||
        !read_string_array(table.get("authors"), m.authors, error, "authors") ||
        !read_string_array(table.get("load_after"), m.load_after, error, "load_after")) {
        return fail(error.text, error.line);
    }
    lines.symbols = line_of(table.get("symbols"));
    lines.settings = line_of(table.get("settings"));

    if (table.contains("dependencies")) {
        const auto* list = table.get_as<toml::array>("dependencies");
        if (list == nullptr) {
            return fail("dependencies must be an array of tables ([[dependencies]])", line_of(table.get("dependencies")));
        }
        for (const auto& node : *list) {
            Dependency d;
            const std::string label = "[[dependencies]] #" + std::to_string(m.dependencies.size() + 1) + ": ";
            if (!read_plugin_ref(node, kKnownDependencyKeys, d.id, d.range, error)) return fail(label + error.text, error.line);
            if (!read_bool(*node.as_table(), "optional", d.optional, error)) return fail(label + error.text, error.line);
            m.dependencies.push_back(std::move(d));
        }
    }
    if (table.contains("conflicts")) {
        const auto* list = table.get_as<toml::array>("conflicts");
        if (list == nullptr) {
            return fail("conflicts must be an array of tables ([[conflicts]])", line_of(table.get("conflicts")));
        }
        for (const auto& node : *list) {
            Conflict c;
            const std::string label = "[[conflicts]] #" + std::to_string(m.conflicts.size() + 1) + ": ";
            if (!read_plugin_ref(node, kKnownConflictKeys, c.id, c.range, error)) return fail(label + error.text, error.line);
            if (c.id == m.id) return fail(label + "a plugin cannot conflict with itself", line_of(node.as_table()->get("id")));
            m.conflicts.push_back(std::move(c));
        }
    }
    if (table.contains("modules")) {
        const auto* list = table.get_as<toml::array>("modules");
        if (list == nullptr || list->empty()) {
            return fail("modules must be a non-empty array of tables ([[modules]])", line_of(table.get("modules")));
        }
        if (list->size() > kMaxModules) {
            return fail("more than " + std::to_string(kMaxModules) + " [[modules]]", line_of(table.get("modules")));
        }
        for (const auto& node : *list) {
            const std::string label = "[[modules]] #" + std::to_string(m.modules.size() + 1) + ": ";
            const auto* entry = node.as_table();
            if (entry == nullptr) return fail(label + "must be a table", line_of(&node));
            if (error = unknown_key(*entry, kKnownModuleKeys); !error.empty()) return fail(label + error.text, error.line);
            for (const auto* key : {"id", "name", "affects_gameplay"}) {
                if (!entry->contains(key)) return fail(label + "missing '" + std::string(key) + "'", line_of(&node));
            }
            Module mod;
            if (!read_string(*entry, "id", mod.id, error) || !read_string(*entry, "name", mod.name, error) ||
                !read_string(*entry, "description", mod.description, error) ||
                !read_bool(*entry, "affects_gameplay", mod.affects_gameplay, error) ||
                !read_allow(*entry, "allow_ironman", mod.allow_ironman, error) ||
                !read_allow(*entry, "allow_multiplayer", mod.allow_multiplayer, error) ||
                !read_bool(*entry, "default", mod.enabled_by_default, error)) {
                return fail(label + error.text, error.line);
            }
            if (!is_valid_module_id(mod.id)) {
                return fail(label + "id '" + mod.id + "' must match [a-z][a-z0-9_]* (max 32 characters)",
                            line_of(entry->get("id")));
            }
            if (mod.name.empty()) return fail(label + "name must not be empty", line_of(entry->get("name")));
            if (std::any_of(m.modules.begin(), m.modules.end(), [&](const Module& o) { return o.id == mod.id; })) {
                return fail(label + "module '" + mod.id + "' is declared twice", line_of(entry->get("id")));
            }
            // An opt-in lets the player keep a module active where the core
            // would suspend it; a module that is never suspended has nothing
            // to opt in to, and saying so would mislead the player.
            if (!mod.affects_gameplay && (mod.allow_ironman == Allow::OptIn || mod.allow_multiplayer == Allow::OptIn)) {
                return fail(label + "a module with affects_gameplay = false is never suspended; remove its opt-in",
                            line_of(entry->contains("allow_ironman") ? entry->get("allow_ironman")
                                                                     : entry->get("allow_multiplayer")));
            }
            m.modules.push_back(std::move(mod));
        }
    }
    result.manifest = std::move(m);
    return result;
}

}  // namespace

ManifestResult parse_manifest_text(std::string_view text, const std::filesystem::path& folder) {
    FileKeyLines lines;
    return parse_manifest(text, folder, lines);
}

ManifestResult load_manifest(const std::filesystem::path& folder) {
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(folder / "plugin.toml", size_error);
    if (!size_error && size > kMaxManifestBytes) return {std::nullopt, "plugin.toml is larger than 1 MB"};
    std::ifstream file(folder / "plugin.toml", std::ios::binary);
    if (!file) {
        return {std::nullopt, "plugin.toml not found"};
    }
    std::ostringstream content;
    content << file.rdbuf();
    std::string text = content.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    FileKeyLines lines;
    ManifestResult result = parse_manifest(text, folder, lines);
    if (!result.manifest) return result;
    for (const auto& [key, relative, line] : {std::tuple{"symbols", &result.manifest->symbols, lines.symbols},
                                              std::tuple{"settings", &result.manifest->settings, lines.settings}}) {
        if (relative->empty()) continue;
        const std::u8string name(relative->begin(), relative->end());  // plugin.toml is UTF-8
        std::error_code ec;
        if (!std::filesystem::is_regular_file(folder / std::filesystem::path(name), ec)) {
            return {std::nullopt,
                    std::string("plugin.toml: ") + key + " file '" + *relative + "' not found in the plugin folder", line};
        }
    }
    return result;
}

}  // namespace anvil
