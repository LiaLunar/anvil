#include "release_check.hpp"

#include "core/json_scan.hpp"
#include "core/semver.hpp"
#include "https.hpp"
#include "i18n/translate.hpp"

#include <windows.h>
#include <shlobj.h>

namespace anvil::net {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

}  // namespace

std::optional<Release> parse_release(std::string_view json) {
    JsonScanner scan(json);
    if (!scan.eat('{')) return std::nullopt;
    Release release;
    if (!scan.peek('}')) {
        do {
            const auto key = scan.string();
            if (!key || !scan.eat(':')) return std::nullopt;
            if ((*key == "tag_name" || *key == "html_url") && scan.peek('"')) {
                const auto value = scan.string();
                if (!value) return std::nullopt;
                (*key == "tag_name" ? release.tag : release.url) = *value;
            } else if (!scan.skip_value()) {
                return std::nullopt;
            }
        } while (scan.eat(','));
    }
    if (!scan.eat('}') || !scan.at_end()) return std::nullopt;
    if (release.tag.empty() || !release.url.starts_with(std::string(kReleasesUrl) + "/")) return std::nullopt;
    for (const char c : release.url) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`') {
            return std::nullopt;
        }
    }
    return release;
}

bool newer_release(std::string_view tag, std::string_view current) {
    if (!tag.empty() && tag.front() == 'v') tag.remove_prefix(1);
    const auto latest = Version::parse(tag);
    const auto own = Version::parse(current);
    return latest && own && *own < *latest;
}

ReleaseCheck fetch_latest_release(const std::wstring& agent) {
    ReleaseCheck out;
    const auto body = https_get(kReleaseApiHost, kReleaseApiPath,
                                L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", 1u << 20,
                                "GitHub", agent, out.error, &out.english_error);
    if (!body) return out;
    out.release = parse_release(*body);
    if (!out.release) {
        out.english_error = i18n::N_("GitHub's reply is not a release of Anvil");
        out.error = i18n::tr(out.english_error.c_str());
    }
    return out;
}

std::filesystem::path manager_settings_file() {
    PWSTR local = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        out = std::filesystem::path(local) / "Anvil" / "manager.toml";
    }
    CoTaskMemFree(local);
    return out;
}

std::optional<bool> manager_release_answer(std::string_view text) {
    std::optional<bool> asked;
    std::optional<bool> check;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = trim(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (!line.empty() && line.front() == '[') break;  // a table: past the top-level keys
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        const std::optional<bool> flag = value == "true" ? std::optional(true) : value == "false" ? std::optional(false) : std::nullopt;
        if (key == "asked_releases") asked = flag;
        if (key == "check_releases") check = flag;
    }
    if (!asked.value_or(false) || !check) return std::nullopt;
    return check;
}

}  // namespace anvil::net
