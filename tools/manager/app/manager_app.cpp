#include "manager_app.hpp"

#include "core/anvil_toml_edit.hpp"
#include "core/crash_history.hpp"
#include "core/game_build.hpp"
#include "core/log.hpp"
#include "core/playset.hpp"
#include "core/semver.hpp"
#include "i18n/cjk_font.hpp"
#include "i18n/translate.hpp"
#include "mods.hpp"
#include "problem_report.hpp"
#include "ui.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <span>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;
using namespace ui;
using i18n::N_;
using i18n::tr;
using i18n::trf;

// The `language` setting of the overlay in that game folder, when it names one.
std::optional<i18n::Language> overlay_language(const fs::path& game) {
    const SettingsFileResult schema = load_settings(game / "anvil" / "plugins" / "anvil-overlay" / "settings.toml");
    if (!schema.file) return std::nullopt;
    const PlayerSettings player = load_player_settings(*schema.file, game / "anvil" / "data" / "anvil-overlay" / "settings.toml");
    for (const Setting& setting : schema.file->settings) {
        if (setting.name != "language" || setting.type != SettingType::Enum) continue;
        const SettingValue value = player.values[setting.id].value_or(setting.default_value);
        if (value.integer < 0 || static_cast<size_t>(value.integer) >= setting.options.size()) return std::nullopt;
        return i18n::language_of_setting(setting.options[static_cast<size_t>(value.integer)].name);
    }
    return std::nullopt;
}

// The player's language as the overlay chooses it (#158): ANVIL_LANGUAGE, the
// overlay's `language` setting, EU4's settings in the game's user folder, or,
// when Mei is in the load order of `report`, Steam's or Windows' Chinese,
// Japanese or Korean.
void follow_language(const fs::path& game, const cli::GameReport* report = nullptr) {
    if (!i18n::forced_language() && !game.empty()) {
        if (const auto language = overlay_language(game)) {
            i18n::set_language(*language);
            return;
        }
    }
    const bool cjk_text = report != nullptr && std::any_of(report->load_order.begin(), report->load_order.end(),
                                                           [](const auto& p) { return p.id == i18n::kCjkTextPlugin; });
    i18n::set_language(i18n::choose_language(eu4_user_dir(game), cjk_text).language);
}

// The problems of a check to show a player. On a build Anvil does not know,
// every symbol differs and says nothing the first line and the reason do not.
std::span<const cli::Problem> shown_problems(const cli::CheckReport& report) {
    const std::span<const cli::Problem> all(report.problems);
    return report.exe_build.empty() ? all.first(std::min<size_t>(all.size(), 1)) : all;
}

// `text` with its middle cut when it is wider than `width`.
std::string fitted(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const std::wstring wide = widen(text);
    for (size_t keep = wide.size(); keep > 8; --keep) {
        const std::string cut = narrow(wide.substr(0, keep / 2) + L"..." + wide.substr(wide.size() - (keep - keep / 2)));
        if (ImGui::CalcTextSize(cut.c_str()).x <= width) return cut;
    }
    return text;
}

bool is_standard(const Package& package, const std::string& id) {
    return std::find(package.standard_plugins.begin(), package.standard_plugins.end(), id) != package.standard_plugins.end();
}

bool listed(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

const char* step_text(Step::Kind kind) {
    switch (kind) {
    case Step::Kind::Write: return tr("write {0}");
    case Step::Kind::CopyManager: return tr("copy the Manager to {0}");
    case Step::Kind::Remove: return tr("remove {0}");
    case Step::Kind::RemoveTree: return tr("remove the folder {0}");
    case Step::Kind::RemoveIfEmpty: return tr("remove {0} if empty");
    case Step::Kind::RemoveManager: return tr("remove {0}");
    }
    return "";
}

std::string megabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / (1 << 20));
    return text;
}

// The file name of a pinned zip's URL, and where it comes from
// ("github.com/happy-dwarf-team/<repo>").
std::string zip_file_name(const CatalogPlugin& plugin) { return plugin.zip.url.substr(plugin.zip.url.rfind('/') + 1); }
std::string zip_source(const CatalogPlugin& plugin) {
    constexpr size_t scheme = std::string_view("https://").size();
    return plugin.zip.url.substr(scheme, plugin.zip.url.find("/releases/") - scheme);
}

}  // namespace

ManagerApp::ManagerApp(HWND window, std::optional<Package> package, std::string package_error, fs::path game)
    : window_(window), package_(std::move(package)), package_error_(std::move(package_error)), exe_(own_exe_path()),
      settings_(load_manager_settings()), benchmark_(window, settings_) {
    const fs::path steam = steam_root_from_registry();
    if (!steam.empty()) steam_games_ = find_steam_games(steam);
    gog_game_ = find_gog_game();
    if (game.empty() && runs_from(exe_, exe_.parent_path()) && inspect_game_folder(exe_.parent_path()).error.empty()) {
        game = exe_.parent_path();  // the copy in <EU4>\AnvilManager.exe
    }
    if (game.empty()) game = settings_.game_folder;
    if (game.empty() && steam_games_.size() + (gog_game_ ? 1 : 0) == 1) {
        game = gog_game_ ? *gog_game_ : steam_games_.front();
    }
    follow_language(game);
    if (!game.empty() && package_) choose_game(game);
    if (settings_.check_releases) release_pending_ = std::async(std::launch::async, fetch_latest_release);
    catalog_ = load_embedded_catalog(catalog_error_);
}

void ManagerApp::choose_game(const fs::path& dir) {
    game_ = dir.lexically_normal();
    gog_ = is_gog_folder(game_);
    follow_language(game_);
    snapshot_.reset();
    editor_.reset();
    crash_selected_.clear();
    crash_text_.clear();
    log_text_.clear();
    running_checked_ = {};
    launcher_opened_ = false;
    if (settings_.game_folder != game_) {
        settings_.game_folder = game_;
        std::string error;
        save_manager_settings(settings_, error);
    }
    refresh();
}

void ManagerApp::refresh() {
    if (game_.empty() || !package_) return;
    if (pending_.valid()) pending_.wait();
    pending_ = std::async(std::launch::async, [game = game_, package = &*package_, exe = exe_] {
        Snapshot snapshot;
        snapshot.folder = inspect_game_folder(game);
        if (!snapshot.folder.error.empty()) return snapshot;
        snapshot.installation = inspect_installation(*package, game, exe);
        std::error_code ec;
        if (fs::is_directory(game / "anvil", ec)) {
            cli::GameOptions options;
            options.game_dir = game;
            snapshot.report = cli::check_game(options);
        }
        snapshot.anvil_toml = read_anvil_settings(game / "anvil" / "anvil.toml");
        snapshot.crashes = crash_report_names(game / "anvil" / "crashes");
        return snapshot;
    });
}

void ManagerApp::poll() {
    using namespace std::chrono;
    if (pending_.valid() && pending_.wait_for(0s) == std::future_status::ready) {
        snapshot_ = pending_.get();
        follow_language(game_, snapshot_->report ? &*snapshot_->report : nullptr);
    }
    if (release_pending_.valid() && release_pending_.wait_for(0s) == std::future_status::ready) {
        release_ = release_pending_.get();
    }
    if (catalog_pending_.valid() && catalog_pending_.wait_for(0s) == std::future_status::ready) {
        CatalogFetch fetch = catalog_pending_.get();
        catalog_error_ = fetch.error;
        if (fetch.catalog) {
            catalog_ = std::move(fetch.catalog);
            catalog_fetched_ = true;
        }
    }
    if (download_pending_.valid() && download_pending_.wait_for(0s) == std::future_status::ready) {
        downloaded(download_pending_.get());
    }
    const auto now = steady_clock::now();
    if (!game_.empty() && now - running_checked_ > 1500ms) {
        running_checked_ = now;
        const bool was = running_;
        running_ = game_running(game_);
        if (running_) launcher_opened_ = false;  // the note did its job
        // The game may have written logs and crash reports; a benchmark series
        // is read once it ends, not between its starts.
        if (was && !running_ && !benchmark_.busy()) refresh();
    }
    if (benchmark_busy_ && !benchmark_.busy()) refresh();
    benchmark_busy_ = benchmark_.busy();
}

