#pragma once

#include "benchmark.hpp"
#include "outcome.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil {

class PluginSet;

// ---- labels of a cold start (docs/spec/benchmark-v1.md §3) ----------------------

// What every start leaves in anvil/data/anvil/last-start.txt for the next
// one. Times are UTC FILETIMEs (100 ns since 1601).
struct StartHistory {
    uint64_t boot_filetime = 0;   // when the computer started: now - GetTickCount64
    uint64_t tick_ms = 0;         // GetTickCount64 at the entry point
    uint64_t entry_filetime = 0;  // the entry point
};

StartHistory current_start_history(uint64_t entry_filetime, uint64_t tick_ms);
std::optional<StartHistory> read_start_history(const std::filesystem::path& file);
std::string start_history_text(const StartHistory& history);

// The computer started again since `previous`: the tick count went back, or
// the boot time moved by more than two minutes (more than the clock drifts
// between two starts).
bool rebooted_since(const StartHistory& previous, const StartHistory& now);

// The time of the newest Microsoft Defender "definitions updated" event
// (2000 in Microsoft-Windows-Windows Defender/Operational, readable without
// elevation); 0 when there is none, nullopt when the log cannot be read.
std::optional<uint64_t> newest_definitions_update();

// ---- the start record (docs/spec/benchmark-v1.md §6) ------------------------------

struct BenchmarkRecordPlugin {
    std::string id;
    std::string version;      // "" when skipped or disabled
    std::optional<Outcome::Kind> outcome;  // none yet: "pending"
    bool disabled_by_arm = false;  // disabled: by the arm, not by the player
    std::string skip_reason;  // skipped: skip_kind_name()
    std::optional<BenchmarkParticipant> participant;
};

struct BenchmarkRecord {
    const char* write = "ready";  // "ready" | "end" (the quit is decided)
    std::string anvil_version;
    BenchmarkArm arm;
    uint32_t process_id = 0;
    // QueryPerformanceCounter at AnvilEntry and at anvil-game.ready (0 before
    // it); the precise system time at AnvilEntry and the process's creation
    // time (FILETIMEs).
    int64_t qpc_frequency = 0;
    int64_t entry_qpc = 0;
    int64_t ready_qpc = 0;
    uint64_t entry_filetime = 0;
    uint64_t creation_filetime = 0;
    std::vector<BenchmarkRecordPlugin> plugins;  // resolved in load order, then skipped
    std::optional<BenchmarkOpenCost> game_first_opens;  // until ready
    std::optional<bool> first_start_since_boot;  // nullopt: no earlier start to compare with
    std::optional<bool> definitions_changed;     // since the previous start; nullopt: unknown
    std::optional<bool> input_during_start;      // keyboard or mouse between the entry point and ready
};

// The record's plugins: every plugin of the set, in its order (the ones to
// load in load order, then the skipped and disabled ones), with the fields of
// the participants that declared them joined to their plugins.
std::vector<BenchmarkRecordPlugin> record_plugins(const PluginSet& plugins, std::vector<BenchmarkParticipant> participants);

// The record as UTF-8 JSON, one document. Times to ready are also given in
// ms (creation -> entry, entry -> ready, creation -> ready).
std::string benchmark_record_json(const BenchmarkRecord& record);

}  // namespace anvil
