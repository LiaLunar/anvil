// Unit tests for the Manager's start-up benchmark (docs/spec/benchmark-v1.md):
// the start record as the core writes it, which starts count, the ABBA order,
// the extra-start budget, the result and the report. Part of
// anvil_manager_tests.
//
// The report is checked against the receiver's own files: the series of
// services/bench-receiver/examples/report-format-1.json gives that document,
// and a stopped series gives test/fixtures/manager-report-stopped.json, which
// the receiver's tests validate. ANVIL_UPDATE_FIXTURES=1 writes that fixture
// again after a deliberate change.
#include "benchmark_report.hpp"
#include "benchmark_run.hpp"
#include "benchmark_send.hpp"
#include "benchmark_series.hpp"
#include "core/benchmark_record.hpp"
#include "core/crash_history.hpp"
#include "core/log.hpp"
#include "files.hpp"
#include "json.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace anvil;
using namespace anvil::manager;

namespace {

namespace fs = std::filesystem;

const fs::path kReceiver = fs::path(ANVIL_SOURCE_DIR) / "services" / "bench-receiver";

// A participant of the core's record, with one field of each kind.
BenchmarkParticipant participant(const std::string& id) {
    BenchmarkParticipant p;
    p.id = id;
    p.done = true;
    BenchmarkField opened{"open_warmup.opened", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_COUNT, "Files opened", {}};
    BenchmarkField mode{"open_warmup.mode", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "", {"off", "on"}};
    BenchmarkField share{"share", ANVIL_BENCHMARK_NUMBER, ANVIL_BENCHMARK_UNIT_PERCENT, "", {}};
    BenchmarkField flag{"flag", ANVIL_BENCHMARK_FLAG, ANVIL_BENCHMARK_UNIT_NONE, "", {}};
    p.fields = {opened, mode, share, flag};
    p.values = {int64_t{11500}, BenchmarkOption{1}, 12.5, BenchmarkValue{}};
    p.open_cost = BenchmarkOpenCost{11500, 115.0};  // 0.01 ms per file: warm
    return p;
}

// What the core writes for a start of `arm` comparing "bellows".
BenchmarkRecord core_record(bool with, double ready_ms) {
    BenchmarkRecord r;
    r.write = "end";
    r.anvil_version = "1.5.0";
    r.arm = BenchmarkArm{with, "bellows"};
    r.process_id = 4242;
    r.qpc_frequency = 10'000'000;
    r.entry_qpc = 1'000'000;
    r.ready_qpc = r.entry_qpc + static_cast<int64_t>((ready_ms - 100.0) * 10'000);
    r.creation_filetime = 133'000'000'000'000'000ull;
    r.entry_filetime = r.creation_filetime + 100 * 10'000;  // 100 ms after creation
    r.plugins.push_back(BenchmarkRecordPlugin{"anvil-game", "1.5.0", "loaded", false, "", std::nullopt});
    BenchmarkRecordPlugin bellows{"bellows", "", "disabled", true, "", std::nullopt};
    if (with) bellows = BenchmarkRecordPlugin{"bellows", "0.10.0", "loaded", false, "", participant("bellows")};
    r.plugins.push_back(bellows);
    r.game_first_opens = BenchmarkOpenCost{11817, 11817 * 0.031};  // warm
    r.first_start_since_boot = false;
    r.definitions_changed = std::nullopt;
    r.input_during_start = false;
    return r;
}

StartObservation seen(const BenchmarkRecord& core) {
    StartObservation o;
    std::string error;
    o.record = parse_start_record(benchmark_record_json(core), error);
    if (!o.record) std::printf("  record error: %s\n", error.c_str());
    return o;
}

// A start of `arm` with the given time and open cost per file (ms).
StartObservation start(Arm arm, double ready_ms, double game_ms_per_file = 0.03) {
    BenchmarkRecord core = core_record(arm == Arm::With, ready_ms);
    core.game_first_opens = BenchmarkOpenCost{10000, 10000 * game_ms_per_file};
    if (arm == Arm::With) {
        // Without an open_cost() of its own, the game's first opens decide.
        core.plugins[1].participant->open_cost.reset();
    }
    return seen(core);
}

std::string arms(const Series& s) {
    std::string out;
    for (const StartRow& r : s.rows()) out += r.arm == Arm::Without ? 'A' : 'B';
    return out;
}

// ---- the series of the receiver's example report --------------------------------------

BenchmarkField field(const char* name, AnvilBenchmarkKind kind, AnvilBenchmarkUnit unit, const char* label,
                     std::vector<std::string> options = {}) {
    return BenchmarkField{name, kind, unit, label, std::move(options)};
}

// Bellows as the example has it: a profile source per module, counters.
// `profile` 0 none, 1 saved; nullopt values were never set.
BenchmarkParticipant bellows(uint32_t profile, int64_t finish_ms, std::optional<int64_t> bytes, std::optional<int64_t> hits) {
    const std::vector<std::string> modes{"off", "on", "verify", "failed"};
    const std::vector<std::string> profiles{"none", "saved", "rebuilt"};
    BenchmarkParticipant p;
    p.id = "bellows";
    p.done = true;
    p.fields = {field("open_warmup.mode", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "Open warm-up", modes),
                field("open_warmup.profile", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "Profile", profiles),
                field("open_warmup.opened", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_COUNT, "Files opened"),
                field("open_warmup.finish_ms", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_MS, "Finished after"),
                field("ram_prefetch.mode", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "", modes),
                field("ram_prefetch.profile", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "", profiles),
                field("ram_prefetch.bytes", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_BYTES, "Prefetched"),
                field("attr_cache.mode", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "", modes),
                field("attr_cache.hits", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_COUNT, "Hits")};
    const auto value = [](std::optional<int64_t> v) { return v ? BenchmarkValue{*v} : BenchmarkValue{}; };
    p.values = {BenchmarkOption{1}, BenchmarkOption{profile}, int64_t{11500}, finish_ms, BenchmarkOption{1},
                BenchmarkOption{profile}, value(bytes), BenchmarkOption{1}, value(hits)};
    p.open_cost = BenchmarkOpenCost{11500, static_cast<double>(finish_ms)};
    return p;
}

BenchmarkRecord example_record(bool with, double ready_ms, double game_ms) {
    BenchmarkRecord r = core_record(with, ready_ms);
    r.plugins = {BenchmarkRecordPlugin{"anvil-game", "1.5.0", "loaded", false, "", std::nullopt},
                 BenchmarkRecordPlugin{"anvil-overlay", "1.5.0", "loaded", false, "", std::nullopt},
                 BenchmarkRecordPlugin{"bellows", "", "disabled", true, "", std::nullopt}};
    r.game_first_opens = BenchmarkOpenCost{2400, game_ms};
    r.definitions_changed = false;
    return r;
}

StartObservation example_with(double ready_ms, BenchmarkParticipant p, bool input = false) {
    BenchmarkRecord r = example_record(true, ready_ms, 96.0);
    r.plugins[2] = BenchmarkRecordPlugin{"bellows", "0.10.0", "loaded", false, "", std::move(p)};
    r.input_during_start = input;
    return seen(r);
}

ReportInput example_input(const Series& series) {
    ReportInput in;
    in.series_id = "3f6c2a9e-8b1d-4c7a-9e2f-5a0b1c2d3e4f";
    in.series = &series;
    in.compared_version = "0.10.0";
    in.system = ReportSystem{"AMD Ryzen 9 7950X 16-Core Processor", 32, 64, "nvme", "nvme", "nvme", 26200,
                             {"Microsoft Defender Antivirus"}};
    in.mods = ReportMods{4, {"1999999901", "1999999902", "1999999903"}, 1, 11500, 2300};
    in.game_build = "eu4-1.37.5-steam-win64";
    in.anvil_version = "1.5.0";
    in.plugins = {ReportPluginSource{"anvil-game", "local", "", std::string(64, '1')},
                  ReportPluginSource{"anvil-overlay", "local", "", std::string(64, '2')},
                  ReportPluginSource{"bellows", "workshop", "1999999900", std::string(64, '3')}};
    return in;
}

Series example_series() {
    Series s("bellows", 3, Arm::Without);
    BenchmarkRecord boot = example_record(false, 52300.0, 9600.0);
    boot.first_start_since_boot = true;
    s.add(seen(boot));
    BenchmarkParticipant learning = bellows(0, 2300, std::nullopt, 0);
    learning.learning = "new_mod_set";
    s.add(example_with(39000.0, learning));
    s.add(example_with(33800.0, bellows(1, 110, 1610612736, 48210)));
    s.add(seen(example_record(false, 40900.0, 96.0)));
    s.add(seen(example_record(false, 41500.0, 101.0)));
    s.add(example_with(33500.0, bellows(1, 104, 1610612736, 48207)));
    BenchmarkParticipant late = bellows(1, 121, 1610612736, std::nullopt);
    late.done = false;
    s.add(example_with(34100.0, late, true));
    s.add(seen(example_record(false, 41000.0, 98.0)));
    return s;
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Where the members of two documents first differ, for the log.
std::string first_difference(const Json& a, const Json& b, const std::string& at = "") {
    if (a.type() != b.type() || a.is_array() != b.is_array()) return at;
    if (a.is_object()) {
        if (a.members().size() != b.members().size()) return at + " (members)";
        for (size_t i = 0; i < a.members().size(); ++i) {
            if (a.members()[i].first != b.members()[i].first) return at + "/" + a.members()[i].first + " (order)";
            const std::string d = first_difference(a.members()[i].second, b.members()[i].second, at + "/" + a.members()[i].first);
            if (!d.empty()) return d;
        }
        return "";
    }
    if (a.is_array()) {
        if (a.items().size() != b.items().size()) return at + " (items)";
        for (size_t i = 0; i < a.items().size(); ++i) {
            const std::string d = first_difference(a.items()[i], b.items()[i], at + "/" + std::to_string(i));
            if (!d.empty()) return d;
        }
        return "";
    }
    return a == b ? "" : at;
}

}  // namespace

void manager_benchmark_tests() {
    run("json: values, order and escapes read back; bad documents are refused", [] {
        std::string error;
        const auto v = parse_json(R"({"b":[1,-2,3.5,1e3,true,null],"a":"x\"\u00e9\ud83d\ude00\n","n":9007199254740993})", error);
        CHECK(v && v->is_object() && v->members().size() == 3 && v->members()[0].first == "b");
        CHECK((*v)["b"].items().size() == 6 && (*v)["b"].items()[1].is_integer() && (*v)["b"].items()[1].as_integer() == -2);
        CHECK(!(*v)["b"].items()[2].is_integer() && (*v)["b"].items()[2].as_number() == 3.5);
        CHECK(!(*v)["b"].items()[3].is_integer() && (*v)["b"].items()[3].as_number() == 1000.0);
        CHECK((*v)["a"].as_string() == "x\"\xC3\xA9\xF0\x9F\x98\x80\n");
        CHECK((*v)["n"].as_integer() == 9007199254740993);  // beyond a double's precision
        CHECK((*v)["missing"].is_null());
        const std::string compact = "{\"b\":[1,-2,3.5,1000,true,null],\"a\":\"x\\\"\xC3\xA9\xF0\x9F\x98\x80\\n\","
                                    "\"n\":9007199254740993}";
        CHECK(write_json(*v) == compact);
        const auto again = parse_json(write_json(*v, true), error);
        CHECK(again && *again == *v);
        for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "01", "1.", "\"\\ud800\"", "{\"a\":1,\"a\":2}", "nul",
                                "[1] [2]", "\"a\tb\"", "1e999"}) {
            CHECK(!parse_json(bad, error));
        }
        std::string deep(70, '[');
        CHECK(!parse_json(deep + std::string(70, ']'), error) && error.find("deep") != std::string::npos);
    });

    run("json: pretty output keeps short scalar groups on one line", [] {
        Json o = Json::object();
        o.set("id", "bellows");
        o.set("version", "0.10.0");
        Json doc = Json::object();
        doc.set("compared", o);
        Json times = Json::array();
        for (int t : {40900, 41500}) times.push(t);
        doc.set("times_ms", times);
        doc.set("empty", Json::object());
        doc.set("ms", 0.1);
        CHECK(write_json(doc, true) ==
              "{\n  \"compared\": { \"id\": \"bellows\", \"version\": \"0.10.0\" },\n  \"times_ms\": [40900, 41500],\n"
              "  \"empty\": {},\n  \"ms\": 0.1\n}\n");
        doc.set("ms", std::nan(""));
        CHECK(write_json(doc).find("\"ms\":null") != std::string::npos);
    });

    run("start record: the core's own record reads back", [] {
        const StartObservation o = seen(core_record(true, 33800.0));
        CHECK(o.record.has_value());
        const StartRecord& r = *o.record;
        CHECK(r.write == "end" && r.anvil == "1.5.0" && r.arm == Arm::With && r.arm_plugin == "bellows");
        CHECK(r.process_id == 4242 && r.creation_filetime == 133'000'000'000'000'000ull);
        CHECK(r.time_ms && std::abs(*r.time_ms - 33800.0) < 0.01);
        CHECK(r.plugins.size() == 2 && r.plugin("anvil-game") && !r.plugin("anvil-game")->participant);
        const RecordPlugin* b = r.plugin("bellows");
        CHECK(b && b->state == "loaded" && b->version == "0.10.0" && b->participant && b->done && b->learning.empty());
        CHECK(b && b->open_cost && b->open_cost->files == 11500 && b->open_cost->ms == 115.0);
        CHECK(b && b->schema.size() == 4 && b->schema[0].label == "Files opened" && b->schema[0].unit == "count");
        CHECK(b && b->schema[1].kind == "enum" && b->schema[1].options.size() == 2 && b->schema[1].unit.empty());
        CHECK(b && b->fields["open_warmup.opened"].as_integer() == 11500 && b->fields["open_warmup.mode"].as_string() == "on");
        CHECK(b && b->fields["share"].as_number() == 12.5 && b->fields["flag"].is_null());
        CHECK(r.game_first_opens && r.game_first_opens->files == 11817);
        CHECK(r.first_start_since_boot == false && !r.definitions_changed && r.input_during_start == false);

        const StartObservation without = seen(core_record(false, 41000.0));
        CHECK(without.record && without.record->plugin("bellows")->state == "disabled" &&
              without.record->plugin("bellows")->by == "arm");

        std::string error;
        CHECK(!parse_start_record("{\"format\":2}", error));
        CHECK(!parse_start_record("[]", error));
        BenchmarkRecord before_ready = core_record(false, 1000.0);
        before_ready.ready_qpc = 0;
        const auto early = parse_start_record(benchmark_record_json(before_ready), error);
        CHECK(early && !early->time_ms);
    });

    run("classify: valid starts and their open cost class", [] {
        std::string stop;
        StartRow row = classify_start(start(Arm::Without, 41000.4), Arm::Without, "bellows", stop);
        CHECK(row.valid && row.discard_reason.empty() && stop.empty() && row.time_ms == 41000);
        CHECK(row.open_cost && !row.open_cost->from_plugin && row.cold_class == "warm");
        CHECK(row.plugins.size() == 2 && row.input_during_start == false && !row.killed);

        row = classify_start(start(Arm::Without, 52000.0, 1.5), Arm::Without, "bellows", stop);
        CHECK(row.valid && row.cold_class == "cold");
        row = classify_start(start(Arm::Without, 45000.0, 0.33), Arm::Without, "bellows", stop);
        CHECK(row.valid && row.cold_class == "unclear" && row.not_counted());

        // With the plugin, its open_cost() replaces the game's first opens.
        BenchmarkRecord core = core_record(true, 33800.0);
        core.game_first_opens = BenchmarkOpenCost{10000, 10000 * 2.0};  // the game's own would say cold
        row = classify_start(seen(core), Arm::With, "bellows", stop);
        CHECK(row.valid && row.open_cost && row.open_cost->from_plugin && row.open_cost->plugin == "bellows");
        CHECK(row.cold_class == "warm");
        core.plugins[1].participant->open_cost = BenchmarkOpenCost{11500, 11500 * 0.1};  // 0.1 ms: unclear for a plugin
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).cold_class == "unclear");
        core.plugins[1].participant->open_cost = BenchmarkOpenCost{11500, 9100.0};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).cold_class == "cold");

        // No open cost at all is unclear; labels never decide.
        core = core_record(false, 41000.0);
        core.game_first_opens.reset();
        core.first_start_since_boot = true;
        CHECK(classify_start(seen(core), Arm::Without, "bellows", stop).cold_class == "unclear");
        core.game_first_opens = BenchmarkOpenCost{10000, 300.0};
        row = classify_start(seen(core), Arm::Without, "bellows", stop);
        CHECK(row.cold_class == "warm" && row.first_since_boot == true);

        // A participant with done: false does not invalidate the time.
        core = core_record(true, 33800.0);
        core.plugins[1].participant->done = false;
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).valid);
        CHECK(open_cost_class(std::nullopt) == "unclear");
        CHECK(open_cost_class(StartOpenCost{false, "", 0, 0.0}) == "unclear");
        CHECK(open_cost_class(StartOpenCost{false, "", 100, 20.0}) == "warm");
        CHECK(open_cost_class(StartOpenCost{false, "", 100, 100.0}) == "cold");
    });

    run("classify: discarded starts and the reasons that stop the series", [] {
        std::string stop;
        StartObservation o;
        StartRow row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(!row.valid && row.discard_reason == "no_record" && stop.empty() && row.cold_class.empty() && !row.time_ms);
        o.crash = StartCrash{"in_plugin_code", "bellows", true};
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.discard_reason == "crash" && stop.empty() && row.crash && row.crash->plugin == "bellows");
        o = StartObservation{};
        o.timed_out = true;
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.discard_reason == "timeout" && stop == "timeout" && row.killed);

        BenchmarkRecord core = core_record(true, 33800.0);
        core.plugins[1].participant->learning = "new_mod_set";
        row = classify_start(seen(core), Arm::With, "bellows", stop);
        CHECK(row.discard_reason == "plugin_learning" && stop.empty() && row.time_ms == 33800);
        CHECK(row.open_cost && row.open_cost->from_plugin);  // raw values stay in the row

        core = core_record(true, 33800.0);
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "", "skipped", false, "safe_mode", std::nullopt};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).discard_reason == "safe_mode" && stop == "safe_mode");
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "", "skipped", false, "verification", std::nullopt};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).discard_reason == "arm_mismatch" &&
              stop == "plugin_not_loaded");
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "0.9.0", "loaded", false, "", std::nullopt};
        CHECK(classify_start(seen(core), Arm::With, "bellows", stop).discard_reason == "not_participant" &&
              stop == "plugin_not_participant");
        // Without the plugin, only the arm's switch may have disabled it.
        CHECK(classify_start(seen(core), Arm::Without, "bellows", stop).discard_reason == "arm_mismatch");
        core = core_record(false, 41000.0);
        core.plugins[1].disabled_by_arm = false;
        CHECK(classify_start(seen(core), Arm::Without, "bellows", stop).discard_reason == "arm_mismatch");
        // The record of another arm or plugin.
        CHECK(classify_start(seen(core_record(false, 41000.0)), Arm::With, "bellows", stop).discard_reason == "arm_mismatch");
        CHECK(classify_start(seen(core_record(false, 41000.0)), Arm::Without, "other", stop).discard_reason == "arm_mismatch");

        o = start(Arm::Without, 41000.0);
        o.environment_changed = true;
        CHECK(classify_start(o, Arm::Without, "bellows", stop).discard_reason == "environment_changed" &&
              stop == "environment_changed");

        // A crash after ready or a kill keeps the time and stops the series.
        o = start(Arm::Without, 41000.0);
        o.crash = StartCrash{"no_plugin", "", false};
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.valid && row.time_ms == 41000 && row.crash && stop == "crashed");
        o = start(Arm::Without, 41000.0);
        o.killed = true;
        row = classify_start(o, Arm::Without, "bellows", stop);
        CHECK(row.valid && row.killed && stop == "quit_failed");
    });

    run("series: ABBA pairs, the first arm as given, complete at the target", [] {
        Series s("bellows", 3, Arm::Without);
        CHECK(s.expected_starts() == 6 && s.next() == Arm::Without);
        while (const auto arm = s.next()) s.add(start(*arm, *arm == Arm::Without ? 41000.0 : 34000.0));
        CHECK(arms(s) == "ABBAAB" && s.complete() && s.stop_reason().empty() && s.extra_starts_used() == 0);
        CHECK(s.warm(Arm::Without) == 3 && s.warm(Arm::With) == 3 && s.expected_starts() == 6);

        Series b("bellows", 5, Arm::With);
        while (const auto arm = b.next()) b.add(start(*arm, 40000.0));
        CHECK(arms(b) == "BAABBAABBA" && b.complete() && b.rows().back().position == 10);
    });

    run("series: starts that do not count are appended, alternating between the short arms", [] {
        Series s("bellows", 3, Arm::Without);
        std::vector<StartObservation> planned = {start(Arm::Without, 41000.0), StartObservation{}, start(Arm::With, 34000.0),
                                                 start(Arm::Without, 41000.0, 0.5), start(Arm::Without, 41000.0),
                                                 start(Arm::With, 34000.0)};
        for (const StartObservation& o : planned) s.add(o);
        // Starts 2 (no record, with) and 4 (unclear, without) left both arms short.
        CHECK(!s.over() && s.extra_starts_used() == 2 && s.expected_starts() == 8);
        CHECK(s.next() == Arm::With);  // the ABBA order goes on
        s.add(StartObservation{});
        CHECK(s.next() == Arm::Without);  // then the arms alternate
        s.add(start(Arm::Without, 41000.0));
        CHECK(s.next() == Arm::With);
        s.add(start(Arm::With, 34000.0));
        CHECK(s.complete() && arms(s) == "ABBAABBAB" && s.extra_starts_used() == 3);

        // A cold start leaves its arm short without using the budget.
        Series c("bellows", 3, Arm::Without);
        c.add(start(Arm::Without, 52000.0, 4.0));
        while (const auto arm = c.next()) c.add(start(*arm, 40000.0));
        CHECK(c.complete() && c.rows().size() == 7 && c.extra_starts_used() == 0 && arms(c) == "ABBAABA");
    });

    run("series: the budget, crashes and stop reasons end it", [] {
        Series s("bellows", 3, Arm::Without);
        for (int i = 0; i < 5; ++i) s.add(StartObservation{});
        CHECK(s.over() && !s.complete() && s.stop_reason() == "too_many_discarded" && s.extra_starts_used() == 5);

        Series cold("bellows", 3, Arm::Without);
        for (int i = 0; i < kMaxColdStarts + 1; ++i) cold.add(start(*cold.next(), 52000.0, 4.0));
        CHECK(cold.stop_reason() == "too_many_discarded" && cold.extra_starts_used() == 0);

        Series crash("bellows", 3, Arm::Without);
        StartObservation crashed;
        crashed.crash = StartCrash{"no_plugin", "", true};
        crash.add(crashed);
        CHECK(!crash.over());
        crash.add(start(*crash.next(), 34000.0));
        crash.add(crashed);
        CHECK(crash.stop_reason() == "crashed" && crash.rows().size() == 3);

        Series learn("bellows", 3, Arm::With);
        BenchmarkRecord core = core_record(true, 33800.0);
        core.plugins[1].participant->learning = "new_mod_set";
        learn.add(seen(core));
        CHECK(!learn.over() && learn.extra_starts_used() == 1 && learn.next() == Arm::Without);

        Series player("bellows", 3, Arm::Without);
        player.add(start(Arm::Without, 41000.0));
        player.stop("player");
        CHECK(player.over() && !player.next() && player.stop_reason() == "player" && player.expected_starts() == 1);

        Series safe("bellows", 3, Arm::Without);
        safe.add(start(Arm::Without, 41000.0));
        core = core_record(true, 33800.0);
        core.plugins[1] = BenchmarkRecordPlugin{"bellows", "", "skipped", false, "safe_mode", std::nullopt};
        safe.add(seen(core));
        CHECK(safe.stop_reason() == "safe_mode" && !safe.next());
        for (const char* reason : {"player", "too_many_discarded", "quit_failed", "crashed", "safe_mode", "plugin_not_loaded",
                                   "plugin_not_participant", "timeout", "environment_changed"}) {
            CHECK(stop_reason_text(reason).back() == '.');
        }
    });

    run("result: warm medians, the difference, cold starts and what was not measured", [] {
        Series s("bellows", 3, Arm::Without);
        BenchmarkRecord boot = core_record(false, 52300.0);
        boot.first_start_since_boot = true;
        boot.game_first_opens = BenchmarkOpenCost{2400, 9600.0};
        s.add(seen(boot));                                          // 1 A cold, after boot
        BenchmarkRecord learning = core_record(true, 39000.0);
        learning.plugins[1].participant->learning = "new_mod_set";
        s.add(seen(learning));                                      // 2 B learning
        s.add(start(Arm::With, 33800.0, 0.01));                     // 3 B
        s.add(start(Arm::Without, 40900.0));                        // 4 A
        s.add(start(Arm::Without, 41500.0));                        // 5 A
        s.add(start(Arm::With, 33500.0));                           // 6 B
        StartObservation input = start(Arm::With, 34100.0);
        input.record->input_during_start = true;
        s.add(input);                                               // 7 B, appended for 2
        s.add(start(Arm::Without, 41000.0));                        // 8 A, appended for 1
        CHECK(s.complete() && arms(s) == "ABBAABBA");

        const SeriesResult r = series_result(s);
        const ArmSummary& without = r.warm[arm_index(Arm::Without)];
        const ArmSummary& with = r.warm[arm_index(Arm::With)];
        CHECK((without.times_ms == std::vector<int64_t>{40900, 41500, 41000}));
        CHECK(without.median_ms == 41000.0 && without.min_ms == 40900 && without.max_ms == 41500);
        CHECK((with.times_ms == std::vector<int64_t>{33800, 33500, 34100}) && with.median_ms == 33800.0);
        CHECK(r.difference && r.difference->ms == -7200.0 && r.difference->percent == -17.56 && !r.difference->ranges_overlap);
        CHECK(r.cold.size() == 1 && r.cold[0].position == 1 && r.cold[0].kind == "after_boot" && r.cold[0].time_ms == 52300);
        CHECK(!r.cold_difference);
        CHECK(r.not_counted.size() == 1 && r.not_counted[0].position == 2 && r.not_counted[0].reason == "plugin_learning");
        CHECK((r.input_during_start == std::vector<int>{7}) && r.extra_starts_used == 1);
        CHECK((r.not_measured == std::vector<std::string>{"time_to_ready", "no_cold_with"}));

        // Overlapping ranges, an even median, a stopped series below its target.
        Series o("bellows", 3, Arm::Without);
        o.add(start(Arm::Without, 40000.0));
        o.add(start(Arm::With, 39000.0));
        o.add(start(Arm::With, 41000.0));
        o.add(start(Arm::Without, 42000.0));
        o.stop("player");
        const SeriesResult p = series_result(o);
        CHECK(p.warm[0].median_ms == 41000.0 && p.warm[1].median_ms == 40000.0);
        CHECK(p.difference && p.difference->ms == -1000.0 && p.difference->ranges_overlap);
        CHECK(std::abs(p.difference->percent - (-2.44)) < 1e-9);
        CHECK((p.not_measured == std::vector<std::string>{"time_to_ready", "no_cold_without", "no_cold_with", "below_target",
                                                          "stopped"}));

        // One arm without warm starts: no difference; cold in both arms: a cold difference.
        Series c("bellows", 3, Arm::Without);
        c.add(start(Arm::Without, 50000.0, 2.0));
        c.add(start(Arm::With, 40000.0, 2.0));
        c.add(start(Arm::With, 34000.0));
        c.stop("player");
        const SeriesResult q = series_result(c);
        CHECK(!q.difference && !q.warm[0].median_ms && !q.warm[0].min_ms && q.warm[1].times_ms.size() == 1);
        CHECK(q.cold.size() == 2 && q.cold[1].kind == "open_cost" && q.cold_difference && q.cold_difference->ms == -10000.0);
        CHECK(q.cold_difference->percent == -20.0);
    });

    run("report: the series of the receiver's example gives that document", [] {
        const Series s = example_series();
        CHECK(s.complete());
        const Json report = benchmark_report(example_input(s));
        std::string error;
        const auto example = parse_json(read_file(kReceiver / "examples" / "report-format-1.json"), error);
        CHECK(example.has_value());
        if (example) {
            const std::string at = first_difference(report, *example);
            if (!at.empty()) std::printf("  differs at %s\n", at.c_str());
            CHECK(at.empty() && report == *example);
        }
        // What is kept, previewed, sent and copied reads back as the same document.
        const std::string text = report_text(report);
        const auto again = parse_json(text, error);
        CHECK(again && *again == report && text.back() == '\n' && text.size() < 256 * 1024);
        // Labels stay in the preview: the record has them, the report does not.
        CHECK(text.find("Files opened") == std::string::npos && text.find("label") == std::string::npos);

        // The Workshop-id switch.
        Json without_ids = benchmark_report([&] {
            ReportInput in = example_input(s);
            in.include_workshop_ids = false;
            return in;
        }());
        CHECK(without_ids["mods"]["workshop_ids"].is_null() && without_ids["mods"]["enabled"].as_integer() == 4);
        Json switched = report;
        set_workshop_ids(switched, std::nullopt);
        CHECK(switched == without_ids);
        set_workshop_ids(switched, std::vector<std::string>{"1999999901", "1999999902", "1999999903"});
        CHECK(switched == report);
    });

    run("report: a stopped series matches the fixture the receiver validates", [] {
        Series s("bellows", 5, Arm::With);
        StartObservation crashed;
        crashed.crash = StartCrash{"in_plugin_code", "bellows", true};
        s.add(crashed);                                    // 1 B: crash before ready
        s.add(start(Arm::Without, 44000.0, 0.5));          // 2 A: unclear
        StartObservation unknown_input = start(Arm::Without, 41000.0);
        unknown_input.record->input_during_start.reset();
        s.add(unknown_input);                              // 3 A: warm, input unknown
        BenchmarkRecord killed = core_record(true, 61000.0);
        killed.plugins[1].participant = participant("bellows");
        killed.plugins[1].participant->open_cost = BenchmarkOpenCost{11500, 9100.0};
        killed.plugins.push_back(BenchmarkRecordPlugin{"old-ui", "", "skipped", false, "conflict", std::nullopt});
        killed.plugins.push_back(BenchmarkRecordPlugin{"broken", "", "failed", false, "", std::nullopt});
        killed.plugins.push_back(BenchmarkRecordPlugin{"late", "", "pending", false, "", std::nullopt});
        killed.write = "ready";
        StartObservation k = seen(killed);
        k.killed = true;
        s.add(k);                                          // 4 B: cold, killed
        CHECK(s.over() && s.stop_reason() == "quit_failed");

        ReportInput in = example_input(s);
        in.series_id = "00000000-0000-4000-8000-000000000000";
        in.compared_version = "0.10.0";
        in.system.antivirus.clear();
        in.system.drive_workshop = "unknown";
        in.include_workshop_ids = false;
        in.game_build.clear();
        in.game_sha256 = "9AD3EFE1" + std::string(52, 'A') + "771A";
        const Json report = benchmark_report(in);
        CHECK(report["series"]["state"].as_string() == "stopped" && report["series"]["stop_reason"].as_string() == "quit_failed");
        const Json& starts = report["starts"];
        CHECK(starts.items().size() == 4 && starts.items()[0]["discard_reason"].as_string() == "crash");
        CHECK(starts.items()[0]["crash"]["attribution"].as_string() == "in_plugin_code" && starts.items()[0]["time_ms"].is_null());
        CHECK(starts.items()[0]["plugins"].members().empty() && starts.items()[0]["cold_start"]["open_cost"].is_null());
        CHECK(starts.items()[1]["valid"].as_bool() && starts.items()[1]["cold_start"]["class"].as_string() == "unclear");
        CHECK(starts.items()[3]["killed"].as_bool() && starts.items()[3]["cold_start"]["class"].as_string() == "cold");
        CHECK(starts.items()[3]["plugins"].find("late") == nullptr);
        CHECK(starts.items()[3]["plugins"]["old-ui"]["reason"].as_string() == "conflict");
        CHECK(report["result"]["warm"]["with"]["median_ms"].is_null() && report["result"]["difference"].is_null());
        CHECK(report["versions"]["game"]["sha256"].as_string().substr(0, 8) == "9ad3efe1");
        CHECK(report["plugin_schemas"]["bellows"]["share"]["unit"].as_string() == "percent");
        CHECK(report["plugin_schemas"]["bellows"]["flag"].find("unit") == nullptr);

        const fs::path fixture = kReceiver / "test" / "fixtures" / "manager-report-stopped.json";
        const std::string text = report_text(report);
        if (const char* update = std::getenv("ANVIL_UPDATE_FIXTURES"); update != nullptr && std::string(update) == "1") {
            fs::create_directories(fixture.parent_path());
            std::ofstream(fixture, std::ios::binary | std::ios::trunc) << text;
        }
        std::string kept = read_file(fixture);
        std::erase(kept, '\r');  // a checkout with core.autocrlf
        const bool same = kept == text;
        if (!same) std::printf("  %s differs from the Manager's report (ANVIL_UPDATE_FIXTURES=1 writes it)\n", fixture.string().c_str());
        CHECK(same);
    });

    run("report: series ids are random UUID v4", [] {
        const std::regex uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
        const std::string a = new_series_id(), b = new_series_id();
        CHECK(std::regex_match(a, uuid) && std::regex_match(b, uuid) && a != b);
    });

    run("kept reports: the last 10, sent markers, the Workshop-id switch", [] {
        const fs::path dir = fs::temp_directory_path() / ("anvil-bench-kept-" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(dir);
        const Series s = example_series();
        const std::string text = report_text(benchmark_report(example_input(s)));
        fs::path first, file;
        std::string error;
        CHECK(keep_report(dir, "2026-10-26T10-00-00", text, first, error));
        CHECK(first.filename() == "2026-10-26T10-00-00.json" && read_file(first) == text);
        CHECK(keep_report(dir, "2026-10-26T10-00-00", text, file, error) && file.filename() == "2026-10-26T10-00-00-2.json");
        CHECK(mark_sent(first, error) && fs::exists(dir / "2026-10-26T10-00-00.sent"));
        CHECK(!rewrite_report(first, "{}", error) && read_file(first) == text);
        CHECK(rewrite_report(file, text + " ", error) && read_file(file) == text + " ");

        std::vector<KeptReport> kept = kept_reports(dir);
        CHECK(kept.size() == 2 && kept[0].name == "2026-10-26T10-00-00-2" && !kept[0].sent);
        CHECK(kept[1].sent && kept[1].readable && kept[1].compared == "bellows" && kept[1].version == "0.10.0");
        CHECK(kept[1].state == "complete" && kept[1].stop_reason.empty());

        for (int i = 1; i <= 10; ++i) {
            char stamp[32];
            std::snprintf(stamp, sizeof(stamp), "2026-10-26T11-%02d-00", i);
            CHECK(keep_report(dir, stamp, text, file, error));
        }
        kept = kept_reports(dir);
        CHECK(kept.size() == kKeptReports && kept.front().name == "2026-10-26T11-10-00" && kept.back().name == "2026-10-26T11-01-00");
        CHECK(!fs::exists(first) && !fs::exists(dir / "2026-10-26T10-00-00.sent"));

        write_file_atomic(dir / "zz-broken.json", "{", error);
        kept = kept_reports(dir);
        CHECK(kept.front().name == "zz-broken" && !kept.front().readable);
        CHECK(report_stamp().size() == 19 && report_stamp()[10] == 'T');
        CHECK(benchmarks_dir("C:/Games/EU4") == fs::path("C:/Games/EU4/anvil/data/anvil/benchmarks"));
        fs::remove_all(dir);
    });
}