bool ManagerApp::read_only() const { return running_ || pending_.valid(); }

void ManagerApp::draw() {
    poll();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Anvil Manager", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (!package_) {
        colored_wrapped(kBad, trf("This AnvilManager.exe is broken: {0}. Download it again from {1}.", {package_error_, kReleasesUrl}));
        ImGui::End();
        return;
    }
    draw_game_bar();
    ImGui::Separator();
    if (game_.empty() || (snapshot_ && !snapshot_->folder.error.empty())) {
        draw_first_run();
    } else if (!snapshot_) {
        weak(tr("Reading the game folder..."));
    } else if (ImGui::BeginTabBar("tabs")) {
        // While a benchmark series runs, the other tabs are dimmed and locked.
        const bool locked = benchmark_.busy();
        // A tab's id stays English, so the open tab stays open when the language changes.
        const auto label = [](const char* name) { return std::string(tr(name)) + "###" + name; };
        const auto tab = [&](const char* name, ImGuiTabItemFlags flags, void (ManagerApp::*body)()) {
            ImGui::BeginDisabled(locked);
            const bool open = ImGui::BeginTabItem(label(name).c_str(), nullptr, locked ? 0 : flags);
            ImGui::EndDisabled();
            if (!open) return;
            (this->*body)();
            ImGui::EndTabItem();
        };
        tab(N_("Anvil"), 0, &ManagerApp::draw_anvil_tab);
        tab(N_("Catalog"), 0, &ManagerApp::draw_catalog_tab);
        tab(N_("Plugins"), 0, &ManagerApp::draw_plugins_tab);
        tab(N_("Mods"), 0, &ManagerApp::draw_mods_tab);
        tab(N_("Settings"), show_settings_tab_ ? ImGuiTabItemFlags_SetSelected : 0, &ManagerApp::draw_settings_tab);
        if (ImGui::BeginTabItem(label(N_("Benchmark")).c_str(), nullptr, locked ? ImGuiTabItemFlags_SetSelected : 0)) {
            draw_benchmark_tab();
            ImGui::EndTabItem();
        }
        tab(N_("Crashes"), 0, &ManagerApp::draw_crashes_tab);
        tab(N_("Log"), 0, &ManagerApp::draw_log_tab);
        tab(N_("About"), 0, &ManagerApp::draw_about_tab);
        ImGui::EndTabBar();
        show_settings_tab_ = false;
    }
    draw_modals();
    ImGui::End();
}

