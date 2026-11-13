// anvil.benchmark v1 (include/anvil/anvil_benchmark.h, docs/spec/benchmark-v1.md
// §5): declaring fields, setting values, done(), learning() and open_cost(),
// through the service table a plugin calls.
#include "core/benchmark.hpp"
#include "core/benchmark_record.hpp"
#include "core/files.hpp"
#include "core/first_opens.hpp"
#include "core/plugin_set.hpp"
#include "core/manifest.hpp"
#include "core/resolver.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

struct Fixture {
    std::vector<std::string> warnings;
    Benchmark bench;
    const AnvilBenchmarkService& s;
    AnvilPlugin plugin;

    explicit Fixture(bool active = true)
        : bench(active, [this](const AnvilPlugin& p, const std::string& m) { warnings.push_back(p.name + ": " + m); }),
          s(*bench.service()) {
        plugin.name = "bellows";
        plugin.loading = true;
    }
    const BenchmarkParticipant* participant() {
        static std::vector<BenchmarkParticipant> copy;
        copy = bench.participants();
        return copy.empty() ? nullptr : &copy.front();
    }
};

const char* const kModes[] = {"off", "on", "verify"};

std::vector<AnvilBenchmarkField> some_fields() {
    return {
        {"open_warmup.mode", ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, "Open warm-up", kModes, 3},
        {"open_warmup.opened", ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_COUNT, "Files opened", nullptr, 0},
        {"open_warmup.time", ANVIL_BENCHMARK_NUMBER, ANVIL_BENCHMARK_UNIT_MS, nullptr, nullptr, 0},
        {"profile_saved", ANVIL_BENCHMARK_FLAG, ANVIL_BENCHMARK_UNIT_NONE, nullptr, nullptr, 0},
    };
}

AnvilBenchmarkField int_field(const char* name) {
    return {name, ANVIL_BENCHMARK_INT, ANVIL_BENCHMARK_UNIT_NONE, nullptr, nullptr, 0};
}

}  // namespace

