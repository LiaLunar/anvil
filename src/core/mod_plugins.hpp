#pragma once

#include "manifest.hpp"
#include "playset.hpp"
#include "trust.hpp"

#include "i18n/message.hpp"
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace anvil {

// What happens to a plugin a mod ships in <mod>/anvil/<id>/ (design §14, #60).
enum class ModPluginState {
    Approved,       // its hash is approved for this mod: it loads from the approved copy
    UpdatePending,  // the mod has another hash: the previously approved copy loads until the new one is approved
    Pending,        // not approved (or approved for another mod): it does not load
    Conflict,       // another enabled mod has a plugin with this id and no approval decides: none loads
    Shadowed,       // a local plugin in anvil/plugins has this id and wins
    Invalid,        // cannot be approved: the folder or plugin.toml is wrong
};

const char* state_name(ModPluginState state);  // "approved", "update-pending", ...

struct ModPlugin {
    std::string id;                // the folder's name, which plugin.toml's id must equal
    PlaysetMod mod;
    std::filesystem::path folder;  // <mod>/anvil/<id>
    std::string hash;              // of `folder`; "" when it cannot be hashed
    std::optional<Manifest> manifest;  // of `folder`
    std::string problem;           // why `folder` cannot load or be approved; "" when it can
    ModPluginState state = ModPluginState::Pending;
    std::string reason;            // what the log and the player read: the English words of `why`
    i18n::Message problem_why, why;  // `problem` and `reason` by code, for the player's language (#158)
    // trust.toml's entry for this id; `bound` when it names this mod.
    std::optional<TrustEntry> approval;
    bool bound = false;
    // Approved and UpdatePending: anvil/data/anvil/approved/<id>/<approved hash>.
    std::filesystem::path load_folder;
    // Approved: the copy does not exist yet; the loader makes it from `folder`
    // (whose hash is the approved one) at the next start.
    bool copy_missing = false;
    std::vector<std::string> others;  // Conflict: the other mods' names; Shadowed: the local plugin's folder

    bool loads() const { return state == ModPluginState::Approved || state == ModPluginState::UpdatePending; }
};

struct ModDiscovery {
    Playset playset;
    TrustFile trust;
    std::vector<ModPlugin> plugins;    // by mod in playset order, then by id
    std::vector<std::string> warnings;  // dlc_load.json, descriptors, trust.toml
};

// anvil/data/anvil/approved/<id>/<hash>
std::filesystem::path approved_copy(const std::filesystem::path& anvil_dir, const std::string& id, const std::string& hash);

// The plugins of the mods enabled in `user_dir`'s playset, decided against
// anvil/trust.toml (design §14, #60). `local_ids`: the ids of anvil/plugins
// (folder names where plugin.toml is broken). Reads, never writes.
//  - A plugin whose id a local plugin has is Shadowed.
//  - Two or more enabled mods with the same id: the one trust.toml binds the
//    id to is decided as below and the others are Conflict; with none bound,
//    all of them are Conflict.
//  - Otherwise the entry for the id decides: bound to this mod with this hash
//    is Approved; bound with another hash is UpdatePending while that hash's
//    approved copy exists (Pending otherwise); anything else is Pending.
// A plugin with a `problem` is Invalid unless its previous approved copy
// still loads (UpdatePending).
ModDiscovery discover_mod_plugins(const std::filesystem::path& anvil_dir, const std::filesystem::path& user_dir,
                                  const std::set<std::string>& local_ids);

// The copy of an Approved plugin whose copy is missing, made from its mod
// folder and checked against the approved hash. False with `error`.
bool make_approved_copy(const std::filesystem::path& anvil_dir, const ModPlugin& plugin, std::string& error);

// Approves `plugin` as the player saw it (`hash`): checks that the mod folder
// still has that hash and can load, copies it to its approved copy, checks
// the copy, and binds the id to the plugin's mod in trust.toml (replacing an
// earlier approval of the id). Copies of the id other than this one and
// `keep_hash` (the copy the running game loaded) are removed where they can
// be. False with `error`; trust.toml is then unchanged.
bool approve_mod_plugin(const std::filesystem::path& anvil_dir, const ModPlugin& plugin, const std::string& hash,
                        const std::string& keep_hash, std::string& error);

// Removes the approval of `id` from trust.toml and its approved copies other
// than `keep_hash`. True also when there was none.
bool revoke_mod_plugin(const std::filesystem::path& anvil_dir, const std::string& id, const std::string& keep_hash,
                       std::string& error);

// Removes the approved copies trust.toml no longer names (revoked ids, older
// hashes, unfinished copies). The loader calls it before any plugin loads.
// Nothing is removed while trust.toml does not parse.
void prune_approved_copies(const std::filesystem::path& anvil_dir, const TrustFile& trust);

}  // namespace anvil
