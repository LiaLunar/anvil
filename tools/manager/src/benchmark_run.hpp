#pragma once

#include "benchmark_series.hpp"
#include "benchmark_setup.hpp"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace anvil::manager {

// Runs a start-up benchmark series (docs/spec/benchmark-v1.md §2, §3) on a
// thread of its own: starts eu4.exe from the game folder with the arm in its
// environment, reads the core's start record, falls back to WM_CLOSE and a
// kill, pauses between starts, and feeds every start into a Series. It never
// changes the game's priority or affinity and never writes anvil.toml.

// The record the core writes for a benchmark start (§6).
std::filesystem::path start_record_file(const std::filesystem::path& game_dir);

// The child's environment: this process's, with SteamAppId=236850 (the
// player's usual start with the Steam client) and ANVIL_BENCHMARK=<arm>:<id>.
// A Windows environment block: sorted, each entry NUL-terminated, then a NUL.
std::wstring benchmark_environment(const std::wstring& current, Arm arm, const std::string& plugin, bool steam = true);

struct RunnerConfig {
    std::filesystem::path game_dir;
    std::filesystem::path exe;    // game_dir/eu4.exe
    std::wstring arguments;       // after the exe on the command line; none for the game
    std::string plugin;           // the compared plugin's id
    bool steam = true;            // false: a GOG install, started without SteamAppId
    int target = kDefaultTarget;
    Arm first = Arm::Without;
    // Read when the series starts, then before and after every start.
    std::function<SeriesEnvironment()> environment;

    std::chrono::milliseconds ready_timeout{std::chrono::seconds(600)};  // no ready: killed (timeout)
    std::chrono::milliseconds close_after{std::chrono::seconds(45)};     // after ready: WM_CLOSE
    std::chrono::milliseconds kill_after{std::chrono::seconds(15)};      // after WM_CLOSE: killed
    std::chrono::milliseconds pause{std::chrono::seconds(5)};            // between starts
    std::chrono::milliseconds poll{250};
};

enum class RunPhase { Starting, Loading, Waiting, Quitting, Pause, Done };

struct RunProgress {
    RunPhase phase = RunPhase::Starting;
    int start = 0;  // 1-based, the start that runs or ran last
    Arm arm = Arm::Without;
    double seconds = 0;  // in this phase (Loading: since the process was created)
    std::vector<std::string> waiting_for;  // Waiting: participants not done yet
    bool stopping = false;  // the player asked to stop
    std::string environment_change;  // why the series stopped with environment_changed
    std::string start_error;         // eu4.exe could not be started: the series ended without a row
    Series series{"", kDefaultTarget, Arm::Without};
};

class SeriesRunner {
public:
    explicit SeriesRunner(RunnerConfig config);
    // A runner that is still going is abandoned: the running game still
    // quits by itself, and the series is lost (§2).
    ~SeriesRunner();
    SeriesRunner(const SeriesRunner&) = delete;
    SeriesRunner& operator=(const SeriesRunner&) = delete;

    // The running start finishes cleanly (ready, done(), quit), then the
    // series ends as stopped by the player.
    void stop();
    bool done() const;
    RunProgress progress() const;

private:
    void run();
    // One start of `arm`: what the Manager saw of it.
    StartObservation run_start(Arm arm, const SeriesEnvironment& before);
    void set_phase(RunPhase phase, std::chrono::steady_clock::time_point since);

    RunnerConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    RunProgress progress_;  // under mutex_
    std::chrono::steady_clock::time_point phase_since_;  // under mutex_
    bool stop_ = false;      // under mutex_
    bool abandon_ = false;   // under mutex_
    std::thread thread_;
};

}  // namespace anvil::manager
