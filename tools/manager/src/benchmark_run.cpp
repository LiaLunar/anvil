#include "benchmark_run.hpp"

#include "core/crash_history.hpp"
#include "core/log.hpp"
#include "i18n/translate.hpp"

#include <algorithm>
#include <set>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

uint64_t filetime(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// The record of this process, once the core wrote it; nullopt before.
std::optional<StartRecord> read_record(const fs::path& file, DWORD pid, uint64_t created) {
    const auto text = read_text_file(file, 4 << 20);
    if (!text) return std::nullopt;
    std::string error;
    auto record = parse_start_record(*text, error);
    if (!record || record->process_id != pid || record->creation_filetime != created) return std::nullopt;
    return record;
}

struct CloseTarget {
    DWORD pid;
    int posted;
};

BOOL CALLBACK post_close(HWND window, LPARAM param) {
    auto* target = reinterpret_cast<CloseTarget*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == target->pid && GetWindow(window, GW_OWNER) == nullptr && PostMessageW(window, WM_CLOSE, 0, 0)) ++target->posted;
    return TRUE;
}

std::vector<std::string> not_done(const StartRecord& record) {
    std::vector<std::string> ids;
    for (const RecordPlugin& p : record.plugins) {
        if (p.state == "loaded" && p.participant && !p.done) ids.push_back(p.id);
    }
    return ids;
}

}  // namespace

fs::path start_record_file(const fs::path& game_dir) { return game_dir / "anvil" / "data" / "anvil" / "benchmark-start.json"; }

std::wstring benchmark_environment(const std::wstring& current, Arm arm, const std::string& plugin) {
    std::vector<std::wstring> entries;
    for (size_t at = 0; at < current.size();) {
        const size_t end = current.find(L'\0', at);
        const std::wstring entry = current.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        if (entry.empty()) break;
        at = end == std::wstring::npos ? current.size() : end + 1;
        const size_t eq = entry.find(L'=', 1);  // "=C:=C:\..." entries start with '='
        const std::wstring name = entry.substr(0, eq);
        if (_wcsicmp(name.c_str(), L"SteamAppId") == 0 || _wcsicmp(name.c_str(), L"ANVIL_BENCHMARK") == 0) continue;
        entries.push_back(entry);
    }
    entries.push_back(L"SteamAppId=236850");
    entries.push_back(L"ANVIL_BENCHMARK=" + widen(std::string(arm_name(arm)) + ":" + plugin));
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.substr(0, a.find(L'=', 1)).c_str(), b.substr(0, b.find(L'=', 1)).c_str()) < 0;
    });
    std::wstring block;
    for (const std::wstring& e : entries) {
        block += e;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

SeriesRunner::SeriesRunner(RunnerConfig config) : config_(std::move(config)) {
    progress_.series = Series(config_.plugin, config_.target, config_.first);
    phase_since_ = Clock::now();
    thread_ = std::thread([this] { run(); });
}

SeriesRunner::~SeriesRunner() {
    {
        std::lock_guard lock(mutex_);
        abandon_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SeriesRunner::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        progress_.stopping = true;
    }
    wake_.notify_all();
}

bool SeriesRunner::done() const {
    std::lock_guard lock(mutex_);
    return progress_.phase == RunPhase::Done;
}

RunProgress SeriesRunner::progress() const {
    std::lock_guard lock(mutex_);
    RunProgress p = progress_;
    p.seconds = std::chrono::duration<double>(Clock::now() - phase_since_).count();
    return p;
}

void SeriesRunner::set_phase(RunPhase phase, Clock::time_point since) {
    std::lock_guard lock(mutex_);
    progress_.phase = phase;
    phase_since_ = since;
    if (phase != RunPhase::Waiting) progress_.waiting_for.clear();
}

void SeriesRunner::run() {
    const SeriesEnvironment initial = config_.environment ? config_.environment() : SeriesEnvironment{};
    while (true) {
        std::optional<Arm> arm;
        {
            std::lock_guard lock(mutex_);
            if (abandon_) return;
            if (stop_) progress_.series.stop("player");
            arm = progress_.series.next();
            if (arm) {
                progress_.start = static_cast<int>(progress_.series.rows().size()) + 1;
                progress_.arm = *arm;
            }
        }
        if (!arm) break;
        const SeriesEnvironment before = config_.environment ? config_.environment() : SeriesEnvironment{};
        if (const std::string change = environment_change(initial, before); !change.empty()) {
            std::lock_guard lock(mutex_);
            progress_.environment_change = change;
            progress_.series.stop("environment_changed");
            break;
        }
        StartObservation seen = run_start(*arm, initial);
        {
            std::lock_guard lock(mutex_);
            if (abandon_) return;
            if (!progress_.start_error.empty()) {
                // eu4.exe is not what it was when the series started.
                progress_.environment_change = progress_.start_error;
                progress_.series.stop("environment_changed");
                break;
            }
            if (seen.environment_changed && progress_.environment_change.empty()) {
                progress_.environment_change = i18n::tr("something changed during the start");
            }
            progress_.series.add(seen);
            if (progress_.series.over()) break;
        }
        set_phase(RunPhase::Pause, Clock::now());
        // A stop during the pause ends the series at once: no start runs.
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, config_.pause, [&] { return abandon_ || stop_; });
        if (abandon_) return;
    }
    set_phase(RunPhase::Done, Clock::now());
}

