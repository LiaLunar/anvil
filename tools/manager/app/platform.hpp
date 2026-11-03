#pragma once

#include "benchmark_send.hpp"
#include "catalog.hpp"
#include "install.hpp"
#include "release_check.hpp"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::manager {

std::filesystem::path own_exe_path();

// The Anvil package linked into AnvilManager.exe (resource ANVIL_PACKAGE).
std::optional<Package> load_embedded_package(std::string& error);

// The Manager's own settings, per Windows user: %LOCALAPPDATA%\Anvil\manager.toml.
struct ManagerSettings {
    std::filesystem::path game_folder;  // the folder the player chose last
    bool check_releases = false;        // opt-in (design §15)
    bool asked_releases = false;        // the player answered the question once
    // The arm the last start-up benchmark series began with ("with" or
    // "without"; "" before the first): the next one begins with the other.
    std::string benchmark_first_arm;
};
ManagerSettings load_manager_settings();
bool save_manager_settings(const ManagerSettings& settings, std::string& error);

// Windows' own pickers; nullopt when the player cancels.
std::optional<std::filesystem::path> pick_folder(HWND owner, const std::filesystem::path& start);
std::optional<std::filesystem::path> pick_zip(HWND owner);

// Opens an https:// link in the browser; anything else is ignored.
void open_url(std::string_view url);
void open_folder(const std::filesystem::path& folder);
bool copy_to_clipboard(HWND owner, std::string_view utf8);

// steam://rungameid/236850 (design §15).
bool launch_game();

// Starts this exe again as administrator for `game_dir` (the player answers
// Windows' prompt); false when that did not happen.
bool restart_elevated(const std::filesystem::path& game_dir);

// After an uninstall that ran from <EU4>\AnvilManager.exe: a copy of the
// Manager in the temporary folder waits for this process to end, then
// removes `exe`. The copy is removed at the next start of a Manager.
bool remove_after_exit(const std::filesystem::path& exe);
// That copy's work: `--remove-after-exit <exe> <pid>`. Only a file named
// AnvilManager.exe next to an eu4.exe is removed.
int run_remove_after_exit(const std::filesystem::path& exe, DWORD pid);
void remove_stale_copies();

// The opt-in release check: GET the latest release from the GitHub API.
// Runs on a worker thread; nothing is downloaded but this reply.
struct ReleaseCheck {
    std::optional<Release> release;  // set when the reply was a release
    std::string error;
};
ReleaseCheck fetch_latest_release();

// The catalog of this build (resource ANVIL_CATALOG, ADR 0002).
std::optional<Catalog> load_embedded_catalog(std::string& error);

// "Get the current list": GET catalog/plugins.json of main from
// raw.githubusercontent.com, on the player's click. Runs on a worker thread.
struct CatalogFetch {
    std::optional<Catalog> catalog;  // set when the reply validated
    std::string error;
};
CatalogFetch fetch_catalog();

// "Install": GET the pinned zip, on the player's confirmation. `bytes` is
// set only when the size and SHA-256 equal the pin. Runs on a worker thread.
struct ZipDownload {
    std::vector<uint8_t> bytes;
    std::string error;
};
ZipDownload download_pinned_zip(const CatalogPlugin& plugin);

// Send: POST the report to https://bench.happydwarf.dev/reports as
// application/json, once. Runs on a worker thread.
SendOutcome post_report(std::string body);

}  // namespace anvil::manager
