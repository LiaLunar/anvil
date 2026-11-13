#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace anvil {
namespace {

struct LogState {
    std::mutex mutex;
    std::vector<std::string> pending;  // lines written before open()
    std::vector<std::string> warnings;  // "[source] message", at most Log::kMaxWarnings
    HANDLE file = INVALID_HANDLE_VALUE;
    // open() could not open the file: the game's own restart starts the new
    // process before the old one has closed its log. Writes try again until then.
    std::filesystem::path path;
    ULONGLONG retry_until = 0;
    ULONGLONG next_try = 0;
    ULONGLONG first_try = 0;
    AnvilLogLevel minimum = ANVIL_LOG_INFO;
    LARGE_INTEGER start{};
    LARGE_INTEGER frequency{};

    LogState() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
};

LogState& state() {
    static LogState s;
    return s;
}

const char* level_name(AnvilLogLevel level) {
    switch (level) {
    case ANVIL_LOG_DEBUG:
        return "debug";
    case ANVIL_LOG_INFO:
        return "info ";
    case ANVIL_LOG_WARNING:
        return "WARN ";
    case ANVIL_LOG_ERROR:
        return "ERROR";
    }
    return "?    ";
}

void write_raw(HANDLE file, const std::string& line) {
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

constexpr ULONGLONG kRetryMs = 60'000;      // how long a log in use is waited for
constexpr ULONGLONG kRetryEveryMs = 250;

// Under the log's lock: keeps the last log as anvil.prev.log, opens a new one
// and writes what waited for it.
bool try_open(LogState& s) {
    std::filesystem::path previous = s.path;
    previous.replace_extension(L".prev.log");
    MoveFileExW(s.path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    s.file = CreateFileW(s.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        return false;
    }
    static const char bom[] = "\xEF\xBB\xBF";
    write_raw(s.file, bom);
    for (const auto& line : s.pending) {
        write_raw(s.file, line);
    }
    s.pending.clear();
    s.pending.shrink_to_fit();
    return true;
}

}  // namespace

Log& Log::instance() {
    static Log log;
    return log;
}

void Log::open(const std::filesystem::path& file) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file != INVALID_HANDLE_VALUE) {
        return;
    }
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    s.path = file;
    if (try_open(s) || GetLastError() != ERROR_SHARING_VIOLATION) {
        return;
    }
    // Another process still writes it: the one the game's own restart replaces.
    s.first_try = GetTickCount64();
    s.retry_until = s.first_try + kRetryMs;
    s.next_try = s.first_try + kRetryEveryMs;
}

namespace {

std::string format_line(AnvilLogLevel level, std::string_view source, std::string_view message) {
    LogState& s = state();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double ms = static_cast<double>(now.QuadPart - s.start.QuadPart) * 1000.0 /
                      static_cast<double>(s.frequency.QuadPart ? s.frequency.QuadPart : 1);
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "[%9.1f ms] %s ", ms, level_name(level));
    std::string line = prefix;
    line += '[';
    line.append(source.data(), source.size());
    line += "] ";
    line.append(message.data(), message.size());
    line += "\r\n";
    return line;
}

// Under the log's lock.
void keep_warning(LogState& s, AnvilLogLevel level, std::string_view source, std::string_view message) {
    if (level < ANVIL_LOG_WARNING || s.warnings.size() >= Log::kMaxWarnings) return;
    std::string line = "[";
    line.append(source.data(), source.size());
    line += "] ";
    line.append(message.data(), message.size());
    s.warnings.push_back(std::move(line));
}

// Under the log's lock.
void append_line(LogState& s, std::string line) {
    if (s.file == INVALID_HANDLE_VALUE && s.retry_until != 0) {
        const ULONGLONG now = GetTickCount64();
        if (now >= s.next_try) {
            s.next_try = now + kRetryEveryMs;
            if (try_open(s)) {
                write_raw(s.file, format_line(ANVIL_LOG_INFO, "anvil",
                                              "anvil.log was still in use by the process this one replaced "
                                              "(the game's own restart); opened " +
                                                  std::to_string(now - s.first_try) + " ms later"));
                s.retry_until = 0;
            } else if (now >= s.retry_until) {
                s.retry_until = 0;
            }
        }
    }
    if (s.file == INVALID_HANDLE_VALUE) {
        if (s.pending.size() < 4096) {
            s.pending.push_back(std::move(line));
        }
        return;
    }
    write_raw(s.file, line);
}

}  // namespace

void Log::write(AnvilLogLevel level, std::string_view source, std::string_view message) {
    LogState& s = state();
    if (level < s.minimum) {
        if (level >= ANVIL_LOG_WARNING) {
            std::lock_guard lock(s.mutex);
            keep_warning(s, level, source, message);
        }
        return;
    }
    std::string line = format_line(level, source, message);
    std::lock_guard lock(s.mutex);
    keep_warning(s, level, source, message);
    append_line(s, std::move(line));
}

bool Log::try_write(AnvilLogLevel level, std::string_view source, std::string_view message) {
    LogState& s = state();
    if (level < s.minimum) {
        return true;
    }
    std::string line = format_line(level, source, message);
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    keep_warning(s, level, source, message);
    append_line(s, std::move(line));
    return true;
}

void Log::set_min_level(AnvilLogLevel level) {
    state().minimum = level;
}

bool Log::is_open() const {
    return state().file != INVALID_HANDLE_VALUE;
}

std::vector<std::string> Log::warnings() const {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    return s.warnings;
}

size_t Log::warning_count() const {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    return s.warnings.size();
}

void log_info(std::string_view message) { Log::instance().write(ANVIL_LOG_INFO, "anvil", message); }
void log_warning(std::string_view message) { Log::instance().write(ANVIL_LOG_WARNING, "anvil", message); }
void log_error(std::string_view message) { Log::instance().write(ANVIL_LOG_ERROR, "anvil", message); }
void log_debug(std::string_view message) { Log::instance().write(ANVIL_LOG_DEBUG, "anvil", message); }

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                          nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr,
                            nullptr);
    }
    return out;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(chars > 0 ? chars : 0), L'\0');
    if (chars > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    }
    return out;
}

}  // namespace anvil