StartObservation SeriesRunner::run_start(Arm arm, const SeriesEnvironment& initial) {
    StartObservation seen;
    set_phase(RunPhase::Starting, Clock::now());
    const fs::path crash_dir = config_.game_dir / "anvil" / "crashes";
    const std::vector<std::string> crashes_before = crash_report_names(crash_dir);
    const fs::path record_file = start_record_file(config_.game_dir);

    const wchar_t* current = GetEnvironmentStringsW();
    std::wstring env_text;
    if (current != nullptr) {
        const wchar_t* end = current;
        while (*end != L'\0') end += wcslen(end) + 1;
        env_text.assign(current, end);
        FreeEnvironmentStringsW(const_cast<wchar_t*>(current));
    }
    std::wstring environment = benchmark_environment(env_text, arm, config_.plugin);
    std::wstring command = L"\"" + config_.exe.wstring() + L"\"";
    if (!config_.arguments.empty()) command += L" " + config_.arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const std::wstring dir = config_.game_dir.wstring();
    if (!CreateProcessW(config_.exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), dir.c_str(), &startup, &info)) {
        std::lock_guard lock(mutex_);
        progress_.start_error = i18n::trf("eu4.exe could not be started (Windows error {0})", {std::to_string(GetLastError())});
        return seen;
    }
    CloseHandle(info.hThread);
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(info.hProcess, &created, &exited, &kernel, &user);
    const uint64_t created_at = filetime(created);
    const Clock::time_point started = Clock::now();
    set_phase(RunPhase::Loading, started);

    std::optional<Clock::time_point> ready_at, closed_at;
    bool quitting = false;
    while (WaitForSingleObject(info.hProcess, static_cast<DWORD>(config_.poll.count())) == WAIT_TIMEOUT) {
        {
            std::lock_guard lock(mutex_);
            if (abandon_) {
                CloseHandle(info.hProcess);  // the game still quits by itself
                return seen;
            }
        }
        const Clock::time_point now = Clock::now();
        if (!quitting) {
            if (auto record = read_record(record_file, info.dwProcessId, created_at)) {
                if (!ready_at) {
                    ready_at = now;
                    set_phase(RunPhase::Waiting, now);
                }
                if (record->write == "end") {
                    quitting = true;
                    set_phase(RunPhase::Quitting, now);
                } else {
                    std::lock_guard lock(mutex_);
                    progress_.waiting_for = not_done(*record);
                }
            }
        }
        if (!ready_at && now - started > config_.ready_timeout) {
            TerminateProcess(info.hProcess, 1);
            seen.timed_out = true;
            break;
        }
        // The game takes WM_CLOSE only after ready (SDL turns it into SDL_QUIT).
        if (ready_at && !closed_at && now - *ready_at > config_.close_after) {
            CloseTarget target{info.dwProcessId, 0};
            EnumWindows(post_close, reinterpret_cast<LPARAM>(&target));
            closed_at = now;
            set_phase(RunPhase::Quitting, now);
        }
        if (closed_at && now - *closed_at > config_.kill_after) {
            TerminateProcess(info.hProcess, 1);
            seen.killed = true;
            break;
        }
    }
    WaitForSingleObject(info.hProcess, 10000);
    CloseHandle(info.hProcess);

    seen.record = read_record(record_file, info.dwProcessId, created_at);
    // A crash report this start wrote: the newest one that was not there before.
    const std::set<std::string> before(crashes_before.begin(), crashes_before.end());
    for (const std::string& name : crash_report_names(crash_dir)) {
        if (before.count(name) != 0) continue;
        if (const auto crash = load_crash_summary(crash_dir, name)) {
            seen.crash = StartCrash{crash->level, crash->plugins.empty() ? "" : crash->plugins.front(),
                                    crash->before_game_ready || !seen.record};
        }
        break;
    }
    if (config_.environment) {
        const SeriesEnvironment after = config_.environment();
        if (!environment_change(initial, after).empty()) {
            seen.environment_changed = true;
            std::lock_guard lock(mutex_);
            progress_.environment_change = environment_change(initial, after);
        }
    }
    return seen;
}

}  // namespace anvil::manager
