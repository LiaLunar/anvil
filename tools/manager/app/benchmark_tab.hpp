#pragma once

#include "benchmark_report.hpp"
#include "benchmark_run.hpp"
#include "benchmark_setup.hpp"
#include "cli/game.hpp"
#include "json.hpp"
#include "platform.hpp"

#include <windows.h>

#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anvil::manager {

// What the Benchmark tab needs of the Manager's state.
struct BenchmarkContext {
    std::filesystem::path game;
    bool gog = false;                         // a GOG install, which starts without Steam
    const cli::GameReport* report = nullptr;  // nullptr: Anvil is not installed
    std::string installed_anvil;              // the installed version.dll's version; "" when it is not Anvil's
    std::string exe_build;                    // a build Anvil knows, or ""
    std::string exe_sha256;
};

// The Benchmark tab (docs/spec/benchmark-v1.md §8, anvil#133 variant A): four
// screens in one column that replace each other: setup, running, result and
// the preview of the report.
class BenchmarkTab {
public:
    BenchmarkTab(HWND window, ManagerSettings& settings);

    void draw(const BenchmarkContext& context);
    // A series runs or its report is being made: the other tabs, Play EU4 and
    // Change... are locked until it ends.
    bool busy() const { return runner_ != nullptr || prepared_.valid() || sending_.valid(); }
    // The game folder changed: back to the setup screen.
    void reset();

private:
    enum class Screen { Setup, Running, Result, Preview };

    // What the report needs besides the rows, read when the series ends.
    struct Prepared {
        ReportSystem system;
        ReportMods mods;
    };

    void draw_setup(const BenchmarkContext& context);
    void draw_kept(const BenchmarkContext& context);
    void draw_running();
    void draw_result();
    void draw_preview();
    void draw_send();

    void check(const BenchmarkContext& context);
    void start(const BenchmarkContext& context);
    void poll(const BenchmarkContext& context);
    void show_report(Json report, std::filesystem::path file);
    void switch_workshop_ids(bool include);

    // The words for an arm: "without bellows" / "with bellows".
    std::string arm_words(const std::string& arm) const;

    HWND window_;
    ManagerSettings& settings_;
    Screen screen_ = Screen::Setup;
    std::filesystem::path game_;

    // Setup.
    std::string plugin_;
    int target_ = kDefaultTarget;
    std::vector<SetupCheck> checks_;
    const cli::GameReport* checked_report_ = nullptr;  // the report checks_ were made from
    std::string checked_plugin_;
    std::optional<std::vector<KeptReport>> kept_;

    // Running.
    std::unique_ptr<SeriesRunner> runner_;
    RunProgress progress_;
    ReportInput input_;  // without the rows; filled when the series starts
    std::filesystem::path user_dir_;
    std::future<Prepared> prepared_;
    std::optional<Series> series_;
    std::string series_note_;  // why a series ended without a report, or a detail of its stop

    // Result and preview: one report, from this series or a kept one.
    std::optional<Json> report_;
    std::string report_text_;
    std::filesystem::path report_file_;
    std::string keep_error_;
    bool from_series_ = false;  // the report of the series that just ran (Result goes back to it)
    std::optional<std::vector<std::string>> workshop_ids_;  // known while they are in the report
    bool include_ids_ = true;
    std::map<std::string, std::map<std::string, std::string>> labels_;  // plugin -> field -> label, this series only
    std::map<std::string, int> fields_start_;  // plugin -> the start whose fields the preview shows
    std::string copy_outcome_;
    bool sent_ = false;  // a .sent marker, or a Send accepted in this session
    std::future<SendOutcome> sending_;
    std::string send_outcome_;
    bool send_failed_ = false;
};

}  // namespace anvil::manager
