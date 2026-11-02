#include "benchmark_series.hpp"
#include "i18n/translate.hpp"

#include <algorithm>
#include <cmath>

namespace anvil::manager {
namespace {

std::optional<bool> optional_bool(const Json& v) {
    if (v.is_bool()) return v.as_bool();
    return std::nullopt;
}

std::string text(const Json& v) { return v.is_string() ? v.as_string() : std::string(); }

bool read_open_cost(const Json& v, std::optional<OpenCost>& out) {
    if (v.is_null()) return true;
    if (!v.is_object() || !v["files"].is_integer() || v["files"].as_integer() < 0 || !v["ms"].is_number()) return false;
    out = OpenCost{static_cast<uint64_t>(v["files"].as_integer()), v["ms"].as_number()};
    return true;
}

bool read_plugin(const Json& v, RecordPlugin& p) {
    if (!v.is_object() || !v["id"].is_string() || !v["state"].is_string()) return false;
    p.id = v["id"].as_string();
    p.version = text(v["version"]);
    p.state = v["state"].as_string();
    p.by = text(v["by"]);
    p.reason = text(v["reason"]);
    p.participant = v["participant"].is_bool() && v["participant"].as_bool();
    if (!p.participant) return true;
    p.done = v["done"].is_bool() && v["done"].as_bool();
    p.learning = text(v["learning"]);
    if (!read_open_cost(v["open_cost"], p.open_cost)) return false;
    if (!v["schema"].is_array() || !v["fields"].is_object()) return false;
    for (const Json& f : v["schema"].items()) {
        if (!f.is_object() || !f["name"].is_string() || !f["kind"].is_string()) return false;
        RecordField field;
        field.name = f["name"].as_string();
        field.kind = f["kind"].as_string();
        field.unit = text(f["unit"]);
        field.label = text(f["label"]);
        for (const Json& o : f["options"].items()) {
            if (!o.is_string()) return false;
            field.options.push_back(o.as_string());
        }
        p.schema.push_back(std::move(field));
    }
    p.fields = v["fields"];
    return true;
}

double per_file(const StartOpenCost& cost) { return cost.ms / static_cast<double>(cost.files); }

double median(std::vector<int64_t> times) {
    std::sort(times.begin(), times.end());
    const size_t mid = times.size() / 2;
    return times.size() % 2 ? static_cast<double>(times[mid]) : (static_cast<double>(times[mid - 1]) + times[mid]) / 2.0;
}

// Decimal places: dividing by a power of ten gives the double nearest to the
// decimal, so it is written without a tail of digits.
double round_places(double value, double scale) { return std::round(value * scale) / scale; }

Difference difference(double without, double with) {
    Difference d;
    d.ms = with - without;
    d.percent = without > 0 ? round_places(d.ms / without * 100.0, 100.0) : 0.0;
    return d;
}

// The stop reason of a discarded start that ends the series by itself.
std::string stop_for(const std::string& discard) {
    if (discard == "safe_mode") return "safe_mode";
    if (discard == "arm_mismatch") return "plugin_not_loaded";
    if (discard == "not_participant") return "plugin_not_participant";
    if (discard == "timeout") return "timeout";
    if (discard == "environment_changed") return "environment_changed";
    return "";
}

}  // namespace

const char* arm_name(Arm arm) { return arm == Arm::Without ? "without" : "with"; }

const RecordPlugin* StartRecord::plugin(std::string_view id) const {
    for (const RecordPlugin& p : plugins) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::optional<StartRecord> parse_start_record(std::string_view json, std::string& error) {
    const auto doc = parse_json(json, error);
    if (!doc) return std::nullopt;
    const auto bad = [&](const char* why) -> std::optional<StartRecord> {
        error = why;
        return std::nullopt;
    };
    const Json& d = *doc;
    if (!d.is_object() || !d["format"].is_integer() || d["format"].as_integer() != 1) return bad("not a start record of format 1");
    StartRecord r;
    r.write = text(d["write"]);
    r.anvil = text(d["anvil"]);
    const Json& arm = d["arm"];
    if (!arm["with"].is_bool() || !arm["plugin"].is_string()) return bad("the record has no arm");
    r.arm = arm["with"].as_bool() ? Arm::With : Arm::Without;
    r.arm_plugin = arm["plugin"].as_string();
    if (!d["process_id"].is_integer()) return bad("the record has no process id");
    r.process_id = static_cast<uint32_t>(d["process_id"].as_integer());
    const Json& timing = d["timing"];
    if (!timing["creation_filetime"].is_integer()) return bad("the record has no creation time");
    r.creation_filetime = static_cast<uint64_t>(timing["creation_filetime"].as_integer());
    if (timing["time_ms"].is_number()) r.time_ms = timing["time_ms"].as_number();
    if (!d["plugins"].is_array()) return bad("the record has no plugins");
    for (const Json& p : d["plugins"].items()) {
        RecordPlugin plugin;
        if (!read_plugin(p, plugin)) return bad("a plugin entry of the record is not readable");
        r.plugins.push_back(std::move(plugin));
    }
    if (!read_open_cost(d["game_first_opens"], r.game_first_opens)) return bad("game_first_opens is not readable");
    r.first_start_since_boot = optional_bool(d["labels"]["first_start_since_boot"]);
    r.definitions_changed = optional_bool(d["labels"]["definitions_changed"]);
    r.input_during_start = optional_bool(d["input_during_start"]);
    return r;
}

std::string open_cost_class(const std::optional<StartOpenCost>& cost) {
    if (!cost || cost->files == 0) return "unclear";
    const double ms = per_file(*cost);
    const double warm = cost->from_plugin ? kWarmPluginOpenMs : kWarmGameOpenMs;
    const double cold = cost->from_plugin ? kColdPluginOpenMs : kColdGameOpenMs;
    return ms <= warm ? "warm" : ms >= cold ? "cold" : "unclear";
}

StartRow classify_start(const StartObservation& seen, Arm arm, const std::string& plugin, std::string& stop) {
    stop.clear();
    StartRow row;
    row.arm = arm;
    row.crash = seen.crash;
    row.killed = seen.killed || seen.timed_out;
    const StartRecord* record = seen.record ? &*seen.record : nullptr;
    if (record != nullptr) {
        if (record->time_ms) row.time_ms = std::llround(std::max(0.0, *record->time_ms));
        row.first_since_boot = record->first_start_since_boot;
        row.definitions_changed = record->definitions_changed;
        row.input_during_start = record->input_during_start;
        row.plugins = record->plugins;
        // In the arm with the plugin, a participant's open cost replaces the
        // game's first opens: the compared plugin's first.
        const RecordPlugin* opener = nullptr;
        if (arm == Arm::With) {
            const RecordPlugin* compared = record->plugin(plugin);
            if (compared != nullptr && compared->participant && compared->open_cost) opener = compared;
            for (const RecordPlugin& p : record->plugins) {
                if (opener == nullptr && p.participant && p.open_cost) opener = &p;
            }
        }
        if (opener != nullptr) {
            row.open_cost = StartOpenCost{true, opener->id, opener->open_cost->files, opener->open_cost->ms};
        } else if (record->game_first_opens) {
            row.open_cost = StartOpenCost{false, "", record->game_first_opens->files, record->game_first_opens->ms};
        }
    }

    const auto discard = [&](const char* reason) {
        row.valid = false;
        row.discard_reason = reason;
        row.cold_class.clear();
        stop = stop_for(row.discard_reason);
        return row;
    };
    if (seen.environment_changed) return discard("environment_changed");
    if (seen.timed_out) return discard("timeout");
    if (record == nullptr || !row.time_ms) return discard(seen.crash ? "crash" : "no_record");

    const RecordPlugin* compared = record->plugin(plugin);
    if (compared != nullptr && compared->state == "skipped" && compared->reason == "safe_mode") return discard("safe_mode");
    const bool arm_seen = record->arm == arm && record->arm_plugin == plugin;
    if (!arm_seen || compared == nullptr) return discard("arm_mismatch");
    if (arm == Arm::Without && !(compared->state == "disabled" && compared->by == "arm")) return discard("arm_mismatch");
    if (arm == Arm::With && compared->state != "loaded") return discard("arm_mismatch");
    if (arm == Arm::With && !compared->participant) return discard("not_participant");
    for (const RecordPlugin& p : record->plugins) {
        if (p.participant && !p.learning.empty()) return discard("plugin_learning");
    }

    row.valid = true;
    row.cold_class = open_cost_class(row.open_cost);
    // A crash after ready or a kill keeps the time, and ends the series.
    if (seen.crash) stop = "crashed";
    else if (seen.killed) stop = "quit_failed";
    return row;
}

Series::Series(std::string plugin, int target, Arm first) : plugin_(std::move(plugin)), target_(target), first_(first) {
    // A B B A, A B B A, ...: target starts per arm.
    for (int i = 0; i < 2 * target_; ++i) planned_.push_back(i % 4 == 0 || i % 4 == 3 ? first_ : other_arm(first_));
}

int Series::planned_left(Arm arm) const {
    int left = 0;
    for (size_t i = rows_.size(); i < planned_.size(); ++i) left += planned_[i] == arm ? 1 : 0;
    return left;
}

std::optional<Arm> Series::next() const {
    if (over_) return std::nullopt;
    if (rows_.size() < planned_.size()) return planned_[rows_.size()];
    const bool short_without = warm(Arm::Without) < target_;
    const bool short_with = warm(Arm::With) < target_;
    if (short_without && short_with) {
        // The first appended start goes on with the ABBA order, the next ones
        // alternate between the short arms.
        if (rows_.size() == planned_.size()) return rows_.size() % 4 == 0 || rows_.size() % 4 == 3 ? first_ : other_arm(first_);
        return other_arm(rows_.back().arm);
    }
    if (short_without) return Arm::Without;
    if (short_with) return Arm::With;
    return std::nullopt;
}

const StartRow& Series::add(const StartObservation& seen) {
    const Arm arm = next().value_or(first_);
    std::string stop;
    StartRow row = classify_start(seen, arm, plugin_, stop);
    row.position = static_cast<int>(rows_.size()) + 1;
    if (row.discard_reason == "crash" && ++crashes_ >= 2) stop = "crashed";  // a third start in a row would be in safe mode
    if (row.warm()) ++warm_[arm_index(arm)];
    if (stop.empty()) {
        if (row.not_counted() && ++extra_ > kMaxExtraStarts) stop = "too_many_discarded";
        if (row.cold() && ++cold_ > kMaxColdStarts) stop = "too_many_discarded";
    }
    rows_.push_back(std::move(row));
    if (!stop.empty()) {
        over_ = true;
        stop_reason_ = stop;
    } else if (warm(Arm::Without) >= target_ && warm(Arm::With) >= target_) {
        over_ = true;
    }
    return rows_.back();
}

void Series::stop(std::string reason) {
    if (over_) return;
    over_ = true;
    stop_reason_ = std::move(reason);
}

int Series::expected_starts() const {
    if (over_) return static_cast<int>(rows_.size());
    int expected = static_cast<int>(std::max(rows_.size(), planned_.size()));
    for (const Arm arm : {Arm::Without, Arm::With}) expected += std::max(0, target_ - warm(arm) - planned_left(arm));
    return expected;
}

SeriesResult series_result(const Series& series) {
    SeriesResult r;
    std::array<std::vector<int64_t>, 2> cold_times;
    for (const StartRow& row : series.rows()) {
        if (row.warm()) r.warm[arm_index(row.arm)].times_ms.push_back(*row.time_ms);
        if (row.cold()) {
            const std::string kind = row.first_since_boot.value_or(false)      ? "after_boot"
                                     : row.definitions_changed.value_or(false) ? "after_definitions_update"
                                                                               : "open_cost";
            r.cold.push_back(ColdStart{row.position, row.arm, *row.time_ms, kind});
            cold_times[arm_index(row.arm)].push_back(*row.time_ms);
        }
        if (row.not_counted()) r.not_counted.push_back(NotCounted{row.position, row.valid ? "unclear" : row.discard_reason});
        if (row.input_during_start.value_or(false)) r.input_during_start.push_back(row.position);
    }
    for (ArmSummary& arm : r.warm) {
        if (arm.times_ms.empty()) continue;
        arm.median_ms = median(arm.times_ms);
        arm.min_ms = *std::min_element(arm.times_ms.begin(), arm.times_ms.end());
        arm.max_ms = *std::max_element(arm.times_ms.begin(), arm.times_ms.end());
    }
    const ArmSummary& without = r.warm[arm_index(Arm::Without)];
    const ArmSummary& with = r.warm[arm_index(Arm::With)];
    if (without.median_ms && with.median_ms) {
        r.difference = difference(*without.median_ms, *with.median_ms);
        r.difference->ranges_overlap = *with.min_ms <= *without.max_ms && *without.min_ms <= *with.max_ms;
    }
    if (!cold_times[0].empty() && !cold_times[1].empty()) r.cold_difference = difference(median(cold_times[0]), median(cold_times[1]));
    r.extra_starts_used = series.extra_starts_used();

    r.not_measured.push_back("time_to_ready");
    if (cold_times[arm_index(Arm::Without)].empty()) r.not_measured.push_back("no_cold_without");
    if (cold_times[arm_index(Arm::With)].empty()) r.not_measured.push_back("no_cold_with");
    if (series.warm(Arm::Without) < series.target() || series.warm(Arm::With) < series.target()) {
        r.not_measured.push_back("below_target");
    }
    if (!series.stop_reason().empty()) r.not_measured.push_back("stopped");
    return r;
}

std::string stop_reason_text(std::string_view reason) {
    using i18n::tr;
    if (reason == "player") return tr("You stopped the series.");
    if (reason == "too_many_discarded") return tr("Too many starts did not count, more than the extra starts allow.");
    if (reason == "quit_failed") {
        return tr("A start did not quit by itself and had to be ended; later starts would begin from a changed "
                  "settings.txt, so the series ended.");
    }
    if (reason == "crashed") return tr("The game crashed.");
    if (reason == "safe_mode") return tr("Anvil started in safe mode after starts that did not reach the menu.");
    if (reason == "plugin_not_loaded") return tr("The compared plugin did not load as this arm needs (for example it was skipped).");
    if (reason == "plugin_not_participant") {
        return tr("The compared plugin does not take part in the benchmark: update it to a version that does.");
    }
    if (reason == "timeout") return tr("A start did not reach the menu within 10 minutes.");
    if (reason == "environment_changed") return tr("Steam, the mods or the compared plugin changed during the series.");
    return std::string(reason);
}

std::string not_counted_text(std::string_view reason) {
    using i18n::tr;
    if (reason == "unclear") return tr("unclear (open cost between warm and cold)");
    if (reason == "plugin_learning") return tr("the plugin was learning");
    if (reason == "crash") return tr("crashed before the menu");
    if (reason == "safe_mode") return tr("safe mode");
    if (reason == "arm_mismatch") return tr("the plugin did not load as the arm needs");
    if (reason == "not_participant") return tr("the plugin does not take part");
    if (reason == "timeout") return tr("no menu within 10 minutes");
    if (reason == "no_record") return tr("no start record");
    if (reason == "environment_changed") return tr("the environment changed");
    return std::string(reason);
}

std::string cold_kind_text(std::string_view kind) {
    using i18n::tr;
    if (kind == "after_boot") return tr("after boot");
    if (kind == "after_definitions_update") return tr("after a definitions update");
    if (kind == "open_cost") return tr("cold by open cost");
    return std::string(kind);
}

}  // namespace anvil::manager
