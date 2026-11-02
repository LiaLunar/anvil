#include "safe_mode.hpp"

#include <windows.h>

#include <array>
#include <cstdio>
#include <string>

namespace anvil {
namespace {

// Win32 only: clean_exit runs from DllMain at process exit.
unsigned read_count(const std::wstring& file) {
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char text[16] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(h, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(h);
    if (!ok) return 0;
    unsigned long value = 0;
    for (DWORD i = 0; i < read && text[i] >= '0' && text[i] <= '9'; ++i) value = value * 10 + (text[i] - '0');
    return value > 1000 ? 1000 : static_cast<unsigned>(value);
}

void write_count(const std::wstring& file, unsigned count) {
    if (count == 0) {
        DeleteFileW(file.c_str());
        return;
    }
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char text[16];
    const int n = std::snprintf(text, sizeof(text), "%u\n", count);
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(h);
}

constexpr std::array<std::string_view, 2> kSafeModePlugins{"anvil-game", "anvil-overlay"};

}  // namespace

bool loads_in_safe_mode(std::string_view id) {
    for (const std::string_view standard : kSafeModePlugins) {
        if (id == standard) return true;
    }
    return false;
}

i18n::Message safe_mode_exclusion(std::string_view id, const std::vector<CrashSummary>& crashes) {
    for (const CrashSummary& crash : crashes) {
        if (crash_in_plugin(crash, id)) {
            const std::string& summary = crash.summary.empty() ? crash.level : crash.summary;
            return i18n::Message("safe_mode_crash", {i18n::crash_message(crash.level, crash.plugins, summary), crash.name});
        }
    }
    return {};
}

std::vector<CrashSummary> safe_mode_crashes(const std::filesystem::path& crash_dir, unsigned unfinished) {
    std::vector<CrashSummary> crashes;
    for (const std::string& name : crash_report_names(crash_dir)) {
        if (crashes.size() >= unfinished) break;
        if (auto crash = load_crash_summary(crash_dir, name); crash && crash->before_game_ready) {
            crashes.push_back(std::move(*crash));
        }
    }
    return crashes;
}

StartRecord StartMarker::begin(const std::filesystem::path& file) {
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    file_ = file.wstring();
    StartRecord record;
    record.unfinished = read_count(file_);
    record.safe_mode = record.unfinished >= kSafeModeAfter;
    previous_ = record.safe_mode ? 0 : record.unfinished;
    write_count(file_, record.safe_mode ? 0 : record.unfinished + 1);
    active_ = !record.safe_mode;
    return record;
}

void StartMarker::game_ready(bool crash_reported) {
    if (!active_) return;
    active_ = false;
    if (!crash_reported) write_count(file_, 0);
}

void StartMarker::clean_exit() {
    if (!active_) return;
    active_ = false;
    write_count(file_, previous_);
}

}  // namespace anvil
