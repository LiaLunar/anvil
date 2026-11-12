#pragma once

#include "benchmark_tab.hpp"
#include "cli/game.hpp"
#include "core/discovery.hpp"
#include "core/plugin_translation.hpp"
#include "core/settings.hpp"
#include "game.hpp"
#include "install.hpp"
#include "platform.hpp"
#include "plugin_zip.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace anvil::manager {

// The Manager's window contents (design §15, issue #59): one ImGui frame per
// draw(). Everything that reads the game folder at length (hashes, anvil
// check --game) runs on a worker thread; while that folder's eu4.exe runs,
// nothing that changes the folder is offered.
class ManagerApp {
public:
    ManagerApp(HWND window, std::optional<Package> package, std::string package_error, std::filesystem::path game);

    void draw();
    // A file dropped on the window: a plugin zip.
    void dropped(const std::filesystem::path& file);
    bool should_close() const { return close_; }

private:
    // What the worker reads from the game folder.
    struct Snapshot {
        GameFolder folder;
        std::optional<Installation> installation;
        std::optional<cli::GameReport> report;  // when the anvil folder exists
        AnvilSettings anvil_toml;
        std::vector<std::string> crashes;
    };
    struct PendingPlan {
        bool uninstall = false;
        Plan plan;
    };
    struct SettingsEditor {
        std::string plugin;  // id
        std::filesystem::path folder;  // the plugin's, for its translations
        std::filesystem::path player_file;
        SettingsFile schema;
        std::vector<SettingValue> values;
        std::vector<std::string> texts;  // string and key settings, as edited
        std::vector<std::string> warnings;
        std::string error;
        bool dirty = false;
    };

    // What catalog_status needs from the snapshot.
    struct CatalogInputs {
        std::vector<std::pair<std::string, std::string>> local;  // (id, version) in anvil/plugins
        std::vector<std::string> mods;                          // ids of plugins from enabled mods
        std::string anvil_version;                              // "" when Anvil is not installed
        std::string build;
    };

    void choose_game(const std::filesystem::path& dir);
    // The release check and, with it, the catalog (ADR 0005, ADR 0006): once
    // per start and when the player turns the check on; what is already being
    // asked is not asked again.
    void check_for_updates();
    CatalogInputs catalog_inputs() const;
    // The catalog plugins with an update for this game folder (ADR 0006).
    std::vector<const CatalogPlugin*> catalog_updates_here() const;
    void refresh();
    void poll();
    bool read_only() const;
    bool anvil_installed() const;  // the snapshot found Anvil's version.dll in game_

    void draw_game_bar();
    void draw_anvil_tab();
    void draw_catalog_tab();
    void draw_plugins_tab();
    void draw_mods_tab();
    void draw_settings_tab();
    void draw_benchmark_tab();
    void draw_crashes_tab();
    void draw_log_tab();
    void draw_about_tab();
    void draw_first_run();
    void draw_modals();
    void draw_plan_modal();
    void draw_zip_modal();
    void draw_download_modal();
    void draw_approve_modal();

    void start_plan(bool uninstall);
    void run_plan();
    void open_zip(const std::filesystem::path& file);
    // A zip from the picker, a drop or the catalog; `pin` is the catalog
    // entry it was downloaded for, whose id and version it must hold.
    void check_zip(ZipArchive zip, std::string name, const CatalogPlugin* pin);
    void downloaded(ZipDownload download);
    void install_zip();
    void edit_list(const std::string& key, const std::string& id, bool listed);
    void draw_modules(const Manifest& manifest);
    void set_module(const std::string& key, bool on_by_default, bool on);
    void open_settings(const cli::GamePlugin& plugin);
    void save_settings();
    void show_message(std::string title, std::string text);
    void failed(const std::string& what, const std::string& error, bool access_denied);
    std::string make_problem_report() const;

    HWND window_;
    std::optional<Package> package_;
    std::string package_error_;
    std::filesystem::path exe_;
    ManagerSettings settings_;
    BenchmarkTab benchmark_;
    bool benchmark_busy_ = false;  // in the last frame: the game folder is read again once a series ends
    std::vector<std::filesystem::path> steam_games_;
    std::optional<std::filesystem::path> gog_game_;

    std::filesystem::path game_;
    bool gog_ = false;  // game_ is a GOG install: Play, the Workshop and the benchmark's checks do without Steam
    bool launcher_opened_ = false;  // Play opened the Paradox launcher in a GOG install and the game has not started yet
    std::optional<Snapshot> snapshot_;
    std::future<Snapshot> pending_;
    bool running_ = false;
    std::chrono::steady_clock::time_point running_checked_{};
    uint64_t running_started_ = 0;  // when the game that runs from game_ started (FILETIME), 0 when not known
    std::vector<std::filesystem::path> running_elsewhere_;  // folders of an eu4.exe that is not game_'s (issue #253)
    bool anvil_missed_ = false;     // the last game from game_ ended without writing anvil.log

    std::filesystem::path start_menu_game_;  // the game in_start_menu_ was read for
    bool in_start_menu_ = false;             // the shortcut starts that game's copy of the Manager

    std::optional<PendingPlan> plan_;
    bool replace_foreign_dll_ = false;
    bool remove_everything_ = false;
    bool restore_backup_ = false;  // uninstall: put the newest legacy backup back (lab#104)

    std::optional<ZipArchive> zip_;
    std::string zip_name_;
    std::optional<PluginZipCheck> zip_check_;
    bool zip_anyway_ = false;

    std::optional<SettingsEditor> editor_;
    // A text of the plugin in `folder` in the player's language from its
    // translations/ (design §5.4), or `english` itself. Each file is read
    // once per language.
    const std::string& plugin_text(const std::filesystem::path& folder, const std::string& english) const;
    mutable std::map<std::pair<std::filesystem::path, std::string>, std::optional<PluginTranslation>> translations_;
    std::string crash_selected_;
    std::string crash_text_;
    std::string log_text_;

    // The catalog (ADR 0002): the Manager's own list until the player fetches main's.
    std::optional<Catalog> catalog_;
    std::string catalog_error_;  // why the built-in list or the last fetch was refused
    bool catalog_fetched_ = false;
    bool catalog_by_check_ = false;  // the fetch, pending or done, is the release check's and not a click
    std::future<CatalogFetch> catalog_pending_;
    std::optional<CatalogPlugin> download_;  // asked about, then downloading
    std::future<ZipDownload> download_pending_;
    bool open_download_ = false;

    std::future<ReleaseCheck> release_pending_;
    std::optional<ReleaseCheck> release_;

    std::string message_title_;
    std::string message_text_;
    bool offer_elevation_ = false;
    bool open_message_ = false;
    bool open_plan_ = false;
    bool open_zip_ = false;
    bool open_approve_ = false;
    std::optional<cli::GameModPlugin> approve_;  // the plugin from a mod the approval modal asks about
    bool close_after_message_ = false;
    bool show_settings_tab_ = false;
    bool show_catalog_tab_ = false;
    bool log_previous_ = false;
    bool log_report_ = false;   // the Log tab shows the problem report, which is in the clipboard
    bool report_copied_ = false;
    bool close_ = false;
};

}  // namespace anvil::manager
