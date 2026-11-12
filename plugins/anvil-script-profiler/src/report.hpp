// A finished profiling span as rankings (docs/script-profiler.md): the root
// blocks of the snapshot merged by owner and role, one list of triggers and
// one of effects and one of scripted trigger calls (v1.1), and the CSV and panel rows made from them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "owners.hpp"
#include "recorder.hpp"
#include "texts.hpp"

namespace script_profiler {

struct Row {
    Kind list = Kind::trigger;
    OwnerKind kind = OwnerKind::none;
    const void* object = nullptr;
    std::string name;
    int option = 0;
    std::string role;
    std::uint32_t caller = 0;  // for a row without an owner in the CSV: the RVA that called the block
    std::uint64_t calls = 0;
    std::uint64_t ticks = 0;
    std::uint64_t max = 0;
    std::uint64_t main_ticks = 0;
};

// How a span ended.
enum class Ending : std::uint8_t { running, complete, interrupted_session, interrupted_exit };

struct Span {
    Ending ending = Ending::running;
    std::string first_date;  // the first and last day measured, "1719.3.18"; "" when no day passed
    std::string last_date;
    std::uint32_t days = 0;
    double seconds = 0;          // wall time
    double ticks_per_second = 3.0e9;
    std::string started_at;      // local wall time, "2026-11-12 08:40:00"
    std::string anvil_version;
    std::string game_build;
    bool multiplayer = false;
    bool ironman = false;
};

struct Report {
    Span span;
    std::vector<Row> triggers;  // by total time, longest first
    std::vector<Row> effects;
    std::vector<Row> scripted;  // every call of a scripted trigger, by name
    std::uint64_t root_calls = 0;
    std::uint64_t nested_triggers = 0;
    std::uint64_t nested_effects = 0;
    std::uint64_t nested_scripted = 0;
    std::uint64_t dropped_calls = 0;
    std::uint32_t threads = 0;
};

// Merges the snapshot's roots by owner and role. `split_unowned` keeps one
// row per calling RVA for blocks without an owner (the CSV); otherwise they
// are one row per list (the panel).
Report make_report(const Snapshot& snapshot, Resolver& resolver, const Span& span, bool split_unowned,
                   bool read_memory = true);

// The CSV: a few lines of `#` comments, then a header and every row of both
// lists, triggers first.
std::string csv(const Report& report);

// The file name for a span that started at `local` (SYSTEMTIME fields):
// script_profile_2026-11-12_084000.csv.
std::string csv_name(int year, int month, int day, int hour, int minute, int second);

// The panel's rows: kind, label, value (none when `has_value` is false), id.
struct PanelRow {
    std::uint32_t kind = 1;  // ANVIL_PANEL_TEXT 1, BUTTON 2, SEPARATOR 3
    std::string label;
    std::string value;
    bool has_value = false;
    std::uint64_t id = 0;
};

enum : std::uint64_t { kStart = 1, kStop = 2, kMeasureDays = 3 };

struct PanelState {
    bool measuring = false;
    std::uint32_t days_so_far = 0;
    std::uint32_t days_to_measure = 0;  // 0: until Stop
    std::int64_t measure_days = 30;     // the setting, for the button
    double seconds_so_far = 0;
    bool in_session = false;
    bool multiplayer = false;
    std::string csv_path;    // the last span's file; "" when none was written
    bool csv_failed = false;
    std::size_t top = 20;
};

std::vector<PanelRow> panel_rows(const PanelState& state, const Report* last, const Texts& t);

// "12.3 ms", "1.24 s", "820 µs".
std::string format_ms(double ms);
std::string format_count(std::uint64_t n);  // 1 234 567

}  // namespace script_profiler
