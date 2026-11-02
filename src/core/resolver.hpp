#pragma once

#include "manifest.hpp"

#include "i18n/message.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace anvil {

struct ResolveInput {
    std::vector<Manifest> manifests;  // in discovery order
    unsigned host_api = 0;
    std::optional<Version> host_version;  // Anvil version, checked against each plugin's "anvil" range
    const char* game_build = nullptr;  // nullptr = unknown build
    std::vector<std::string> disabled;  // ids disabled in anvil.toml
    // A benchmark start without the compared plugin (ANVIL_BENCHMARK=
    // without:<id>): that id is disabled at the same point, without anvil.toml.
    std::string benchmark_without;
    // Optional check after the API and Anvil version checks (the plugin's
    // symbols.toml); returns why the plugin must be skipped, empty = keep it.
    std::function<i18n::Message(const Manifest&)> verify;
};

// Why a plugin is skipped, in words the start-up benchmark's record may carry
// (docs/spec/benchmark-v1.md §6); the reason text never leaves the computer.
enum class SkipKind { other, safe_mode, conflict, dependency, verification, approval };

const char* skip_kind_name(SkipKind kind);  // "other", "safe_mode", ...

struct Skipped {
    std::string id;     // may be a folder name when the manifest is broken
    std::string reason;  // the English words of `why`
    std::filesystem::path folder;  // the plugin folder
    bool disabled = false;         // disabled by the player in anvil.toml, or by the benchmark arm; not a fault
    bool benchmark_arm = false;    // disabled by the arm of a benchmark start (ResolveInput::benchmark_without)
    std::string mod;               // the mod's name for a plugin from a mod (design §14)
    bool awaiting_approval = false;  // a plugin from a mod that waits for the player's approval
    SkipKind kind = SkipKind::other;   // not used while `disabled`
    i18n::Message why;                 // the reason by code, for the player's language (#158)
};

// A skipped plugin whose `reason` holds the English words of `why`.
Skipped skipped_for(std::string id, i18n::Message why, std::filesystem::path folder, SkipKind kind = SkipKind::other,
                    bool disabled = false);

struct ResolveResult {
    std::vector<Manifest> order;  // load order: all early plugins first, then main
    std::vector<Skipped> skipped;
};

// Validates plugins against the host (and, through `verify`, the game), skips
// plugins that declare a `conflicts` entry matching another loaded plugin,
// resolves required and optional dependencies (SemVer ranges) and load_after,
// and orders them topologically (ties broken by id). A plugin with
// affects_gameplay = true depends on anvil-game (any version) without
// declaring it (design §13): the resolver adds that dependency, `implied`. Plugins that cannot be
// loaded are listed in `skipped` with a human-readable reason; anything
// depending on them is skipped too. Conflict edge cases: docs/design.md §10.
ResolveResult resolve(const ResolveInput& input);

}  // namespace anvil