void benchmark_tests() {
    run("benchmark: names", [] {
        CHECK(is_valid_benchmark_name("open_warmup.opened"));
        CHECK(is_valid_benchmark_name("mode"));
        CHECK(is_valid_benchmark_name("a1_2.b"));
        CHECK(is_valid_benchmark_name(std::string(ANVIL_BENCHMARK_MAX_NAME, 'a')));
        CHECK(!is_valid_benchmark_name(std::string(ANVIL_BENCHMARK_MAX_NAME + 1, 'a')));
        CHECK(!is_valid_benchmark_name(""));
        CHECK(!is_valid_benchmark_name("Mode"));
        CHECK(!is_valid_benchmark_name("a.b.c"));
        CHECK(!is_valid_benchmark_name(".a"));
        CHECK(!is_valid_benchmark_name("a."));
        CHECK(!is_valid_benchmark_name("a-b"));
        CHECK(!is_valid_benchmark_name("C:\\x"));
    });

    run("benchmark: the arm", [] {
        std::string error;
        auto arm = parse_benchmark_arm("without:bellows", error);
        CHECK(arm && !arm->with && arm->plugin == "bellows" && arm->text() == "without:bellows");
        arm = parse_benchmark_arm("with:anvil-game", error);
        CHECK(arm && arm->with && arm->plugin == "anvil-game");
        for (const char* bad : {"", "with", "with:", "without:", "With:bellows", "on:bellows", "with:Bellows",
                                "with: bellows", "without:bellows,other", "with:a/b"}) {
            error.clear();
            CHECK(!parse_benchmark_arm(bad, error) && !error.empty());
        }
    });

    run("benchmark: the arm without the plugin disables it in the resolver", [] {
        ResolveInput in;
        in.host_api = kManifestApi;
        in.manifests = {
            manifest(plugin_toml("bellows")),
            manifest(plugin_toml("ui", "[[dependencies]]\nid = \"bellows\"\n")),
            manifest(plugin_toml("other")),
        };
        in.benchmark_without = "bellows";
        ResolveResult r = resolve(in);
        CHECK(r.order.size() == 1 && r.order[0].id == "other");
        const Skipped* bellows = nullptr;
        for (const auto& s : r.skipped) {
            if (s.id == "bellows") bellows = &s;
        }
        CHECK(bellows != nullptr && bellows->disabled && bellows->benchmark_arm);
        CHECK(bellows != nullptr && bellows->reason == "left out by the start-up benchmark (ANVIL_BENCHMARK=without:bellows)");
        // The player's switch comes first.
        in.disabled = {"bellows"};
        r = resolve(in);
        for (const auto& s : r.skipped) {
            if (s.id == "bellows") CHECK(s.disabled && !s.benchmark_arm && s.reason == "disabled in anvil.toml");
        }
    });

    run("benchmark: the table and active()", [] {
        Fixture on(true);
        CHECK(on.s.size == sizeof(AnvilBenchmarkService));
        CHECK(ANVIL_BENCHMARK_HAS(&on.s, open_cost));
        CHECK(on.s.active() == 1);
        Fixture* f = new Fixture(false);  // the newest Benchmark serves the table
        CHECK(f->s.active() == 0);
        delete f;
        CHECK(on.s.active() == 0);  // none left behind the process-wide pointer
    });

    run("benchmark: declaring fields", [] {
        Fixture f;
        const auto fields = some_fields();
        CHECK(f.s.declare(&f.plugin, fields.data(), 2) == ANVIL_OK);
        CHECK(f.s.declare(&f.plugin, fields.data() + 2, 2) == ANVIL_OK);  // a second call adds
        const BenchmarkParticipant* p = f.participant();
        CHECK(p != nullptr && p->id == "bellows" && p->fields.size() == 4 && p->values.size() == 4);
        CHECK(p != nullptr && p->fields[0].options == (std::vector<std::string>{"off", "on", "verify"}));
        CHECK(p != nullptr && p->fields[0].label == "Open warm-up" && p->fields[2].label.empty());
        CHECK(p != nullptr && p->fields[1].unit == ANVIL_BENCHMARK_UNIT_COUNT && p->fields[3].kind == ANVIL_BENCHMARK_FLAG);
        CHECK(p != nullptr && std::holds_alternative<std::monostate>(p->values[1]));
        CHECK(f.warnings.empty());

        // Only inside anvil_plugin_load.
        f.plugin.loading = false;
        const AnvilBenchmarkField late = int_field("late");
        CHECK(f.s.declare(&f.plugin, &late, 1) == ANVIL_ERROR_WRONG_PHASE);
        CHECK(f.warnings.size() == 1 && f.warnings[0].find("inside anvil_plugin_load") != std::string::npos);
        CHECK(f.participant()->fields.size() == 4);
    });

    run("benchmark: zero fields join", [] {
        Fixture f;
        CHECK(f.s.declare(&f.plugin, nullptr, 0) == ANVIL_OK);
        CHECK(f.participant() != nullptr && f.participant()->fields.empty());
        CHECK(f.s.done(&f.plugin) == ANVIL_OK && f.participant()->done);
    });

    run("benchmark: bad declarations add nothing", [] {
        Fixture f;
        const auto refused = [&](AnvilBenchmarkField field, AnvilResult expected) {
            const AnvilBenchmarkField pair[] = {int_field("good"), field};
            const AnvilResult r = f.s.declare(&f.plugin, pair, 2);
            return r == expected && f.participant()->fields.empty();
        };
        CHECK(refused(int_field("Bad"), ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused(int_field(nullptr), ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused(int_field("good"), ANVIL_ERROR_ALREADY_EXISTS));
        CHECK(refused({"k", 9, 0, nullptr, nullptr, 0}, ANVIL_ERROR_INVALID_ARGUMENT));                        // kind
        CHECK(refused({"u", ANVIL_BENCHMARK_INT, 6, nullptr, nullptr, 0}, ANVIL_ERROR_INVALID_ARGUMENT));       // unit
        CHECK(refused({"f", ANVIL_BENCHMARK_FLAG, ANVIL_BENCHMARK_UNIT_MS, nullptr, nullptr, 0},
                      ANVIL_ERROR_INVALID_ARGUMENT));  // a unit on a flag
        CHECK(refused({"e", ANVIL_BENCHMARK_ENUM, 0, nullptr, nullptr, 0}, ANVIL_ERROR_INVALID_ARGUMENT));    // no options
        CHECK(refused({"i", ANVIL_BENCHMARK_INT, 0, nullptr, kModes, 3}, ANVIL_ERROR_INVALID_ARGUMENT));      // options on an int
        const char* const bad_options[] = {"on", "On"};
        CHECK(refused({"e", ANVIL_BENCHMARK_ENUM, 0, nullptr, bad_options, 2}, ANVIL_ERROR_INVALID_ARGUMENT));
        const char* const twice[] = {"on", "on"};
        CHECK(refused({"e", ANVIL_BENCHMARK_ENUM, 0, nullptr, twice, 2}, ANVIL_ERROR_INVALID_ARGUMENT));
        const std::vector<const char*> many(ANVIL_BENCHMARK_MAX_OPTIONS + 1, "x");
        CHECK(refused({"e", ANVIL_BENCHMARK_ENUM, 0, nullptr, many.data(), static_cast<uint32_t>(many.size())},
                      ANVIL_ERROR_INVALID_ARGUMENT));
        const std::string long_label(ANVIL_BENCHMARK_MAX_LABEL + 1, 'x');
        CHECK(refused({"l", ANVIL_BENCHMARK_INT, 0, long_label.c_str(), nullptr, 0}, ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(refused({"l", ANVIL_BENCHMARK_INT, 0, "two\nlines", nullptr, 0}, ANVIL_ERROR_INVALID_ARGUMENT));
        CHECK(f.s.declare(&f.plugin, nullptr, 1) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.warnings.size() == 14);  // one per refused declare call, never deduplicated
        CHECK(f.warnings[0].find("field name 'Bad'") != std::string::npos);

        // At most 128 fields per plugin, over all calls.
        std::vector<std::string> names;
        for (int i = 0; i < ANVIL_BENCHMARK_MAX_FIELDS + 1; ++i) names.push_back("f" + std::to_string(i));
        std::vector<AnvilBenchmarkField> fields;
        for (const auto& n : names) fields.push_back(int_field(n.c_str()));
        CHECK(f.s.declare(&f.plugin, fields.data(), ANVIL_BENCHMARK_MAX_FIELDS) == ANVIL_OK);
        CHECK(f.s.declare(&f.plugin, fields.data() + ANVIL_BENCHMARK_MAX_FIELDS, 1) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.participant()->fields.size() == ANVIL_BENCHMARK_MAX_FIELDS);
        // A refused declaration still made the plugin a participant.
        AnvilPlugin other;
        other.name = "other";
        other.loading = true;
        CHECK(f.s.declare(&other, nullptr, 1) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.bench.participants().size() == 2);
    });

    run("benchmark: values", [] {
        Fixture f;
        const auto fields = some_fields();
        CHECK(f.s.declare(&f.plugin, fields.data(), 4) == ANVIL_OK);
        f.plugin.loading = false;  // values come from any thread at any time
        CHECK(f.s.set_enum(&f.plugin, "open_warmup.mode", "verify") == ANVIL_OK);
        CHECK(f.s.set_int(&f.plugin, "open_warmup.opened", 11) == ANVIL_OK);
        CHECK(f.s.set_int(&f.plugin, "open_warmup.opened", 11525) == ANVIL_OK);  // the last one wins
        CHECK(f.s.set_number(&f.plugin, "open_warmup.time", 91.5) == ANVIL_OK);
        CHECK(f.s.set_flag(&f.plugin, "profile_saved", 7) == ANVIL_OK);
        const BenchmarkParticipant* p = f.participant();
        CHECK(p->values[0] == BenchmarkValue(BenchmarkOption{2}));
        CHECK(p->values[1] == BenchmarkValue(int64_t{11525}));
        CHECK(p->values[2] == BenchmarkValue(91.5));
        CHECK(p->values[3] == BenchmarkValue(true));
        CHECK(f.warnings.empty());

        CHECK(f.s.set_int(&f.plugin, "nope", 1) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(f.s.set_int(&f.plugin, "open_warmup.time", 1) == ANVIL_ERROR_WRONG_KIND);
        CHECK(f.s.set_flag(&f.plugin, "open_warmup.opened", 1) == ANVIL_ERROR_WRONG_KIND);
        CHECK(f.s.set_number(&f.plugin, "open_warmup.time", std::nan("")) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.s.set_number(&f.plugin, "open_warmup.time", std::numeric_limits<double>::infinity()) ==
              ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.s.set_enum(&f.plugin, "open_warmup.mode", "fast") == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.s.set_enum(&f.plugin, "open_warmup.mode", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.s.set_int(&f.plugin, nullptr, 1) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.participant()->values[2] == BenchmarkValue(91.5));  // refused values change nothing
        const size_t warned = f.warnings.size();
        CHECK(warned == 6);  // NaN and infinity, and both bad options, are one refusal each
        // The same refusal again: no second warning (set calls may run every frame).
        CHECK(f.s.set_int(&f.plugin, "nope", 1) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(f.s.set_int(&f.plugin, "open_warmup.time", 2) == ANVIL_ERROR_WRONG_KIND);
        CHECK(f.warnings.size() == warned);
        CHECK(f.warnings[0] == "bellows: anvil.benchmark set_int nope refused: no such field");

        // A plugin that never declared.
        AnvilPlugin stranger;
        stranger.name = "stranger";
        CHECK(f.s.set_int(&stranger, "open_warmup.opened", 1) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(f.s.done(&stranger) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(f.s.learning(&stranger, "new_mod_set") == ANVIL_ERROR_NOT_DECLARED);
        CHECK(f.s.open_cost(&stranger, 1, 1.0) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(f.warnings.back().find("declared no benchmark fields") != std::string::npos);
        CHECK(f.s.set_int(nullptr, "x", 1) == ANVIL_ERROR_INVALID_ARGUMENT);
    });

    run("benchmark: done, learning, open_cost", [] {
        Fixture f;
        const auto fields = some_fields();
        CHECK(f.s.declare(&f.plugin, fields.data(), 4) == ANVIL_OK);
        CHECK(f.s.learning(&f.plugin, "new_mod_set") == ANVIL_OK);
        CHECK(f.s.learning(&f.plugin, "New mod set") == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.s.open_cost(&f.plugin, 11525, 9100.0) == ANVIL_OK);
        CHECK(f.s.open_cost(&f.plugin, 11525, -1.0) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.s.open_cost(&f.plugin, 11525, std::nan("")) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(f.s.open_cost(&f.plugin, 11500, 95.0) == ANVIL_OK);  // the last one wins
        CHECK(f.s.done(&f.plugin) == ANVIL_OK);
        CHECK(f.s.done(&f.plugin) == ANVIL_OK);  // again: nothing changes
        const BenchmarkParticipant* p = f.participant();
        CHECK(p->done && p->learning == "new_mod_set");
        CHECK(p->open_cost && p->open_cost->files == 11500 && p->open_cost->ms == 95.0);
        // Nothing changes after done().
        CHECK(f.s.set_int(&f.plugin, "open_warmup.opened", 1) == ANVIL_ERROR_WRONG_PHASE);
        CHECK(f.s.learning(&f.plugin, "late") == ANVIL_ERROR_WRONG_PHASE);
        CHECK(f.s.open_cost(&f.plugin, 1, 1.0) == ANVIL_ERROR_WRONG_PHASE);
        p = f.participant();
        CHECK(std::holds_alternative<std::monostate>(p->values[1]) && p->learning == "new_mod_set");
        CHECK(f.warnings.back().find("already called done()") != std::string::npos);
    });

    run("benchmark: outside a benchmark start the calls check the same", [] {
        Fixture f(false);
        const auto fields = some_fields();
        CHECK(f.s.active() == 0);
        CHECK(f.s.declare(&f.plugin, fields.data(), 4) == ANVIL_OK);
        CHECK(f.s.set_int(&f.plugin, "open_warmup.opened", 3) == ANVIL_OK);
        CHECK(f.s.set_int(&f.plugin, "open_warmup.time", 3) == ANVIL_ERROR_WRONG_KIND);
        CHECK(f.s.done(&f.plugin) == ANVIL_OK);
    });

    run("benchmark: the game's first opens", [] {
        CHECK(open_path_key(L"C:\\Game\\Common\\x.txt") == open_path_key(L"c:/game/common/X.TXT"));
        CHECK(open_path_key(L"C:\\Game\\a.txt") != open_path_key(L"C:\\Game\\b.txt"));
        CHECK(counts_as_file_open(GENERIC_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL));
        CHECK(counts_as_file_open(FILE_READ_DATA | SYNCHRONIZE, OPEN_EXISTING, 0));
        CHECK(!counts_as_file_open(GENERIC_WRITE, OPEN_EXISTING, 0));
        CHECK(!counts_as_file_open(GENERIC_READ, OPEN_ALWAYS, 0));
        CHECK(!counts_as_file_open(GENERIC_READ, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS));  // a directory
        FirstOpens opens;
        opens.add(L"C:\\Game\\a.txt", 300);
        opens.add(L"c:/game/A.TXT", 5000);  // opened again: not a first open
        opens.add(L"C:\\Game\\b.txt", 700);
        BenchmarkOpenCost total = opens.total(1'000'000);
        CHECK(total.files == 2 && total.ms == 1.0);
        opens.stop();
        opens.add(L"C:\\Game\\c.txt", 1000);
        CHECK(opens.total(1'000'000).files == 2 && !opens.counting());
    });

    run("benchmark: start history and the first start since boot", [] {
        const uint64_t minute = 60ull * 1000 * 10'000;  // FILETIME units
        const StartHistory earlier = current_start_history(1000 * minute, 30ull * 60 * 1000);  // 30 min after boot
        CHECK(earlier.boot_filetime == 970 * minute);
        const StartHistory later = current_start_history(1060 * minute, 90ull * 60 * 1000);
        CHECK(!rebooted_since(earlier, later));
        // A clock adjustment of one minute is no reboot; ten minutes are.
        CHECK(!rebooted_since(earlier, current_start_history(1061 * minute, 90ull * 60 * 1000)));
        CHECK(rebooted_since(earlier, current_start_history(1070 * minute, 90ull * 60 * 1000)));
        CHECK(rebooted_since(earlier, current_start_history(1060 * minute, 5ull * 60 * 1000)));  // ticks went back

        const fs::path dir = fs::temp_directory_path() / ("anvil_history_" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(dir);
        std::string error;
        CHECK(write_file_atomic(dir / "last-start.txt", start_history_text(later), error));
        const auto back = read_start_history(dir / "last-start.txt");
        CHECK(back && back->boot_filetime == later.boot_filetime && back->tick_ms == later.tick_ms &&
              back->entry_filetime == later.entry_filetime);
        CHECK(write_file_atomic(dir / "last-start.txt", "boot_filetime=1\n", error));
        CHECK(!read_start_history(dir / "last-start.txt"));  // incomplete
        CHECK(!read_start_history(dir / "missing.txt"));
        fs::remove_all(dir);
    });

    run("benchmark: the start record as JSON", [] {
        BenchmarkRecord r;
        r.anvil_version = "1.4.0";
        r.arm = BenchmarkArm{false, "bellows"};
        r.process_id = 77;
        r.qpc_frequency = 10'000'000;
        r.entry_qpc = 5'000'000;
        r.ready_qpc = 5'000'000 + 400'000'000;  // 40 s
        r.creation_filetime = 1'000'000;
        r.entry_filetime = 1'000'000 + 1'500'000;  // 150 ms
        r.plugins.push_back({"anvil-game", "1.4.0", Outcome::Kind::loaded});
        r.plugins.push_back({"bellows", "", Outcome::Kind::disabled, true});
        r.plugins.push_back({"broken", "", Outcome::Kind::skipped, false, "verification"});
        r.game_first_opens = BenchmarkOpenCost{11502, 460.25};
        r.first_start_since_boot = true;
        r.input_during_start = false;
        const std::string json = benchmark_record_json(r);
        CHECK(json.starts_with("{\"format\":1,\"write\":\"ready\",\"anvil\":\"1.4.0\","
                               "\"arm\":{\"with\":false,\"plugin\":\"bellows\"},\"process_id\":77,"));
        CHECK(json.find("\"creation_to_entry_ms\":150,\"entry_to_ready_ms\":40000,\"time_ms\":40150}") != std::string::npos);
        CHECK(json.find("{\"id\":\"anvil-game\",\"version\":\"1.4.0\",\"state\":\"loaded\",\"participant\":false}") !=
              std::string::npos);
        CHECK(json.find("{\"id\":\"bellows\",\"state\":\"disabled\",\"by\":\"arm\",\"participant\":false}") != std::string::npos);
        CHECK(json.find("{\"id\":\"broken\",\"state\":\"skipped\",\"reason\":\"verification\",\"participant\":false}") !=
              std::string::npos);
        CHECK(json.find("\"game_first_opens\":{\"files\":11502,\"ms\":460.25}") != std::string::npos);
        CHECK(json.find("\"labels\":{\"first_start_since_boot\":true,\"definitions_changed\":null},"
                        "\"input_during_start\":false}") != std::string::npos);
        r.ready_qpc = 0;
        CHECK(benchmark_record_json(r).find("\"ready_qpc\":null") != std::string::npos);
        // Defender's log: a time, 0 or unreadable, but never a crash.
        const auto update = newest_definitions_update();
        std::printf("  newest definitions update: %s\n", update ? std::to_string(*update).c_str() : "unreadable");
    });

    run("benchmark: a plugin that failed to load is forgotten", [] {
        Fixture f;
        AnvilPlugin other;
        other.name = "other";
        other.loading = true;
        CHECK(f.s.declare(&f.plugin, nullptr, 0) == ANVIL_OK);
        CHECK(f.s.declare(&other, nullptr, 0) == ANVIL_OK);
        f.bench.forget(&f.plugin);
        const auto left = f.bench.participants();
        CHECK(left.size() == 1 && left[0].id == "other");
    });
}
