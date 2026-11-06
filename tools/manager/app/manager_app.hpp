#pragma once

#include "benchmark_tab.hpp"
#include "cli/game.hpp"
#include "core/discovery.hpp"
#include "core/settings.hpp"
#include "game.hpp"
#include "install.hpp"
#include "platform.hpp"
#include "plugin_zip.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <future>
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
        std::filesystem::path player_file;
        SettingsFile schema;
        std::vector<SettingValue> values;
        std::vector<std::string> texts;  // string and key settings, as edited
        std::vector<std::string> warnings;
        std::string error;
        bool dirty = false;
    };

    void choose_game(const std::filesystem::path& dir);
    void refresh();
    void poll();
    bool read_only() const;

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

    std::filesystem::path start_menu_game_;  // the game in_start_menu_ was read for
    bool in_start_menu_ = false;             // the shortcut starts that game's copy of the Manager

    std::optional<PendingPlan> plan_;
    bool replace_foreign_dll_ = false;
    bool remove_everything_ = false;

    std::optional<ZipArchive> zip_;
    std::string zip_name_;
    std::optional<PluginZipCheck> zip_check_;
    bool zip_anyway_ = false;

    std::optional<SettingsEditor> editor_;
    std::string crash_selected_;
    std::string crash_text_;
    std::string log_text_;

    // The catalog (ADR 0002): the Manager's own list until the player fetches main's.
    std::optional<Catalog> catalog_;
    std::string catalog_error_;  // why the built-in list or the last fetch was refused
    bool catalog_fetched_ = false;
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
    bool log_previous_ = false;
    bool log_report_ = false;   // the Log tab shows the problem report, which is in the clipboard
    bool report_copied_ = false;
    bool close_ = false;
};

}  // namespace anvil::manager
