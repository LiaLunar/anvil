// The start as the game makes it, for the tests that load plugins: a change
// registry over the test's image, the bus, the host table and the gameplay
// policy, and a plugin set made from what the test adds, as runtime.cpp
// makes it from what the start found. Plugins load through the set, one at a
// time in load order, so a test runs the sequence the game runs.
#pragma once

#include "core/changes.hpp"
#include "core/gameplay_policy.hpp"
#include "core/host.hpp"
#include "core/log.hpp"
#include "core/manifest.hpp"
#include "core/plugin_set.hpp"
#include "core/plugin_symbols.hpp"
#include "core/services.hpp"
#include "core/settings.hpp"
#include "core/symbol_check.hpp"
#include "test_support.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// mod-probe's folder (tests/plugins/mod-probe), when CMake gives it: a plugin
// DLL without symbols that loads anywhere, the entry of a plugin.toml a test
// writes for a plugin of a given shape.
#ifdef ANVIL_TEST_MOD_PROBE_DIR
inline std::filesystem::path mod_probe_dir() { return std::filesystem::path(ANVIL_TEST_MOD_PROBE_DIR); }
#endif

class TestStart {
public:
    anvil::ChangeRegistry changes;
    anvil::ServiceBus bus;
    anvil::HostContext context;
    AnvilGameInfo info{};
    AnvilHost host{};
    anvil::GameplayPolicy policy;
    std::vector<std::string> log;  // "<plugin, module or anvil>: <message>"
    // The ANVIL_<ID>_<NAME> setting overrides the set sees: none unless a test sets them.
    anvil::PluginSettings::Environment environment = [](const std::string&) { return std::optional<std::string>(); };

    // The image whose symbols plugins declare (none: 0, 0) and its known build.
    explicit TestStart(uintptr_t image_base = 0, uint32_t image_size = 0, const char* build = nullptr,
                       anvil::GameplayOptIns opt_ins = {})
        : changes(image_base, image_size),
          policy(changes, bus, std::move(opt_ins),
                 [this](AnvilLogLevel, const std::string& id, const std::string& m) { log.push_back(id + ": " + m); }),
          build_(build),
          image_base_(image_base) {
        context.changes = &changes;
        context.bus = &bus;
        context.image = GetModuleHandleW(nullptr);
        context.log = [this](const AnvilPlugin& p, AnvilLogLevel, std::string_view m) {
            log.push_back(p.name + ": " + std::string(m));
        };
        info.size = sizeof(info);
        info.build_id = build;
        info.image_base = image_base;
        info.image_size = image_size;
        host = anvil::make_host(&info);
        // As the runtime: the policy listens before any plugin hears of a session.
        policy.subscribe();
        data_root_ = std::filesystem::temp_directory_path() / ("anvil_test_start_" + std::to_string(GetCurrentProcessId()));
    }
    TestStart(const TestStart&) = delete;
    TestStart& operator=(const TestStart&) = delete;

    // ---- before make() ----

    // A plugin folder as the start finds it: plugin.toml, its symbols.toml
    // verified against the image, its settings.toml with the player's file in
    // a fresh folder (each run starts from the defaults). Null, after a failed
    // check, when it does not parse or verify.
    anvil::ResolvedPlugin* add(const std::filesystem::path& folder) {
        const auto parsed = anvil::load_manifest(folder);
        if (!parsed.manifest) std::printf("  %s: %s\n", anvil::narrow(folder.wstring()).c_str(), parsed.error.c_str());
        CHECK(parsed.manifest.has_value());
        if (!parsed.manifest) return nullptr;
        anvil::ResolvedPlugin r;
        r.manifest = *parsed.manifest;
        if (!r.manifest.symbols.empty()) {
            anvil::SymbolVerification verified;
            anvil::SymbolFile declared;
            const std::string reason = anvil::i18n::english(anvil::check_plugin_symbols(
                folder / r.manifest.symbols, build_, changes, image_base_, &verified, &declared));
            if (!reason.empty()) std::printf("  %s: %s\n", r.manifest.id.c_str(), reason.c_str());
            CHECK(reason.empty());
            if (!reason.empty()) return nullptr;
            r.symbols = anvil::PluginSymbols(declared, verified, image_base_);
        }
        if (!r.manifest.settings.empty()) {
            anvil::SettingsFileResult settings = anvil::load_settings(folder / r.manifest.settings);
            CHECK(settings.file.has_value());
            if (!settings.file) return nullptr;
            r.settings = std::move(*settings.file);
        }
        return &add(std::move(r));
    }