void ManagerApp::draw_game_bar() {
    ImGui::Text("Anvil Manager %s", ANVIL_VERSION);
    if (game_.empty()) return;
    ImGui::SameLine();
    weak("|");
    ImGui::SameLine();
    // The path gives way, so that Change and Play EU4 stay in the window.
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* play = tr("Play EU4");
    const float play_width = ImGui::CalcTextSize(play).x + style.FramePadding.x * 4;
    const float change_width = ImGui::CalcTextSize(tr("Change...")).x + style.FramePadding.x * 2;
    const std::string folder = u8(game_);
    const std::string shown = fitted(folder, ImGui::GetContentRegionAvail().x - play_width - change_width - style.ItemSpacing.x * 3);
    ImGui::TextUnformatted(shown.c_str());
    if (shown != folder && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", folder.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(running_ || benchmark_.busy());
    if (ImGui::SmallButton(tr("Change..."))) {
        if (const auto dir = pick_folder(window_, game_)) choose_game(*dir);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - play_width));
    const bool no_game = snapshot_ && !snapshot_->folder.error.empty();  // nothing here to start
    ImGui::BeginDisabled(running_ || no_game || benchmark_.busy());
    const fs::path gog_start = gog_ ? gog_program(game_) : fs::path();
    if (ImGui::Button((std::string(play) + "###play").c_str(), ImVec2(play_width, 0))) {
        const bool started = launch_game(gog_start);
        launcher_opened_ = started && gog_;
        if (!started && gog_) {
            show_message(tr("The Paradox launcher did not start"), trf("Windows could not open {0}.", {u8(gog_start)}));
        } else if (!started) {
            show_message(tr("Steam did not start"), tr("Windows could not open steam://rungameid/236850. Is Steam installed?"));
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", gog_ ? tr("Opens the Paradox launcher, as GOG does. Press Play there.") : tr("Starts the game through Steam"));
    }

    if (snapshot_ && snapshot_->folder.error.empty()) {
        if (snapshot_->folder.build.empty() && snapshot_->folder.gog) {
            colored_wrapped(kWarn, tr("eu4.exe is a GOG build Anvil does not know: plugins that need game addresses are "
                                      "skipped, the others load. Update the game to 1.37.5.2 in GOG Galaxy."));
            if (!snapshot_->folder.gog_version.empty()) weak(trf("GOG calls this version {0}.", {snapshot_->folder.gog_version}));
        } else if (snapshot_->folder.build.empty()) {
            colored_wrapped(kWarn, tr("eu4.exe is not a build Anvil knows (EU4 updated?): plugins that need game "
                                      "addresses are skipped, the others load."));
        } else {
            weak("eu4.exe: " + snapshot_->folder.build);
        }
    }
    if (launcher_opened_ && !running_) {
        weak_wrapped(tr("The Paradox launcher opens: press Play there and Anvil loads with the game. On its first start it shows Paradox's EULA before Play. Only you can accept it; it comes from the launcher, not from Anvil."));
    }
    if (benchmark_.busy()) {
        colored_wrapped(kWarn, tr("A start-up benchmark series is running: EU4 starts and quits by itself. The other "
                                  "tabs are locked until the series ends."));
    } else if (running_) {
        colored_wrapped(kWarn, tr("EU4 is running from this folder. Close the game to change Anvil, plugins or "
                                  "settings; until then the Manager only shows them."));
    }
}

void ManagerApp::draw_first_run() {
    if (snapshot_ && !snapshot_->folder.error.empty()) {
        colored_wrapped(kBad, u8(game_) + ": " + snapshot_->folder.error + ".");
        ImGui::Spacing();
    }
    wrapped(tr("Choose the Europa Universalis IV folder: the one with eu4.exe."));
    ImGui::Spacing();
    const auto use = [&](const fs::path& game) {
        ImGui::PushID(u8(game).c_str());
        if (ImGui::Button(tr("Use"))) choose_game(game);
        ImGui::SameLine();
        ImGui::TextUnformatted(u8(game).c_str());
        ImGui::PopID();
    };
    if (!steam_games_.empty()) {
        weak(tr("Steam has the game here:"));
        for (const fs::path& game : steam_games_) use(game);
        ImGui::Spacing();
    }
    if (gog_game_) {
        weak(tr("GOG has the game here:"));
        use(*gog_game_);
        ImGui::Spacing();
    }
    if (steam_games_.empty() && !gog_game_) weak(tr("Neither Steam nor GOG lists the game."));
    if (ImGui::Button(tr("Choose the folder..."))) {
        if (const auto dir = pick_folder(window_, game_)) choose_game(*dir);
    }
}

void ManagerApp::draw_anvil_tab() {
    const Installation& inst = *snapshot_->installation;
    const std::string& version = package_->version;
    const auto installed_version = Version::parse(inst.dll_version);
    const auto own_version = Version::parse(version);
    const bool newer_installed = installed_version && own_version && *own_version < *installed_version;

    ImGui::Spacing();
    switch (inst.dll) {
    case Installation::Dll::Missing:
        wrapped(inst.anvil_folder ? tr("Anvil is not installed (no version.dll). The anvil folder still has your "
                                       "plugins and settings: installing brings them back.")
                                  : tr("Anvil is not installed. The game starts as usual (vanilla)."));
        break;
    case Installation::Dll::Foreign:
        colored_wrapped(kWarn, tr("version.dll in the game folder is not Anvil's. Another mod or tool may use it: "
                                  "Anvil replaces it only if you say so."));
        break;
    case Installation::Dll::Anvil:
        if (inst.dll_version != version) {
            wrapped(trf("Anvil {0} is installed. This Manager carries Anvil {1}.", {inst.dll_version, version}));
            if (newer_installed) {
                colored_wrapped(kWarn, trf("The installed Anvil is newer: installing this one would go back to {0}. "
                                           "Use the Manager of the newer release instead.",
                                           {version}));
            }
        } else if (inst.intact()) {
            colored_wrapped(kGood, trf("Anvil {0} is installed and complete.", {version}));
        } else {
            colored_wrapped(kWarn, trf("Anvil {0} is installed, but some of its files differ from its package:", {version}));
        }
        break;
    }
    if (inst.manager_copy && !inst.manager_current && !runs_from(exe_, game_)) {
        weak_wrapped(tr("AnvilManager.exe in the game folder is another version than this one: installing replaces it."));
    }
    if (inst.dll == Installation::Dll::Anvil && inst.dll_version == version && !inst.intact()) {
        for (const auto& path : inst.missing) {
            if (path != kPlayerToml) bullet(trf("{0} is missing", {path}));
        }
        for (const auto& path : inst.differing) bullet(trf("{0} differs", {path}));
        for (const auto& path : inst.extra) bullet(trf("{0} does not belong to Anvil", {path}));
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(read_only());
    std::string label = inst.dll != Installation::Dll::Anvil ? trf("Install Anvil {0}", {version})
                        : inst.dll_version != version         ? trf("Install this Anvil {0}", {version})
                                                              : std::string(tr("Repair"));
    if (inst.dll == Installation::Dll::Foreign) {
        ImGui::Checkbox(tr("Replace the other version.dll"), &replace_foreign_dll_);
    }
    const bool nothing = inst.intact() && inst.dll_version == version && inst.manager_current;
    ImGui::BeginDisabled(nothing || (inst.dll == Installation::Dll::Foreign && !replace_foreign_dll_));
    if (ImGui::Button((label + "###install").c_str())) start_plan(false);
    ImGui::EndDisabled();
    if (nothing) {
        ImGui::SameLine();
        weak(tr("nothing to repair"));
    }
    ImGui::EndDisabled();
    weak_wrapped(trf("Copies version.dll, the standard plugins ({0}) and this Manager into the game folder. Your anvil.toml, "
             "plugins and their settings stay.",
             {join(package_->standard_plugins)}));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only() || (inst.dll != Installation::Dll::Anvil && !inst.anvil_folder && !inst.manager_copy));
    ImGui::Checkbox(tr("Also remove my plugins, their settings and data, crash reports and logs"), &remove_everything_);
    if (ImGui::Button(tr("Uninstall Anvil"))) start_plan(true);
    ImGui::EndDisabled();
    weak(tr("Without version.dll the game starts vanilla at once."));

    if (!settings_.asked_releases) {
        ImGui::Spacing();
        ImGui::Separator();
        wrapped(tr("Should the Manager ask GitHub for new Anvil releases when it starts? It only shows a link; "
                   "nothing is downloaded. You can change this in About."));
        const auto answer = [&](bool yes) {
            settings_.asked_releases = true;
            settings_.check_releases = yes;
            std::string error;
            save_manager_settings(settings_, error);
            if (yes && !release_pending_.valid()) release_pending_ = std::async(std::launch::async, fetch_latest_release);
        };
        if (ImGui::Button(tr("Yes, check for releases"))) answer(true);
        ImGui::SameLine();
        if (ImGui::Button(tr("No"))) answer(false);
    }
    if (release_ && release_->release && newer_release(release_->release->tag, ANVIL_VERSION)) {
        ImGui::Spacing();
        colored_wrapped(kGood, trf("Anvil {0} is available.", {release_->release->tag}));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Open its page"))) open_url(release_->release->url);
    }
}

void ManagerApp::draw_catalog_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    wrapped(gog_ ? tr("Plugins of the Anvil team. Install puts one in anvil/plugins.")
                 : tr("Plugins of the Anvil team. Install puts one in anvil/plugins; a plugin from the Steam Workshop comes "
                      "with its mod instead."));
    ImGui::BeginDisabled(catalog_pending_.valid());
    if (ImGui::Button(tr("Get the current list"))) catalog_pending_ = std::async(std::launch::async, fetch_catalog);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (catalog_pending_.valid()) weak(tr("asking GitHub..."));
    else weak(catalog_fetched_ ? std::string(tr("The list from GitHub.")) : trf("The list of Manager {0}.", {ANVIL_VERSION}));
    weak_wrapped(tr("Nothing is downloaded until you click: the list from the anvil repository on GitHub, and a plugin's "
                    "zip from its GitHub release, installed only if its SHA-256 equals the list's."));
    if (!catalog_error_.empty()) colored_wrapped(kWarn, trf("The list was refused: {0}", {catalog_error_}));
    if (!catalog_) return;
    if (!report) colored_wrapped(kWarn, tr("Install Anvil first, then its plugins."));
    if (download_pending_.valid() && download_) weak(trf("Downloading {0}...", {zip_file_name(*download_)}));

    std::vector<std::pair<std::string, std::string>> local;
    std::vector<std::string> mods;
    if (report) {
        for (const cli::GamePlugin& plugin : report->plugins) {
            if (plugin.report.manifest) local.emplace_back(plugin.report.manifest->id, plugin.report.manifest->version.to_string());
        }
        for (const cli::GameModPlugin& plugin : report->mod_plugins) mods.push_back(plugin.id);
    }
    const auto& installation = snapshot_->installation;
    const std::string anvil_version = installation && installation->installed() ? installation->dll_version : "";
    const std::string& build = snapshot_->folder.build;
    const bool steam = !gog_;

    for (const CatalogPlugin& plugin : catalog_->plugins) {
        const CatalogStatus status = catalog_status(plugin, build, anvil_version, local, mods);
        ImGui::PushID(plugin.id.c_str());
        ImGui::Separator();
        ImGui::Text("%s %s", plugin.name.c_str(), plugin.version.c_str());
        if (!plugin.summary.empty()) wrapped(plugin.summary);
        if (!plugin.note.empty()) weak_wrapped(plugin.note);
        if (plugin.affects_gameplay) {
            weak_wrapped(tr("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there."));
        }

        bool can_install = report && report->cannot_check.empty();
        if (!status.for_build) {
            colored_wrapped(kWarn, build.empty() ? std::string(tr("It runs only on a game build Anvil knows."))
                                                 : trf("Not made for this game's build; it runs on {0}.", {join(plugin.builds)}));
            can_install = false;
        } else if (!status.anvil_ok) {
            colored_wrapped(kWarn, trf("It needs Anvil {0}: run the Manager of a newer Anvil first.", {plugin.anvil}));
            can_install = false;
        }
        const auto installed = Version::parse(status.installed);
        const auto pinned = Version::parse(plugin.version);
        const bool newer_installed = installed && pinned && *pinned < *installed;
        if (status.current(plugin)) {
            colored_wrapped(kGood, tr("Installed."));
        } else if (!status.installed.empty()) {
            weak_wrapped(trf("Version {0} is installed.", {status.installed}));
        }
        if (status.from_mod) {
            weak_wrapped(status.installed.empty()
                             ? tr("An enabled mod carries it (see the Mods tab). A copy installed here would load instead.")
                             : tr("An enabled mod carries it too; the copy in anvil/plugins loads."));
        }

        if (!status.current(plugin) && !newer_installed) {
            ImGui::BeginDisabled(!can_install || read_only() || download_pending_.valid());
            if (ImGui::Button(status.installed.empty() ? tr("Install") : tr("Update"))) {
                download_ = plugin;
                open_download_ = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (ImGui::Button(tr("Open its page"))) open_url(plugin.page);
        if (!plugin.workshop.empty() && steam) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Subscribe on Steam Workshop"))) open_url(plugin.workshop);
        }
        ImGui::PopID();
    }
}

void ManagerApp::draw_plugins_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only() || !report || !report->cannot_check.empty());
    if (ImGui::Button(tr("Install a plugin from a zip..."))) {
        if (const auto file = pick_zip(window_)) open_zip(*file);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Open the plugins folder"))) open_folder(game_ / "anvil" / "plugins");
    ImGui::SameLine();
    ImGui::BeginDisabled(pending_.valid());
    if (ImGui::Button(tr("Check again"))) refresh();
    ImGui::EndDisabled();
    weak(tr("You can also drop a plugin zip on this window."));
    ImGui::Spacing();

    if (!report) {
        wrapped(tr("Anvil is not installed in this folder: no plugins load."));
        return;
    }
    if (!report->cannot_check.empty()) {
        colored_wrapped(kBad, trf("Cannot check the plugins: {0}", {report->cannot_check}));
        return;
    }
    for (const cli::Problem& problem : report->problems) {
        // dlc_load.json and trust.toml are shown on the Mods tab.
        if (u8(fs::path(widen(problem.file)).filename()) == "anvil.toml") colored_wrapped(kBad, "anvil.toml: " + problem.message);
    }
    if (!report->mod_plugins.empty()) {
        size_t waiting = 0;
        for (const cli::GameModPlugin& p : report->mod_plugins) {
            waiting += p.state == "pending" || p.state == "update-pending" || p.state == "conflict" ? 1 : 0;
        }
        const std::string count = std::to_string(report->mod_plugins.size());
        const std::string text = waiting > 0 ? trf("{0} plugin(s) from enabled mods, {1} waiting for your approval: see the Mods tab.",
                                                   {count, std::to_string(waiting)})
                                             : trf("{0} plugin(s) from enabled mods: see the Mods tab.", {count});
        if (waiting > 0) colored_wrapped(kWarn, text);
        else weak(text);
    }
    if (!report->enabled) colored_wrapped(kWarn, tr("anvil.toml says enabled = false: no plugin loads."));
    if (snapshot_->installation && !snapshot_->installation->installed()) {
        colored_wrapped(kWarn, tr("version.dll is not Anvil's or missing: nothing below loads until Anvil is installed."));
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("plugins", 5, flags)) {
        ImGui::TableSetupColumn(tr("Load"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(tr("Plugin"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(tr("Version"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(tr("Status"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const cli::GamePlugin& plugin : report->plugins) {
            const auto& manifest = plugin.report.manifest;
            const std::string id = manifest ? manifest->id : plugin.folder;
            ImGui::PushID(plugin.folder.c_str());
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool load = !listed(report->disabled_plugins, id);
            ImGui::BeginDisabled(read_only() || !manifest);
            if (ImGui::Checkbox("##load", &load)) edit_list("disabled_plugins", id, !load);
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(manifest && !manifest->name.empty() ? manifest->name.c_str() : plugin.folder.c_str());
            if (manifest && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
                ImGui::TextUnformatted(trf("id {0}, folder {1}", {manifest->id, plugin.folder}).c_str());
                if (!manifest->authors.empty()) ImGui::TextUnformatted(trf("by {0}", {join(manifest->authors)}).c_str());
                if (!manifest->description.empty()) ImGui::TextUnformatted(manifest->description.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            if (is_standard(*package_, id)) {
                ImGui::SameLine();
                weak(tr("(comes with Anvil)"));
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(manifest ? manifest->version.to_string().c_str() : "-");

            ImGui::TableNextColumn();
            const auto skipped = std::find_if(report->skipped.begin(), report->skipped.end(),
                                              [&](const cli::SkippedPlugin& s) { return s.folder == plugin.folder; });
            if (plugin.status == "loaded") {
                ImGui::TextColored(kGood, "%s", tr("loads"));
            } else if (plugin.status == "disabled") {
                weak(tr("disabled"));
            } else {
                colored_wrapped(kBad, skipped != report->skipped.end() ? trf("skipped: {0}", {i18n::text(skipped->why)})
                                                                       : std::string(tr("skipped")));
            }
            for (const cli::Problem& problem : shown_problems(plugin.report)) {
                ImGui::PushStyleColor(ImGuiCol_Text, plugin.disabled ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : kWarn);
                wrapped(problem.message);
                ImGui::PopStyleColor();
            }
            if (manifest && manifest->affects_gameplay) {
                weak_wrapped(tr("Changes the game: kept out of ironman and multiplayer unless allowed."));
                ImGui::BeginDisabled(read_only());
                if (manifest->allow_ironman == Allow::OptIn) {
                    bool allow = listed(snapshot_->anvil_toml.allow_ironman, id);
                    if (ImGui::Checkbox(tr("Allow in ironman"), &allow)) edit_list("allow_ironman", id, allow);
                    ImGui::SameLine();
                }
                if (manifest->allow_multiplayer == Allow::OptIn) {
                    bool allow = listed(snapshot_->anvil_toml.allow_multiplayer, id);
                    if (ImGui::Checkbox(tr("Allow in multiplayer"), &allow)) edit_list("allow_multiplayer", id, allow);
                }
                ImGui::EndDisabled();
            }

            ImGui::TableNextColumn();
            if (manifest && !manifest->settings.empty() && ImGui::SmallButton(tr("Settings"))) open_settings(plugin);
            if (!is_standard(*package_, id)) {
                ImGui::BeginDisabled(read_only());
                if (ImGui::SmallButton(tr("Remove"))) ImGui::OpenPopup("remove");
                ImGui::EndDisabled();
            }
            if (ImGui::BeginPopup("remove")) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
                ImGui::TextUnformatted(trf("Remove the folder {0}? Its settings and data in anvil/data stay.",
                                           {"anvil/plugins/" + plugin.folder})
                                           .c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::Button(tr("Remove"))) {
                    std::string error;
                    if (!remove_plugin_folder(game_, plugin.folder, package_->standard_plugins, error)) {
                        failed(tr("The plugin was not removed"), error, false);
                    }
                    refresh();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (report->plugins.empty()) weak(tr("No plugins are installed."));

    if (!report->load_order.empty()) {
        ImGui::Spacing();
        weak(tr("Load order:"));
        std::string order;
        for (const cli::LoadedPlugin& p : report->load_order) order += (order.empty() ? "" : " > ") + p.id + " " + p.version +
                                                                    (p.mod.empty() ? "" : " " + trf("(mod '{0}')", {p.mod}));
        wrapped(order);
    }
    for (const cli::OverlapWarning& warning : report->warnings) colored_wrapped(kWarn, warning.message);
}

namespace {

std::string mod_source(const cli::GameModPlugin& p) {
    return trf("mod '{0}' ({1})", {p.mod, p.workshop_id.empty() ? std::string(tr("local mod")) : trf("Workshop {0}", {p.workshop_id})});
}

// What a plugin from a mod waits for, or does, in the player's words.
std::string mod_state_text(const cli::GameModPlugin& p) {
    if (p.state == "approved") return p.status == "loaded" ? std::string(tr("approved: loads")) : trf("approved, but {0}", {i18n::text(p.skip_why)});
    if (p.state == "update-pending") return trf("an update waits for approval; the approved {0} loads", {p.approved_version});
    if (p.state == "pending") return tr("waits for approval: does not load");
    if (p.state == "conflict") return tr("conflict: does not load");
    if (p.state == "shadowed") return tr("a plugin in anvil/plugins has this id and loads instead");
    return tr("cannot be approved: does not load");
}

}  // namespace

void ManagerApp::draw_mods_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    ImGui::BeginDisabled(pending_.valid());
    if (ImGui::Button(tr("Check again"))) refresh();
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (!report) {
        wrapped(tr("Anvil is not installed in this folder: no plugins load."));
        return;
    }
    wrapped(trf("Mods from the Workshop or your mod folder can carry plugins in their anvil/<id>/ folder. Only the "
                "mods enabled in the playset the game starts with count ({0} enabled; the launcher changes the "
                "playset). A plugin from a mod loads only after you approve its exact files; an update waits for a "
                "new approval while the approved version keeps loading.",
                {std::to_string(report->enabled_mods)}));
    if (!report->playset_found) weak_wrapped(trf("No dlc_load.json in {0}: no mods are enabled.", {report->user_dir}));
    for (const std::string& w : report->mod_warnings) colored_wrapped(kWarn, w);
    for (const cli::Problem& problem : report->problems) {
        const std::string file = u8(fs::path(widen(problem.file)).filename());
        if (file != "anvil.toml") colored_wrapped(kBad, file + ": " + problem.message);
    }
    if (report->mod_plugins.empty()) {
        ImGui::Spacing();
        weak(tr("No enabled mod has plugins."));
        return;
    }
    if (read_only() && running_) colored_wrapped(kWarn, tr("The game is running: approvals can be changed after it closes."));
    ImGui::Spacing();
    for (const cli::GameModPlugin& p : report->mod_plugins) {
        ImGui::PushID((p.id + "/" + p.descriptor).c_str());
        ImGui::Separator();
        const auto& m = p.report.manifest;
        ImGui::Text("%s %s", m && !m->name.empty() ? m->name.c_str() : p.id.c_str(), p.version.c_str());
        ImGui::SameLine();
        weak(trf("id {0}, {1}", {p.id, mod_source(p)}));
        const bool good = p.state == "approved" && p.status == "loaded";
        const bool waits = p.state == "pending" || p.state == "update-pending" || p.state == "conflict";
        colored_wrapped(good ? kGood : waits ? kWarn : p.state == "shadowed" ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : kBad,
                        mod_state_text(p));
        weak(p.why.empty() ? p.reason : i18n::text(p.why));
        if (!p.others.empty()) weak(trf("Other mods with this id: {0}", {join(p.others)}));
        for (const cli::Problem& problem : shown_problems(p.report)) colored_wrapped(kWarn, problem.message);
        const bool approved_here = !p.approved_hash.empty() && p.approved_hash == p.hash;
        const bool can_approve = p.state != "invalid" && p.state != "shadowed" && !p.hash.empty() && m && !approved_here;
        ImGui::BeginDisabled(read_only());
        if (can_approve && ImGui::Button(p.state == "update-pending" ? tr("Approve the update...") : tr("Approve..."))) {
            approve_ = p;
            open_approve_ = true;
        }
        if (!p.approved_hash.empty()) {
            if (can_approve) ImGui::SameLine();
            if (ImGui::Button(tr("Revoke the approval"))) {
                std::string error;
                if (!revoke_shown_plugin(game_, p, error)) failed(tr("The approval was not revoked"), error, false);
                refresh();
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

void ManagerApp::draw_approve_modal() {
    next_modal(40);
    if (!ImGui::BeginPopupModal((std::string(tr("Approve a plugin from a mod")) + "###approve").c_str(), nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (!approve_ || !snapshot_ || !snapshot_->report) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const cli::GameModPlugin& p = *approve_;
    const Manifest& m = *p.report.manifest;
    ImGui::Text("%s %s", m.name.empty() ? m.id.c_str() : m.name.c_str(), p.version.c_str());
    weak(m.authors.empty() ? trf("id {0}", {m.id}) : trf("id {0}, by {1}", {m.id, join(m.authors)}));
    weak(trf("from {0}", {mod_source(p)}));
    if (!m.description.empty()) wrapped(m.description);
    if (!m.repository.empty() && m.repository.starts_with("https://")) {
        if (ImGui::SmallButton(tr("Open its repository"))) open_url(m.repository);
    }
    ImGui::Spacing();
    colored_wrapped(kWarn, tr("A plugin is native code: it runs inside the game with your rights, like any program. "
                              "Approve plugins only from authors you trust."));
    if (m.affects_gameplay) {
        bullet(tr("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there."));
    } else {
        bullet(tr("It says it does not change the game (affects_gameplay = false)."));
    }
    bullet(p.report.has_symbols ? trf("{0} game address(es) declared in symbols.toml", {std::to_string(p.report.symbols)})
                                : std::string(tr("It declares no game addresses (no symbols.toml).")));
    bullet(trf("Hash of its files: {0}", {p.hash}));
    if (p.state == "update-pending") bullet(trf("It replaces the approved {0} from the next start.", {p.approved_version}));
    if (p.state == "conflict") bullet(tr("Other enabled mods have a plugin with this id; this approval picks this one."));
    if (p.status == "skipped" && !p.report.skip_reason.empty()) {
        colored_wrapped(kBad, trf("Anvil would still skip it: {0}", {i18n::text(p.report.skip_why)}));
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("Approve"))) {
        std::string error;
        if (!approve_shown_plugin(game_, eu4_user_dir(game_), *snapshot_->report, p, error)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            approve_.reset();
            failed(tr("The plugin was not approved"), error, false);
            refresh();
            return;
        }
        approve_.reset();
        refresh();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        approve_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::open_settings(const cli::GamePlugin& plugin) {
    const Manifest& manifest = *plugin.report.manifest;
    SettingsEditor editor;
    editor.plugin = manifest.id;
    editor.player_file = game_ / "anvil" / "data" / widen(manifest.id) / "settings.toml";
    const SettingsFileResult schema =
        load_settings(game_ / "anvil" / "plugins" / widen(plugin.folder) / fs::path(widen(manifest.settings)));
    if (!schema.file) {
        editor.error = schema.error;
    } else {
        editor.schema = *schema.file;
        PlayerSettings player = load_player_settings(editor.schema, editor.player_file);
        editor.warnings = std::move(player.warnings);
        for (const Setting& setting : editor.schema.settings) {
            const SettingValue value = player.values[setting.id].value_or(setting.default_value);
            editor.values.push_back(value);
            editor.texts.push_back(setting.type == SettingType::Key      ? key_name(value.integer)
                                   : setting.type == SettingType::String ? value.text
                                                                          : std::string());
        }
    }
    editor_ = std::move(editor);
    show_settings_tab_ = true;
}

void ManagerApp::draw_settings_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    if (!report || report->plugins.empty()) {
        wrapped(tr("No installed plugin has settings."));
        return;
    }
    const std::string current = editor_ ? editor_->plugin : std::string(tr("Choose a plugin"));
    if (ImGui::BeginCombo((std::string(tr("Plugin")) + "###plugin").c_str(), current.c_str())) {
        for (const cli::GamePlugin& plugin : report->plugins) {
            const auto& manifest = plugin.report.manifest;
            if (!manifest || manifest->settings.empty()) continue;
            if (ImGui::Selectable(manifest->id.c_str(), editor_ && editor_->plugin == manifest->id)) open_settings(plugin);
        }
        ImGui::EndCombo();
    }
    if (!editor_) {
        weak_wrapped(tr("Plugins with a settings.toml can be set up here. The values go to anvil/data/<plugin>/settings.toml "
                "and apply when the game starts."));
        return;
    }
    SettingsEditor& editor = *editor_;
    if (!editor.error.empty()) {
        colored_wrapped(kBad, editor.error);
        return;
    }
    for (const std::string& warning : editor.warnings) colored_wrapped(kWarn, trf("Ignored in your file: {0}", {warning}));
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only());
    std::string group;
    for (const Setting& setting : editor.schema.settings) {
        SettingValue& value = editor.values[setting.id];
        std::string& text = editor.texts[setting.id];
        if (setting.group != group) {
            group = setting.group;
            ImGui::Spacing();
            ImGui::SeparatorText(group.c_str());
        }
        ImGui::PushID(static_cast<int>(setting.id));
        const std::string label = setting.label.empty() ? setting.name : setting.label;
        bool changed = false;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        switch (setting.type) {
        case SettingType::Bool: {
            bool on = value.integer != 0;
            if (ImGui::Checkbox(label.c_str(), &on)) {
                value.integer = on ? 1 : 0;
                changed = true;
            }
            break;
        }
        case SettingType::Int: {
            const int64_t step = setting.step ? setting.step->integer : 1;
            changed = ImGui::InputScalar(label.c_str(), ImGuiDataType_S64, &value.integer, &step);
            if (setting.min) value.integer = std::max(value.integer, setting.min->integer);
            if (setting.max) value.integer = std::min(value.integer, setting.max->integer);
            break;
        }
        case SettingType::Float: {
            const double step = setting.step ? setting.step->real : 0.0;
            changed = ImGui::InputDouble(label.c_str(), &value.real, step, step * 10, "%.4g");
            if (setting.min) value.real = std::max(value.real, setting.min->real);
            if (setting.max) value.real = std::min(value.real, setting.max->real);
            break;
        }
        case SettingType::Enum: {
            const auto index = static_cast<size_t>(value.integer);
            const std::string shown = index < setting.options.size() ? setting.options[index].label : "?";
            if (ImGui::BeginCombo(label.c_str(), shown.c_str())) {
                for (size_t i = 0; i < setting.options.size(); ++i) {
                    if (ImGui::Selectable(setting.options[i].label.c_str(), i == index)) {
                        value.integer = static_cast<int64_t>(i);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case SettingType::String:
            if (ImGui::InputText(label.c_str(), &text)) {
                value.text = text;
                changed = true;
            }
            break;
        case SettingType::Key:
            if (ImGui::InputTextWithHint(label.c_str(), tr("no key"), &text)) {
                if (const auto code = parse_key(text)) value.integer = *code;
                changed = true;
            }
            if (!parse_key(text)) {
                ImGui::SameLine();
                ImGui::TextColored(kBad, "%s", tr("not a key (e.g. Ctrl+Shift+F12)"));
            }
            break;
        }
        if (!(value == setting.default_value)) {
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Default"))) {
                value = setting.default_value;
                text = setting.type == SettingType::Key ? key_name(value.integer) : value.text;
                changed = true;
            }
        }
        if (!setting.description.empty()) {
            ImGui::Indent();
            weak(setting.description);
            ImGui::Unindent();
        }
        editor.dirty = editor.dirty || changed;
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!editor.dirty);
    if (ImGui::Button(tr("Save"))) save_settings();
    ImGui::SameLine();
    if (ImGui::Button(tr("Undo changes"))) {
        const auto& plugins = snapshot_->report->plugins;
        const auto it = std::find_if(plugins.begin(), plugins.end(), [&](const cli::GamePlugin& p) {
            return p.report.manifest && p.report.manifest->id == editor.plugin;
        });
        if (it != plugins.end()) open_settings(*it);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    weak_wrapped(trf("Saved to {0}; values apply when the game starts.", {u8(editor_ ? editor_->player_file : fs::path())}));
}

void ManagerApp::save_settings() {
    SettingsEditor& editor = *editor_;
    for (const Setting& setting : editor.schema.settings) {
        if (setting.type == SettingType::Key && !parse_key(editor.texts[setting.id])) {
            show_message(tr("Not saved"), trf("{0}: '{1}' is not a key.", {setting.label.empty() ? setting.name : setting.label,
                                                                            editor.texts[setting.id]}));
            return;
        }
        if (const std::string why = check_setting_value(setting, editor.values[setting.id]); !why.empty()) {
            show_message(tr("Not saved"), (setting.label.empty() ? setting.name : setting.label) + ": " + why);
            return;
        }
    }
    std::string error;
    if (!write_player_settings(editor.schema, editor.values, editor.player_file, error)) {
        failed(tr("The settings were not saved"), error, error.find("error 5)") != std::string::npos);
        return;
    }
    editor.dirty = false;
    editor.warnings.clear();
}

void ManagerApp::draw_benchmark_tab() {
    BenchmarkContext context;
    context.game = game_;
    context.gog = gog_;
    if (snapshot_->report && snapshot_->report->cannot_check.empty()) context.report = &*snapshot_->report;
    if (snapshot_->installation && snapshot_->installation->dll == Installation::Dll::Anvil) {
        context.installed_anvil = snapshot_->installation->dll_version;
    }
    context.exe_build = snapshot_->folder.build;
    context.exe_sha256 = snapshot_->folder.exe_sha256;
    benchmark_.draw(context);
}

void ManagerApp::draw_crashes_tab() {
    ImGui::Spacing();
    const fs::path folder = game_ / "anvil" / "crashes";
    if (ImGui::Button(tr("Open the crashes folder"))) open_folder(folder);
    ImGui::Spacing();
    if (snapshot_->crashes.empty()) {
        wrapped(tr("No crash reports. When the game crashes with Anvil, a report is written here (the last 10 are kept)."));
        return;
    }
    ImGui::BeginChild("list", ImVec2(ImGui::GetFontSize() * 14, 0), ImGuiChildFlags_Borders);
    for (const std::string& name : snapshot_->crashes) {
        if (ImGui::Selectable(name.c_str(), name == crash_selected_)) {
            crash_selected_ = name;
            crash_text_ = read_text_file(folder / widen(name + ".txt"), 1 << 20).value_or(tr("(the report's text cannot be read)"));
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("report");
    if (crash_selected_.empty()) {
        weak(tr("Choose a report."));
    } else if (const auto crash = load_crash_summary(folder, crash_selected_)) {
        const bool named = !crash->summary.empty() || !crash->plugins.empty();
        wrapped(crash->time + ": " +
                (named ? i18n::text(i18n::crash_message(crash->level, crash->plugins, crash->summary)) : std::string(tr("a crash"))));
        if (!crash->plugins.empty()) wrapped(trf("Plugins involved: {0}", {join(crash->plugins)}));
        if (crash->before_game_ready) weak(tr("Before the game was ready."));
        if (ImGui::Button(tr("Copy the report"))) copy_to_clipboard(window_, crash_text_);
        if (!crash->issues.empty()) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Report it to the plugin's author"))) open_url(crash->issues);
        }
        ImGui::InputTextMultiline("##text", &crash_text_, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
    } else {
        weak(tr("The report cannot be read."));
    }
    ImGui::EndChild();
}

void ManagerApp::draw_log_tab() {
    ImGui::Spacing();
    bool& previous = log_previous_;
    const fs::path file = game_ / "anvil" / (previous ? "anvil.prev.log" : "anvil.log");
    bool reload = log_text_.empty();
    if (ImGui::RadioButton(tr("Last start"), !previous && !log_report_)) {
        previous = false;
        reload = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("The start before"), previous && !log_report_)) {
        previous = true;
        reload = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Reload"))) reload = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy"))) copy_to_clipboard(window_, log_text_);
    ImGui::SameLine();
    if (ImGui::Button(tr("Open the folder"))) open_folder(game_ / "anvil");
    if (reload) {
        log_report_ = false;
        log_text_ = read_text_tail(file, 2000, 512 << 10);
        if (log_text_.empty()) log_text_ = trf("({0} is empty or missing: Anvil writes it when the game starts)", {u8(file.filename())});
    }

    // The problem report (issue #186): what the bug forms ask for, with private paths shortened, shown where the
    // log was so the player sees what the clipboard holds.
    if (ImGui::Button(tr("Copy a problem report"))) {
        log_text_ = make_problem_report();
        log_report_ = true;
        report_copied_ = copy_to_clipboard(window_, log_text_);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Open an Anvil issue"))) open_url("https://github.com/happy-dwarf-team/anvil/issues/new/choose");
    if (log_report_) {
        weak_wrapped(report_copied_ ? tr("The report is in the clipboard. Nothing is uploaded.")
                                    : tr("The clipboard could not be opened."));
        weak_wrapped(tr("Check it before you post it. If a plugin is skipped or named in a crash, report it to that "
                        "plugin's author instead."));
    } else {
        weak_wrapped(tr("For an issue: versions, the game build, the plugins and why one is skipped, and the end of "
                        "anvil.log, with your folders and Windows user name left out."));
    }
    ImGui::InputTextMultiline("##log", &log_text_, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
}

std::string ManagerApp::make_problem_report() const {
    ProblemReportInput input;
    input.manager_version = ANVIL_VERSION;
    input.installation = snapshot_->installation;
    input.exe_sha256 = snapshot_->folder.exe_sha256;
    input.build = snapshot_->folder.build;
    input.gog = snapshot_->folder.gog;
    input.gog_version = snapshot_->folder.gog_version;
    if (snapshot_->report) input.game = &*snapshot_->report;
    if (!snapshot_->crashes.empty()) input.newest_crash = load_crash_summary(game_ / "anvil" / "crashes", snapshot_->crashes.front());
    input.log_name = "anvil.log";
    input.log_tail = read_text_tail(game_ / "anvil" / "anvil.log", kProblemReportLogLines, kProblemReportLogBytes);

    const auto environment = [](const wchar_t* name) {
        wchar_t value[512];
        const DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
        return n > 0 && n < std::size(value) ? std::wstring(value, n) : std::wstring();
    };
    input.folders.push_back({game_, "<game>"});
    if (snapshot_->report && !snapshot_->report->user_dir.empty()) {
        input.folders.push_back({fs::path(widen(snapshot_->report->user_dir)), "<eu4 user folder>"});
    }
    if (const std::wstring profile = environment(L"USERPROFILE"); !profile.empty()) input.folders.push_back({profile, "<user folder>"});
    input.user_name = u8(environment(L"USERNAME"));
    return problem_report(input);
}

void ManagerApp::draw_about_tab() {
    ImGui::Spacing();
    wrapped(trf("Anvil Manager {0} installs Anvil {1}, the native plugin loader for Europa Universalis IV 1.37.5, and "
                "manages its plugins.",
                {ANVIL_VERSION, package_->version}));
    weak_wrapped(tr("Unofficial community project, not affiliated with Paradox Interactive. MIT License."));
    if (ImGui::Button(tr("Anvil on GitHub"))) open_url("https://github.com/happy-dwarf-team/anvil");
    ImGui::Spacing();
    ImGui::SeparatorText(tr("New releases"));
    bool check = settings_.check_releases;
    if (ImGui::Checkbox(tr("Ask GitHub for the latest release when the Manager starts"), &check)) {
        settings_.check_releases = check;
        settings_.asked_releases = true;
        std::string error;
        save_manager_settings(settings_, error);
    }
    weak_wrapped(tr("Only a link is shown; nothing is downloaded. To update Anvil, run the Manager of the new release."));
    ImGui::BeginDisabled(release_pending_.valid());
    if (ImGui::Button(tr("Check now"))) release_pending_ = std::async(std::launch::async, fetch_latest_release);
    ImGui::EndDisabled();
    if (release_pending_.valid()) {
        ImGui::SameLine();
        weak(tr("asking GitHub..."));
    } else if (release_) {
        ImGui::SameLine();
        if (!release_->release) {
            ImGui::TextColored(kWarn, "%s", release_->error.c_str());
        } else if (newer_release(release_->release->tag, ANVIL_VERSION)) {
            ImGui::TextColored(kGood, "%s", trf("Anvil {0} is available.", {release_->release->tag}).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Open its page"))) open_url(release_->release->url);
        } else {
            ImGui::TextUnformatted(trf("You have the latest release ({0}).", {release_->release->tag}).c_str());
        }
    }
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Third-party code"));
    weak_wrapped(tr("Dear ImGui, miniz, toml++ and Zydis (MIT License). Their license texts are in anvil/docs/third-party "
            "after installing."));
    weak_wrapped(tr("Chinese, Japanese and Korean text uses subsets of Source Han Sans by Adobe (SIL Open Font License "
                    "1.1, Reserved Font Name 'Source'). The license is in anvil/docs/third-party after installing."));
}

void ManagerApp::start_plan(bool uninstall) {
    const Installation& inst = *snapshot_->installation;
    PendingPlan pending;
    pending.uninstall = uninstall;
    pending.plan = uninstall ? plan_uninstall(*package_, inst, remove_everything_)
                             : plan_install(*package_, inst, game_, exe_, replace_foreign_dll_);
    plan_ = std::move(pending);
    open_plan_ = true;
}

void ManagerApp::run_plan() {
    const PendingPlan pending = std::move(*plan_);
    plan_.reset();
    if (game_running(game_)) {
        running_ = true;
        show_message(tr("Nothing changed"), tr("EU4 is running from this folder. Close it first."));
        return;
    }
    const ApplyResult result = apply_plan(*package_, pending.plan, game_, exe_);
    refresh();
    if (!result.ok) {
        failed(pending.uninstall ? tr("Anvil was not fully uninstalled") : tr("Anvil was not fully installed"),
               result.error + (result.done > 0 ? "\n\n" + std::string(tr("The steps before it are done; run it again once "
                                                                        "the cause is fixed."))
                                               : std::string()),
               result.access_denied);
        return;
    }
    if (!pending.uninstall) {
        show_message(tr("Done"), trf("Anvil {0} is installed. Start the game to load your plugins.", {package_->version}));
    } else if (result.manager_left) {
        const bool scheduled = remove_after_exit(game_ / kManagerExe);
        show_message(tr("Done"), scheduled ? tr("Anvil is uninstalled. The Manager closes now and removes its copy in "
                                                "the game folder.")
                                           : tr("Anvil is uninstalled. Delete AnvilManager.exe in the game folder "
                                                "yourself once the Manager is closed."));
        close_after_message_ = true;
    } else {
        show_message(tr("Done"), tr("Anvil is uninstalled. The game starts vanilla."));
    }
}

void ManagerApp::open_zip(const fs::path& file) {
    if (read_only() || !snapshot_ || !snapshot_->report) {
        show_message(tr("Not now"), running_ ? tr("Close the game first.") : tr("Install Anvil first, then its plugins."));
        return;
    }
    std::string error;
    auto zip = ZipArchive::open_file(file, error);
    if (!zip) {
        show_message(tr("Not a plugin zip"), u8(file.filename()) + ": " + error);
        return;
    }
    check_zip(std::move(*zip), u8(file.filename()), nullptr);
}

void ManagerApp::check_zip(ZipArchive zip, std::string name, const CatalogPlugin* pin) {
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    zip_check_ = check_plugin_zip(zip, game_, temp, package_->standard_plugins);
    if (pin != nullptr && zip_check_->error.empty()) zip_check_->error = check_pinned_manifest(*pin, zip_check_->id, zip_check_->version);
    zip_ = std::move(zip);
    zip_name_ = std::move(name);
    zip_anyway_ = false;
    open_zip_ = true;
}

void ManagerApp::downloaded(ZipDownload download) {
    const std::optional<CatalogPlugin> plugin = std::move(download_);
    download_.reset();
    if (!plugin) return;
    if (!download.error.empty()) {
        show_message(tr("The plugin was not installed"), zip_file_name(*plugin) + ": " + download.error);
        return;
    }
    if (read_only() || !snapshot_ || !snapshot_->report) {
        show_message(tr("Not now"), running_ ? tr("Close the game first.") : tr("Install Anvil first, then its plugins."));
        return;
    }
    std::string error;
    auto zip = ZipArchive::open(std::move(download.bytes), error);
    if (!zip) {
        show_message(tr("Not a plugin zip"), zip_file_name(*plugin) + ": " + error);
        return;
    }
    check_zip(std::move(*zip), zip_file_name(*plugin), &*plugin);
}

void ManagerApp::install_zip() {
    if (game_running(game_)) {
        running_ = true;
        show_message(tr("Nothing changed"), tr("EU4 is running from this folder. Close it first."));
        return;
    }
    const PluginInstallResult result = install_plugin_zip(*zip_, *zip_check_, game_);
    const std::string id = zip_check_->id;
    zip_.reset();
    zip_check_.reset();
    refresh();
    if (!result.ok) {
        failed(tr("The plugin was not installed"), result.error, result.access_denied);
        return;
    }
    show_message(tr("Done"), trf("{0} is installed in {1}. It loads when the game starts.",
                                 {id, "anvil/plugins/" + u8(result.folder.filename())}));
}

void ManagerApp::dropped(const fs::path& file) {
    if (_wcsicmp(file.extension().c_str(), L".zip") != 0) {
        show_message(tr("Not a zip"), tr("Drop a plugin's zip (<id>-<version>.zip) here to install it."));
        return;
    }
    open_zip(file);
}

void ManagerApp::edit_list(const std::string& key, const std::string& id, bool listed_now) {
    std::string error;
    if (!set_plugin_listed(game_ / "anvil" / "anvil.toml", key, id, listed_now, error)) {
        failed(tr("anvil.toml was not changed"), error, error.find("error 5)") != std::string::npos);
    }
    refresh();
}

void ManagerApp::show_message(std::string title, std::string text) {
    message_title_ = std::move(title);
    message_text_ = std::move(text);
    offer_elevation_ = false;
    open_message_ = true;
}

void ManagerApp::failed(const std::string& what, const std::string& error, bool access_denied) {
    show_message(what, error + (access_denied ? "\n\n" + std::string(tr("Windows refused access to the game folder. The "
                                                                        "Manager can start again as administrator and retry."))
                                              : std::string()));
    offer_elevation_ = access_denied;
}

void ManagerApp::draw_modals() {
    if (open_plan_) ImGui::OpenPopup("###plan");
    if (open_zip_) ImGui::OpenPopup("###zip");
    if (open_approve_) ImGui::OpenPopup("###approve");
    if (open_download_) ImGui::OpenPopup("###download");
    if (open_message_) ImGui::OpenPopup("###message");
    open_plan_ = open_zip_ = open_message_ = open_approve_ = open_download_ = false;
    draw_plan_modal();
    draw_zip_modal();
    draw_download_modal();
    draw_approve_modal();

    next_modal(36);
    if (ImGui::BeginPopupModal((message_title_ + "###message").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        wrapped(message_text_);
        ImGui::Spacing();
        if (offer_elevation_ && ImGui::Button(tr("Restart as administrator"))) {
            if (restart_elevated(game_)) close_ = true;
            ImGui::CloseCurrentPopup();
        }
        if (offer_elevation_) ImGui::SameLine();
        if (ImGui::Button(tr("OK"))) {
            ImGui::CloseCurrentPopup();
            if (close_after_message_) close_ = true;
        }
        ImGui::EndPopup();
    }
}

void ManagerApp::draw_plan_modal() {
    next_modal(40);
    const bool same = snapshot_->installation->installed() && snapshot_->installation->dll_version == package_->version;
    const std::string title = plan_ && plan_->uninstall ? std::string(tr("Uninstall Anvil"))
                              : same                    ? trf("Repair Anvil {0}", {package_->version})
                                                        : trf("Install Anvil {0}", {package_->version});
    if (!ImGui::BeginPopupModal((title + "###plan").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (!plan_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Plan& plan = plan_->plan;
    if (!plan.refused.empty()) colored_wrapped(kBad, plan.refused);
    for (const std::string& note : plan.notes) bullet(note);
    if (plan.steps.empty() && plan.refused.empty()) wrapped(tr("Nothing to change."));
    if (!plan.steps.empty() &&
        ImGui::TreeNode("changes", "%s", trf("{0} change(s) in {1}", {std::to_string(plan.steps.size()), u8(game_)}).c_str())) {
        for (const Step& step : plan.steps) ImGui::BulletText("%s", i18n::format(step_text(step.kind), {step.path}).c_str());
        ImGui::TreePop();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(plan.steps.empty() || !plan.refused.empty() || read_only());
    const Installation& inst = *snapshot_->installation;
    const bool repair = inst.installed() && inst.dll_version == package_->version;
    if (ImGui::Button(plan_->uninstall ? tr("Uninstall") : repair ? tr("Repair") : tr("Install"))) {
        ImGui::CloseCurrentPopup();
        run_plan();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        plan_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::draw_download_modal() {
    next_modal(40);
    const std::string title = download_ ? trf("Install {0}", {download_->name}) : std::string();
    if (!ImGui::BeginPopupModal((title + "###download").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (!download_ || download_pending_.valid()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const CatalogPlugin& plugin = *download_;
    wrapped(trf("Download {0} ({1} MB) from {2}?", {zip_file_name(plugin), megabytes(plugin.zip.size), zip_source(plugin)}));
    weak_wrapped(trf("It is installed only if its SHA-256 is {0}, the one in the list. Before that you see what it holds and "
                     "what anvil check finds.",
                     {plugin.zip.sha256}));
    ImGui::Spacing();
    if (ImGui::Button(tr("Download"))) {
        download_pending_ = std::async(std::launch::async, download_pinned_zip, plugin);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        download_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::draw_zip_modal() {
    next_modal(40);
    if (!ImGui::BeginPopupModal((std::string(tr("Install a plugin")) + "###zip").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (!zip_check_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const PluginZipCheck& check = *zip_check_;
    ImGui::TextUnformatted(zip_name_.c_str());
    ImGui::Separator();
    bool can_install = check.error.empty();
    if (!check.error.empty()) {
        colored_wrapped(kBad, trf("This zip cannot be installed: {0}", {check.error}));
    } else {
        const Manifest& m = *check.report.manifest;
        ImGui::Text("%s %s", m.name.empty() ? m.id.c_str() : m.name.c_str(), check.version.c_str());
        weak(m.authors.empty() ? trf("id {0}", {m.id}) : trf("id {0}, by {1}", {m.id, join(m.authors)}));
        if (!m.description.empty()) wrapped(m.description);
        if (!m.repository.empty() && m.repository.starts_with("https://")) {
            if (ImGui::SmallButton(tr("Open its repository"))) open_url(m.repository);
        }
        ImGui::Spacing();
        colored_wrapped(kWarn, tr("A plugin is native code: it runs inside the game with your rights, like any "
                                  "program. Install plugins only from authors you trust."));
        if (m.affects_gameplay) {
            bullet(tr("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there."));
        } else {
            bullet(tr("It says it does not change the game (affects_gameplay = false)."));
        }
        if (!check.installed_folder.empty()) {
            const std::string folder = "anvil/plugins/" + check.installed_folder;
            bullet(check.installed_version.empty()
                       ? trf("It replaces the installed copy in {0}. Its settings and data stay.", {folder})
                       : trf("It replaces version {0} in {1}. Its settings and data stay.", {check.installed_version, folder}));
        }
        if (!check.ignored.empty()) weak_wrapped(trf("Not installed (outside the plugin's folder in the zip): {0}", {join(check.ignored)}));
        for (const cli::Problem& problem : shown_problems(check.report)) {
            colored_wrapped(kWarn, problem.file + (problem.line > 0 ? ":" + std::to_string(problem.line) : std::string()) +
                                       ": " + problem.message);
        }
        if (check.would_skip) {
            colored_wrapped(kBad, trf("Anvil would not load it with this eu4.exe: {0}", {i18n::text(check.report.skip_why)}));
            ImGui::Checkbox(tr("Install it anyway"), &zip_anyway_);
            can_install = zip_anyway_;
        } else if (check.report.problems.empty()) {
            colored_wrapped(kGood, tr("anvil check found no problems with this eu4.exe."));
        }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!can_install || read_only());
    if (ImGui::Button(tr("Install"))) {
        ImGui::CloseCurrentPopup();
        install_zip();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        zip_.reset();
        zip_check_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace anvil::manager
