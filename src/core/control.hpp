#pragma once

#include "control/anvil_control.h"
#include "crash_history.hpp"
#include "gameplay_policy.hpp"
#include "mod_plugins.hpp"
#include "resolver.hpp"
#include "settings.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct AnvilPlugin;

namespace anvil {

class PluginSettings;

// What anvil.control reports on. The runtime fills it once the plugins are
// resolved; the tests build their own.
struct ControlSources {
    std::filesystem::path anvil_dir;  // anvil/: anvil.toml, anvil.log, crashes/, data/anvil/
    const char* anvil_version = "";
    const char* game_build = nullptr;
    // Resolved plugins in load order, and the skipped ones; neither changes
    // after start-up (the plugins' states do, until loading_done).
    const std::vector<std::unique_ptr<AnvilPlugin>>* plugins = nullptr;
    const std::vector<Skipped>* skipped = nullptr;
    const GameplayPolicy* policy = nullptr;  // may be null
    std::vector<std::string> disabled;       // disabled_plugins at this start
    bool safe_mode = false;
    unsigned unfinished_starts = 0;
    // The newest crash report written before this start ("": none). Read at
    // start-up, before this process could write one.
    std::string previous_crash;
    // The plugins in the playset's mods as the start decided them (design
    // §14); may be null. Does not change after start-up.
    const ModDiscovery* mods = nullptr;
    // The player's language for the overlay (i18n::language_code).
    std::string language = "en";
};

// The core's private service for anvil-overlay (src/control/anvil_control.h,
// design §15). One per process; service() is what the core registers as
// "anvil.control" v1. Every call may come from any thread.
class Control {
public:
    explicit Control(ControlSources sources);
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    // The table of function pointers. They reach this object through a
    // process-wide pointer that the constructor sets (and the destructor
    // clears), so only one Control exists at a time.
    const AnvilControlService* service() const;

    // Every plugin was processed: the plugins' states are final from now on
    // and the service reports them.
    void loading_done();

    void status(AnvilControlStatus& out) const;
    bool plugin(uint32_t index, AnvilControlPlugin& out) const;
    std::optional<std::string> warning(uint32_t index) const;
    PluginSettings* settings_of(std::string_view plugin_id) const;
    // The previous crash, loaded on the first call; that call also marks it
    // as shown in anvil/data/anvil/crash-shown.txt.
    const AnvilControlCrash* previous_crash();
    std::string log_tail(uint32_t max_lines) const;
    const char* language() const { return sources_.language.c_str(); }
    AnvilResult disable_next_start(std::string_view plugin_id, std::string& error);
    bool disabled_next_start(std::string_view plugin_id) const;

    // Plugins in mods (design §14). Approving or revoking changes trust.toml
    // for the next start; what this start loaded stays.
    uint32_t mod_plugin_count() const;
    bool mod_plugin(uint32_t index, AnvilControlModPlugin& out) const;
    AnvilResult approve_mod_plugin(uint32_t index, std::string_view hash, std::string& error);
    AnvilResult revoke_mod_plugin(uint32_t index, std::string& error);

private:
    struct CrashCache {
        CrashSummary summary;
        std::string plugins;
        std::string report;
        std::wstring path;
        AnvilControlCrash view{};
    };

    ControlSources sources_;
    std::vector<std::string> versions_;  // by plugin index
    // The reasons as messages (i18n::serialize): the skipped plugins' made
    // once, the resolved plugins' at loading_done.
    std::vector<std::string> skipped_messages_;
    std::vector<std::string> resolved_messages_;
    std::atomic<bool> loading_done_{false};
    mutable std::mutex mutex_;
    std::set<std::string, std::less<>> disabled_next_;  // under mutex_
    bool crash_loaded_ = false;                         // under mutex_
    std::unique_ptr<CrashCache> crash_;                 // under mutex_, then fixed

    // The strings of each mod plugin's view, made once.
    struct ModView {
        std::string name, version, authors, description, repository, issues, others;
        std::string approved_version, approved_hash, approved_date;
        std::string reason_message;
        uint32_t symbols = 0;
    };
    // Which mod and hash trust.toml binds an id to now ("" mod: none).
    struct Binding {
        std::string mod;
        std::string hash;
    };
    std::vector<ModView> mod_views_;
    std::map<std::string, Binding, std::less<>> bindings_;  // under mutex_
    std::string running_hash(const std::string& id) const;  // the approved copy this start loaded for the id
};

// The value as text, as parse_setting_text reads it back: true/false, a
// decimal int, a number, an option's name, the string, a key name.
std::string setting_text(const Setting& setting, const SettingValue& value);

// The hash of design §13 over plugin ids and versions ("id version" lines in
// byte order), shown as "XXXX-XXXX-XXXX-XXXX" (the start of a SHA-256);
// "" for none.
std::string plugin_set_hash(std::vector<std::string> ids_and_versions);

}  // namespace anvil