// ---- the runner, with this test as the game ----------------------------------------------

namespace {

std::wstring env(const wchar_t* name) {
    wchar_t value[512] = {};
    const DWORD n = GetEnvironmentVariableW(name, value, 512);
    return n > 0 && n < 512 ? std::wstring(value, n) : std::wstring();
}

// The game folder of a runner test, with the fake game's modes: one per
// start, in turn, from modes.txt.
struct FakeGame {
    fs::path dir;
    explicit FakeGame(const std::string& modes) {
        dir = fs::temp_directory_path() / ("anvil-bench-run-" + std::to_string(GetCurrentProcessId()) + "-" +
                                           std::to_string(GetTickCount64()));
        fs::create_directories(dir / "anvil" / "data" / "anvil");
        std::ofstream(dir / "modes.txt", std::ios::binary) << modes;
    }
    ~FakeGame() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

RunnerConfig fake_config(const FakeGame& game, int target, Arm first) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    RunnerConfig c;
    c.game_dir = game.dir;
    c.exe = self;
    c.arguments = L"--bench-game";
    c.plugin = "stub";
    c.target = target;
    c.first = first;
    c.ready_timeout = std::chrono::milliseconds(3000);
    c.close_after = std::chrono::milliseconds(3000);
    c.kill_after = std::chrono::milliseconds(1000);
    c.pause = std::chrono::milliseconds(0);
    c.poll = std::chrono::milliseconds(20);
    return c;
}

RunProgress finish(SeriesRunner& runner, int seconds = 60) {
    for (int i = 0; i < seconds * 50 && !runner.done(); ++i) Sleep(20);
    return runner.progress();
}

LRESULT CALLBACK fake_window(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

// A Windows environment block from its entries.
std::wstring block(std::initializer_list<const wchar_t*> entries) {
    std::wstring out;
    for (const wchar_t* e : entries) {
        out += e;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

}  // namespace

// The fake eu4.exe: `anvil_manager_tests --bench-game`, started by the
// runner in a FakeGame folder. It writes the core's own record with a stub
// participant (the role Bellows 0.10 plays) and ends as its mode says.
int fake_benchmark_game() {
    if (env(L"SteamAppId") != L"236850") return 3;
    std::string error;
    const auto arm = parse_benchmark_arm(narrow(env(L"ANVIL_BENCHMARK")), error);
    if (!arm) return 4;
    const fs::path dir = fs::current_path();
    std::vector<std::string> modes;
    {
        std::ifstream in(dir / "modes.txt", std::ios::binary);
        for (std::string m; std::getline(in, m, ',');) modes.push_back(m);
    }
    int n = 0;
    {
        std::ifstream in(dir / "starts.txt");
        in >> n;
    }
    std::ofstream(dir / "starts.txt", std::ios::trunc) << n + 1;
    const std::string mode = modes.empty() ? "quit" : modes[static_cast<size_t>(n) % modes.size()];
    if (mode == "no-record") return 0;
    if (mode == "hang") Sleep(INFINITE);
    if (mode == "crash") {
        write_file_atomic(dir / "anvil" / "crashes" / ("2026-10-26T12-00-0" + std::to_string(n) + ".json"),
                          "{\n  \"time\": \"x\",\n  \"level\": \"in_plugin_code\",\n  \"before_game_ready\": true,\n"
                          "  \"plugins_involved\": [\"stub\"]\n}\n",
                          error);
        return 1;
    }

    BenchmarkRecord r;
    r.write = "ready";
    r.anvil_version = "1.5.0";
    r.arm = *arm;
    r.process_id = GetCurrentProcessId();
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    r.creation_filetime = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    r.entry_filetime = r.creation_filetime + 50 * 10'000;
    r.qpc_frequency = 10'000'000;
    r.entry_qpc = 1;
    r.ready_qpc = 1 + 20'000'000;  // 2 s after the entry point
    r.plugins.push_back(BenchmarkRecordPlugin{"anvil-game", "1.5.0", "loaded", false, "", std::nullopt});
    if (arm->with) {
        BenchmarkParticipant stub;
        stub.id = arm->plugin;
        stub.fields = {field("work.ms", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_MS, "Work")};
        stub.values = {int64_t{12}};
        if (mode == "learning") stub.learning = "first_start";
        r.plugins.push_back(BenchmarkRecordPlugin{arm->plugin, "0.1.0", "loaded", false, "", stub});
    } else {
        r.plugins.push_back(BenchmarkRecordPlugin{arm->plugin, "", "disabled", true, "", std::nullopt});
    }
    r.game_first_opens = BenchmarkOpenCost{10000, 300.0};
    r.first_start_since_boot = false;
    r.definitions_changed = false;
    r.input_during_start = false;
    if (mode == "slow") Sleep(1500);
    const fs::path record = start_record_file(dir);
    write_file_atomic(record, benchmark_record_json(r), error);
    if (mode == "stuck") Sleep(INFINITE);
    if (mode == "window") {
        WNDCLASSW wc{};
        wc.lpfnWndProc = fake_window;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"anvil-fake-game";
        RegisterClassW(&wc);
        CreateWindowExW(0, wc.lpszClassName, L"fake game", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                        wc.hInstance, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        return 0;
    }
    Sleep(200);  // the participant finishes its after-ready work
    if (r.plugins[1].participant) r.plugins[1].participant->done = true;
    r.write = "end";
    write_file_atomic(record, benchmark_record_json(r), error);
    return 0;
}

void manager_runner_tests() {
    run("send: the line under Send for each answer of the receiver", [] {
        CHECK(send_outcome_text(SendOutcome{201, "{\"message\":\"Received. Thank you.\"}", ""}) == "Sent. Thank you.");
        CHECK(send_outcome_text(SendOutcome{200, "", ""}) == "This report has already arrived.");
        CHECK(send_accepted(SendOutcome{201, "", ""}) && send_accepted(SendOutcome{200, "", ""}));
        CHECK(!send_accepted(SendOutcome{422, "", ""}) && !send_accepted(SendOutcome{0, "", "x"}));
        // 413 and 422: the server's message.
        CHECK(send_outcome_text(SendOutcome{422, "{\"message\":\"The report does not match format 1: / bad\"}", ""}) ==
              "The report does not match format 1: / bad");
        CHECK(send_outcome_text(SendOutcome{413, "not json", ""}) == "The receiver refused the report.");
        CHECK(send_outcome_text(SendOutcome{429, "{\"message\":\"Too many\"}", ""}) == "The receiver is busy. Try again later.");
        CHECK(send_outcome_text(SendOutcome{503, "", ""}) == "The receiver is busy. Try again later.");
        CHECK(send_outcome_text(SendOutcome{0, "", "cannot reach bench.happydwarf.dev (error 12007)"}) ==
              "The report was not sent: cannot reach bench.happydwarf.dev (error 12007). You can copy it instead.");
        CHECK(send_outcome_text(SendOutcome{404, "{\"message\":\"Not found.\"}", ""}) == "The receiver answered 404: Not found.");
        CHECK(receiver_message("{\"message\":" + std::string("\"") + std::string(400, 'x') + "\"}").size() == 300);
    });

    run("runner: the child's environment carries the arm and SteamAppId", [] {
        const std::wstring current = block({L"=C:=C:\\x", L"Path=C:\\Windows", L"steamappid=1", L"ANVIL_BENCHMARK=old", L"Zed=1"});
        CHECK(benchmark_environment(current, Arm::With, "bellows") ==
              block({L"=C:=C:\\x", L"ANVIL_BENCHMARK=with:bellows", L"Path=C:\\Windows", L"SteamAppId=236850", L"Zed=1"}));
        CHECK(benchmark_environment(L"", Arm::Without, "b") == block({L"ANVIL_BENCHMARK=without:b", L"SteamAppId=236850"}));
        // A GOG install starts without Steam: an inherited SteamAppId goes as well.
        CHECK(benchmark_environment(current, Arm::With, "bellows", false) ==
              block({L"=C:=C:\\x", L"ANVIL_BENCHMARK=with:bellows", L"Path=C:\\Windows", L"Zed=1"}));
    });

    run("runner: a series of starts that quit by themselves completes", [] {
        FakeGame game("quit");
        int reads = 0;
        RunnerConfig c = fake_config(game, 3, Arm::With);
        c.environment = [&] {
            ++reads;
            return SeriesEnvironment{true, "mods", "0.1.0", "dll"};
        };
        SeriesRunner runner(c);
        const RunProgress p = finish(runner);
        CHECK(p.phase == RunPhase::Done && p.series.complete() && arms(p.series) == "BAABBA");
        CHECK(reads == 1 + 2 * 6);  // when the series starts, then before and after every start
        for (const StartRow& row : p.series.rows()) {
            CHECK(row.valid && row.warm() && row.time_ms == 2050 && !row.killed);
            CHECK(row.open_cost && !row.open_cost->from_plugin);  // the stub reports no open_cost()
        }
        CHECK(p.series.rows()[0].plugins.size() == 2 && p.series.rows()[0].plugins[1].done);
        CHECK(!fs::exists(game.dir / "anvil" / "anvil.toml"));  // nothing is written but by the game
        ReportInput in = example_input(p.series);
        in.plugins.push_back(ReportPluginSource{"stub", "local", "", std::string(64, 'a')});
        in.compared_version = "0.1.0";
        const Json report = benchmark_report(in);
        CHECK(report["plugin_schemas"]["stub"]["work.ms"]["unit"].as_string() == "ms");
        CHECK(report["versions"]["plugins"].items().size() == 2);  // anvil-game and stub
    });

    run("runner: starts that do not count are replaced; a crash is read from its report", [] {
        FakeGame game("learning,no-record,quit,quit,crash,quit,quit,quit,quit");
        SeriesRunner runner(fake_config(game, 3, Arm::With));
        const RunProgress p = finish(runner);
        const auto& rows = p.series.rows();
        CHECK(p.series.complete() && rows.size() == 9 && p.series.extra_starts_used() == 3);
        CHECK(rows.size() > 3 && rows[0].discard_reason == "plugin_learning" && rows[1].discard_reason == "no_record");
        CHECK(rows.size() > 4 && rows[4].discard_reason == "crash" && rows[4].crash &&
              rows[4].crash->attribution == "in_plugin_code" && rows[4].crash->plugin == "stub" && rows[4].crash->before_ready);
    });

    run("runner: no ready in time is a timeout; the process is killed", [] {
        FakeGame game("hang");
        RunnerConfig c = fake_config(game, 3, Arm::Without);
        c.ready_timeout = std::chrono::milliseconds(300);
        SeriesRunner runner(c);
        const RunProgress p = finish(runner);
        CHECK(p.series.stop_reason() == "timeout" && p.series.rows().size() == 1 && p.series.rows()[0].killed);
    });

    run("runner: WM_CLOSE after ready ends a game that does not quit; a kill stops the series", [] {
        {
            FakeGame game("window");
            RunnerConfig c = fake_config(game, 3, Arm::Without);
            c.close_after = std::chrono::milliseconds(100);
            SeriesRunner runner(c);
            const RunProgress p = finish(runner);
            CHECK(p.series.complete() && p.series.rows().size() == 6 && !p.series.rows()[0].killed);
        }
        FakeGame game("stuck");
        RunnerConfig c = fake_config(game, 3, Arm::Without);
        c.close_after = std::chrono::milliseconds(100);
        c.kill_after = std::chrono::milliseconds(200);
        SeriesRunner runner(c);
        const RunProgress p = finish(runner);
        CHECK(p.series.stop_reason() == "quit_failed" && p.series.rows().size() == 1);
        CHECK(p.series.rows()[0].killed && p.series.rows()[0].valid && p.series.rows()[0].time_ms == 2050);
    });

    run("runner: the player's stop lets the start finish; a changed environment stops the series", [] {
        {
            FakeGame game("slow");
            RunnerConfig c = fake_config(game, 3, Arm::Without);
            c.pause = std::chrono::milliseconds(5000);
            SeriesRunner runner(c);
            for (int i = 0; i < 500 && runner.progress().phase != RunPhase::Loading; ++i) Sleep(10);
            runner.stop();
            CHECK(runner.progress().stopping);
            const RunProgress p = finish(runner);
            CHECK(p.series.stop_reason() == "player" && p.series.rows().size() == 1 && p.series.rows()[0].valid);
        }
        FakeGame game("quit");
        int reads = 0;
        RunnerConfig c = fake_config(game, 3, Arm::Without);
        c.environment = [&] { return SeriesEnvironment{true, ++reads >= 5 ? "other mods" : "mods", "0.1.0", "dll"}; };
        SeriesRunner runner(c);
        const RunProgress p = finish(runner);
        // Read 5 is after the second start: that start is discarded and ends the series.
        CHECK(p.series.stop_reason() == "environment_changed" && p.series.rows().size() == 2);
        CHECK(p.series.rows().back().discard_reason == "environment_changed" && p.environment_change == "the enabled mods changed");

        FakeGame missing("quit");
        RunnerConfig m = fake_config(missing, 3, Arm::Without);
        m.exe = missing.dir / "eu4.exe";
        SeriesRunner none(m);
        const RunProgress q = finish(none);
        CHECK(q.series.stop_reason() == "environment_changed" && q.series.rows().empty() && !q.start_error.empty());
    });

    run("setup: the checks before a series", [] {
        SetupFacts f;
        f.steam_running = true;
        f.installed_anvil = "1.5.0";
        f.anvil_game_loads = true;
        f.plugin = "bellows";
        f.comparable = ComparablePlugin{"bellows", "0.10.0", "workshop", "1999999900", "x.dll", false};
        std::vector<SetupCheck> checks = setup_checks(f);
        CHECK(checks.size() == 8 && checks_pass(checks));
        f.on_battery = true;
        checks = setup_checks(f);
        CHECK(checks_pass(checks) && !checks.back().ok && !checks.back().blocks);
        const std::vector<std::function<void(SetupFacts&)>> failing{
            [](SetupFacts& x) { x.steam_running = false; }, [](SetupFacts& x) { x.eu4_running = true; },
            [](SetupFacts& x) { x.unfinished_starts = 2; }, [](SetupFacts& x) { x.installed_anvil = "1.3.0"; },
            [](SetupFacts& x) { x.installed_anvil.clear(); }, [](SetupFacts& x) { x.anvil_game_loads = false; },
            [](SetupFacts& x) { x.comparable.reset(); }, [](SetupFacts& x) { x.comparable->update_pending = true; }};
        for (const auto& change : failing) {
            SetupFacts g = f;
            change(g);
            CHECK(!checks_pass(setup_checks(g)));
        }
        f.unfinished_starts = 1;
        CHECK(checks_pass(setup_checks(f)));
        // A GOG install starts without Steam, so its state is no check there.
        SetupFacts gog = f;
        gog.needs_steam = false;
        gog.steam_running = false;
        CHECK(setup_checks(gog).size() == checks.size() - 1 && checks_pass(setup_checks(gog)));

        const SeriesEnvironment a{true, "m", "1", "h"};
        CHECK(environment_change(a, a).empty());
        CHECK(environment_change(a, SeriesEnvironment{false, "m", "1", "h"}) == "Steam is no longer running");
        CHECK(environment_change(a, SeriesEnvironment{true, "m", "2", "h"}) == "the compared plugin's version changed");
        CHECK(environment_change(a, SeriesEnvironment{true, "m", "1", "i"}) == "the compared plugin's files changed");
    });

    run("setup: the system, drives and mods read without elevation", [] {
        const ReportSystem s = read_system(fs::temp_directory_path(), fs::temp_directory_path());
        CHECK(!s.cpu.empty() && s.cpu.size() <= 128 && s.logical_cores >= 1 && s.ram_gb >= 1 && s.windows_build >= 10000);
        const std::set<std::string> kinds{"nvme", "sata_ssd", "hdd", "other", "unknown"};
        CHECK(kinds.count(s.drive_game) && kinds.count(s.drive_user) && kinds.count(s.drive_workshop));
        CHECK(drive_kind("") == "unknown" && s.antivirus.size() <= 8);
        std::printf("  this computer: %s, %d cores, %d GB, temp on %s, build %d, %zu antivirus\n", s.cpu.c_str(),
                    s.logical_cores, s.ram_gb, s.drive_game.c_str(), s.windows_build, s.antivirus.size());

        const fs::path user = fs::temp_directory_path() / ("anvil-bench-mods-" + std::to_string(GetCurrentProcessId()));
        const fs::path workshop = user / "steamapps" / "workshop" / "content" / "236850" / "123";
        std::string error;
        write_file_atomic(user / "mod" / "ugc_123.mod", "name = \"W\"\npath = \"" + workshop.generic_string() + "\"\n", error);
        write_file_atomic(user / "mod" / "local.mod", "name = \"L\"\npath = \"mod/local\"\n", error);
        write_file_atomic(user / "dlc_load.json", "{\"enabled_mods\":[\"mod/ugc_123.mod\",\"mod/local.mod\"]}", error);
        for (int i = 0; i < 120; ++i) write_file_atomic(workshop / ("f" + std::to_string(i) + ".txt"), "x", error);
        write_file_atomic(user / "mod" / "local" / "a.txt", "y", error);
        const ReportMods m = read_mods(user);
        CHECK(m.enabled == 2 && m.local == 1 && (m.workshop_ids == std::vector<std::string>{"123"}));
        CHECK(m.files == 100 && m.size_mb == 0);  // 121 files, rounded
        CHECK(workshop_folder("C:/Lib/steamapps/common/EU4", user) == workshop.parent_path());
        CHECK(workshop_folder("C:/Lib/steamapps/common/EU4", user / "none") == fs::path("C:/Lib/steamapps/workshop/content/236850"));
        std::error_code ec;
        fs::remove_all(user, ec);
    });
}
