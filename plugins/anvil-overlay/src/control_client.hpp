// The C++ side of anvil.control (src/control/anvil_control.h): copies of what
// the core reports, in std::string and std::vector.
#pragma once

#include "anvil_control.h"
#include "i18n/translate.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace overlay {

// A module of a plugin (ADR 0011), as anvil.control reports it (Anvil 1.22).
struct ModuleRow {
    std::string key, name, description;  // name and description in the player's language
    std::string state;                    // resolved plugins: "loaded", "disabled", ...; "" in a mod
    bool gameplay = false;
    bool offers_ironman = false;
    bool offers_multiplayer = false;
    bool on_by_default = true;
    bool enabled = false;  // on at this start (resolved plugins)
    bool suspended = false;
};

struct PluginRow {
    std::string id, name, version, state, reason, issues;
    bool early = false;
    bool gameplay = false;
    bool suspended = false;
    bool disabled = false;       // by the player at this start
    bool disabled_next = false;  // listed in anvil.toml now
    uint32_t settings = 0;
    std::string mod;                 // a plugin from a mod: the mod's name
    bool awaiting_approval = false;  // skipped until the player approves it
    anvil::i18n::Message why;        // `reason` by code; empty from a core before 1.7
    std::vector<ModuleRow> modules;  // [[modules]], in plugin.toml order

    // The reason in the player's language.
    std::string reason_text() const { return why.empty() ? reason : anvil::i18n::text(why); }
};

// A plugin in a mod (design §14), as anvil.control reports it.
struct ModRow {
    uint32_t index = 0;  // for approve_mod_plugin / revoke_mod_plugin
    std::string id, name, version, authors, description, repository, issues;
    std::string mod, workshop_id, state, reason, hash, others;
    std::string approved_version, approved_hash, approved_date;
    bool gameplay = false;
    uint32_t symbols = 0;
    bool loaded = false;
    bool can_approve = false;
    bool can_revoke = false;
    bool approved_next = false;  // approved in this game; loads from the next start
    bool revoked_next = false;
    std::vector<ModuleRow> modules;  // as its plugin.toml declares them
    anvil::i18n::Message why;    // `reason` by code; empty from a core before 1.7

    std::string reason_text() const { return why.empty() ? reason : anvil::i18n::text(why); }

    // Waits for the player: not approved yet, an update, or a conflict.
    bool waiting() const {
        return can_approve && !approved_next && (state == "pending" || state == "update-pending" || state == "conflict");
    }
};

struct CrashInfo {
    std::string name, level, summary, issues, report;
    std::vector<std::string> plugins;
    std::wstring path;
    bool before_game_ready = false;
    bool is_new = false;
};

class ControlClient {
public:
    explicit ControlClient(const AnvilControlService* service = nullptr) : service_(service) {}
    const AnvilControlService* service() const { return service_; }
    explicit operator bool() const { return service_ != nullptr; }

    AnvilControlStatus status() const;
    // The player's language ("en" from a core before 1.7).
    anvil::i18n::Language language() const;
    std::vector<PluginRow> plugins() const;
    std::vector<std::string> warnings() const;
    std::optional<CrashInfo> previous_crash() const;
    std::string log_tail(uint32_t max_lines) const;

    uint32_t setting_count(const std::string& plugin) const;
    // The label, description and group come in the player's language when
    // the plugin's translation has them (plugin_text), else as written.
    std::optional<AnvilControlSetting> setting_info(const std::string& plugin, AnvilSetting setting) const;
    // (name, label) of each option of an enum setting, the label translated likewise.
    std::vector<std::pair<std::string, std::string>> setting_options(const std::string& plugin,
                                                                     AnvilSetting setting) const;
    std::string setting_value(const std::string& plugin, AnvilSetting setting, uint32_t which) const;
    std::vector<std::string> setting_warnings(const std::string& plugin) const;

    // "" on success, otherwise why.
    std::string set(const std::string& plugin, AnvilSetting setting, const AnvilControlValue& value) const;
    std::string set_text(const std::string& plugin, AnvilSetting setting, const std::string& text) const;
    std::string reset(const std::string& plugin, AnvilSetting setting) const;
    std::string disable_next_start(const std::string& plugin) const;

    // A text of the plugin in the player's language from its translations/
    // (design §5.4), or `english` from a core before 1.20 or without one.
    const char* plugin_text(const std::string& plugin, const char* english) const;
    // Some plugin text came back in Chinese, Japanese or Korean, which Anvil's
    // own font subsets do not cover: the overlay merges the fonts of Windows.
    static bool translated_cjk();

    // Plugins in mods; none from a core without them. The description is
    // translated as plugin_text does.
    std::vector<ModRow> mod_plugins() const;
    std::string approve_mod_plugin(const ModRow& plugin) const;  // with the hash it shows
    std::string revoke_mod_plugin(const ModRow& plugin) const;

private:
    const AnvilControlService* service_;
};

// "a, b" -> {"a", "b"}
std::vector<std::string> split_list(const std::string& text);

}  // namespace overlay
