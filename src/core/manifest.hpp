#pragma once

#include "semver.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil {

enum class Phase { Early, Main };

// The manifest's `api` this host accepts: the ANVIL_API_VERSION of the host table.
inline constexpr unsigned kManifestApi = 2;

// allow_ironman / allow_multiplayer: false, or "opt-in" (the player may allow
// the plugin in that mode).
enum class Allow { No, OptIn };

struct Dependency {
    std::string id;
    VersionRange range;
    bool optional = false;
    // Added by the resolver, not written in plugin.toml: affects_gameplay =
    // true needs anvil-game (design §13).
    bool implied = false;
};

// [[conflicts]]: the declaring plugin is skipped while a matching one is enabled.
struct Conflict {
    std::string id;
    VersionRange range;
};

struct Manifest {
    std::string id;
    std::string name;
    Version version;
    std::vector<std::string> authors;
    std::string description;
    unsigned api = 0;
    std::optional<VersionRange> anvil;  // Anvil versions the plugin needs ("anvil" key); empty = any
    std::string entry;  // DLL file name inside the plugin folder
    Phase phase = Phase::Main;
    std::string symbols;   // symbols.toml path relative to the folder; empty = none (runs on every build)
    std::string settings;  // settings.toml path relative to the folder; empty = none
    bool affects_gameplay = false;
    Allow allow_ironman = Allow::No;
    Allow allow_multiplayer = Allow::No;
    std::vector<std::string> load_after;
    std::string issues;      // URL, optional
    std::string repository;  // URL, optional
    std::vector<Dependency> dependencies;
    std::vector<Conflict> conflicts;

    std::filesystem::path folder;  // plugin folder
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;  // set when manifest is empty
    uint32_t line = 0;  // the line of plugin.toml the error is about; 0 = the whole file
};

// Parses plugin.toml v2 (design §5.1). Any unknown key or a value of the wrong
// type is an error; so is `api = 1`. Does not touch the file system.
ManifestResult parse_manifest_text(std::string_view text, const std::filesystem::path& folder);
// Reads folder/plugin.toml and also checks that the files it names
// (`symbols`, `settings`) exist in the folder.
ManifestResult load_manifest(const std::filesystem::path& folder);

bool is_valid_plugin_id(std::string_view id);

// The largest plugin.toml, and the largest symbols.toml or settings.toml, that
// is read. Anvil reads the manifests of plugins nobody has approved yet at every
// start, so a mod must not be able to make that cost what it likes.
inline constexpr std::uintmax_t kMaxManifestBytes = 1u << 20;
inline constexpr std::uintmax_t kMaxPluginTomlBytes = 16u << 20;

// The longest byte pattern symbols.toml accepts.
inline constexpr size_t kMaxPatternBytes = 65536;

// Parses "48 8B ?? 24" (whitespace optional, "??" = any byte) into bytes and
// a compare mask. At least one byte must not be a wildcard.
bool parse_byte_pattern(std::string_view text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask,
                        std::string& error);

}  // namespace anvil