    // A plugin of a given shape: its plugin.toml as text, its entry DLL looked
    // for in `folder` (none: a folder that does not exist, so a load fails).
    anvil::ResolvedPlugin& add(const std::string& plugin_toml, const std::filesystem::path& folder = {}) {
        const std::filesystem::path where = folder.empty() ? data_root_ / "no-such-folder" : folder;
        auto parsed = anvil::parse_manifest_text(plugin_toml, where);
        if (!parsed.manifest) std::printf("  manifest error: %s\n", parsed.error.c_str());
        anvil::ResolvedPlugin r;
        r.manifest = std::move(*parsed.manifest);
        return add(std::move(r));
    }

    anvil::ResolvedPlugin& add(anvil::ResolvedPlugin r) {
        r.data_dir = data_root_ / r.manifest.id;
        std::error_code ignored;
        std::filesystem::remove_all(r.data_dir, ignored);
        resolved_.push_back(std::move(r));
        return resolved_.back();
    }

    void skip(anvil::Skipped s) { skipped_.push_back(std::move(s)); }

    // The set, from everything added: from here on no plugin is added.
    anvil::PluginSet& make() {
        anvil::PluginSet::Dependencies deps;
        deps.context = &context;
        deps.policy = &policy;
        deps.host = &host;
        deps.log = [this](AnvilLogLevel, const std::string& id, const std::string& m) { log.push_back(id + ": " + m); };
        deps.environment = environment;
        std::vector<anvil::ResolvedPlugin> resolved(std::make_move_iterator(resolved_.begin()), std::make_move_iterator(resolved_.end()));
        set_ = std::make_unique<anvil::PluginSet>(std::move(resolved), std::move(skipped_), std::move(deps));
        return *set_;
    }

    // ---- after make() ----

    anvil::PluginSet& set() { return *set_; }
    const anvil::PluginRecord& plugin(std::string_view id) const {
        const anvil::PluginRecord* p = set_->find(id);
        if (p == nullptr) std::printf("  no plugin %.*s in the set\n", static_cast<int>(id.size()), id.data());
        return *p;
    }
    AnvilPlugin* handle(std::string_view id) const { return plugin(id).handle(); }
    // A module's handle by its key ("<plugin>.<module>"); null when it is off.
    AnvilPlugin* module(std::string_view plugin_id, std::string_view key) const {
        for (const auto& m : plugin(plugin_id).modules()) {
            if (m.key == key) return m.handle;
        }
        return nullptr;
    }
    // Loads the plugin through the set; its handle when it loaded.
    AnvilPlugin* load(std::string_view id) {
        const anvil::PluginRecord& p = plugin(id);
        set_->load(p);
        const anvil::Outcome* o = p.outcome();
        return o != nullptr && o->kind == anvil::Outcome::Kind::loaded ? p.handle() : nullptr;
    }
    bool loaded(std::string_view id) const {
        const anvil::Outcome* o = plugin(id).outcome();
        return o != nullptr && o->kind == anvil::Outcome::Kind::loaded;
    }

    bool logged(const std::string& line) const {
        for (const auto& l : log) {
            if (l == line) return true;
        }
        return false;
    }
    size_t logged_containing(const std::string& part) const {
        size_t n = 0;
        for (const auto& l : log) n += l.find(part) != std::string::npos ? 1 : 0;
        return n;
    }
    void dump() const {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
    }

private:
    const char* build_;
    uintptr_t image_base_;
    std::filesystem::path data_root_;
    std::deque<anvil::ResolvedPlugin> resolved_;  // references stay valid while a test adds more
    std::vector<anvil::Skipped> skipped_;
    std::unique_ptr<anvil::PluginSet> set_;
};
