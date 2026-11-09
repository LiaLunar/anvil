#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::net {

// The opt-in release check of the Manager and the overlay (design §15, §19,
// ADR 0005): one GET of the latest release from the GitHub API and, when it is
// newer, a link to its page. Nothing is downloaded but this reply; updating
// Anvil means running a newer Manager.
inline constexpr const wchar_t* kReleaseApiHost = L"api.github.com";
inline constexpr const wchar_t* kReleaseApiPath = L"/repos/happy-dwarf-team/anvil/releases/latest";
inline constexpr const char* kReleasesUrl = "https://github.com/happy-dwarf-team/anvil/releases";

struct Release {
    std::string tag;  // "v1.2.0"
    std::string url;  // its page on GitHub
};

// tag_name and html_url of a GitHub release object (top-level keys only).
// nullopt when the text is not such an object, or the URL is not a page of
// the anvil repository on github.com (only those are opened).
std::optional<Release> parse_release(std::string_view json);

// True when `tag` ("v1.2.0" or "1.2.0") is a newer version than `current`.
// A tag that is not a version is never newer.
bool newer_release(std::string_view tag, std::string_view current);

// The GET itself, sent as `agent`. Blocks: call it on a worker thread.
struct ReleaseCheck {
    std::optional<Release> release;  // set when the reply was a release
    std::string error;               // in the player's language
    std::string english_error;       // the same in English, for anvil.log
};
ReleaseCheck fetch_latest_release(const std::wstring& agent);

// The Manager's settings, per Windows user: %LOCALAPPDATA%\Anvil\manager.toml
// ("" when Windows names no such folder).
std::filesystem::path manager_settings_file();

// The player's answer to the Manager's question in the text of that file:
// check_releases once asked_releases is true, nullopt before the player
// answered or when the file says neither. Reads only `key = true|false` lines
// at the top level, as the Manager writes them, so the overlay needs no TOML
// parser.
std::optional<bool> manager_release_answer(std::string_view manager_toml);

}  // namespace anvil::net
