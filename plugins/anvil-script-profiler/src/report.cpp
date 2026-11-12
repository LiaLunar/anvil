#include "report.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>

namespace script_profiler {

namespace {

double ms_of(std::uint64_t ticks, double per_second) {
    return per_second > 0 ? 1000.0 * static_cast<double>(ticks) / per_second : 0.0;
}

std::string fmt(const char* pattern, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), pattern, v);
    return buf;
}

std::string csv_field(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += (c == '\r' || c == '\n') ? ' ' : c;
    }
    return out + "\"";
}

const char* ending_key(Ending e) {
    switch (e) {
    case Ending::complete: return "complete";
    case Ending::interrupted_session: return "interrupted_session";
    case Ending::interrupted_exit: return "interrupted_exit";
    case Ending::running: break;
    }
    return "running";
}

const char* kind_text(OwnerKind kind) {
    switch (kind) {
    case OwnerKind::event: return N_("Event");
    case OwnerKind::event_option: return N_("Event option");
    case OwnerKind::decision: return N_("Decision");
    case OwnerKind::on_action: return N_("On action");
    case OwnerKind::casus_belli: return N_("Casus belli");
    case OwnerKind::country_modifier: return N_("Triggered modifier");
    case OwnerKind::province_modifier: return N_("Province triggered modifier");
    case OwnerKind::mission: return N_("Mission");
    case OwnerKind::church_aspect: return N_("Church aspect");
    case OwnerKind::scripted_trigger: return N_("Scripted trigger");
    case OwnerKind::none: break;
    }
    return N_("No owner found");
}

}  // namespace

std::string format_ms(double ms) {
    if (ms >= 1000.0) return fmt("%.2f s", ms / 1000.0);
    if (ms >= 10.0) return fmt("%.1f ms", ms);
    if (ms >= 1.0) return fmt("%.2f ms", ms);
    const double us = ms * 1000.0;
    return us >= 10.0 ? fmt("%.0f \xC2\xB5s", us) : fmt("%.2f \xC2\xB5s", us);
}

std::string format_count(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), " ");
    return s;
}

Report make_report(const Snapshot& snapshot, Resolver& resolver, const Span& span, bool split_unowned,
                   bool read_memory) {
    Report report;
    report.span = span;
    report.nested_triggers = snapshot.nested_triggers;
    report.nested_effects = snapshot.nested_effects;
    report.nested_scripted = snapshot.nested_scripted;
    report.dropped_calls = snapshot.dropped_calls;
    report.threads = snapshot.threads;
    // (list, owner object or caller, role, name) -> row; a scripted trigger merges by its name,
    // since each use of one in a script is an object of its own.
    std::map<std::tuple<Kind, const void*, std::uintptr_t, std::string, std::string>, Row> merged;
    for (const RootStat& r : snapshot.roots) {
        report.root_calls += r.calls;
        const Owner& o = resolver.owner_of(r.block, r.kind, read_memory);
        const bool owned = o.kind != OwnerKind::none;
        const std::uintptr_t by_caller = !owned && split_unowned ? r.caller : 0;
        const void* object = owned && o.kind != OwnerKind::scripted_trigger ? o.object : nullptr;
        auto [it, fresh] = merged.try_emplace({r.kind, object, by_caller, owned ? o.role : "", owned ? o.name : ""});
        Row& row = it->second;
        if (fresh) {
            row.list = r.kind;
            row.kind = o.kind;
            row.object = o.object;
            row.name = o.name;
            row.option = o.option;
            row.role = owned ? o.role : (r.kind == Kind::effect ? "effect" : "trigger");
            row.caller = owned ? 0 : r.caller;
        }
        row.calls += r.calls;
        row.ticks += r.ticks;
        row.main_ticks += r.main_ticks;
        row.max = std::max(row.max, r.max);
    }
    for (auto& [key, row] : merged) {
        (row.list == Kind::effect ? report.effects : row.list == Kind::scripted ? report.scripted : report.triggers)
            .push_back(std::move(row));
    }
    const auto longest = [](const Row& a, const Row& b) {
        return a.ticks != b.ticks ? a.ticks > b.ticks : a.calls > b.calls;
    };
    std::sort(report.triggers.begin(), report.triggers.end(), longest);
    std::sort(report.effects.begin(), report.effects.end(), longest);
    std::sort(report.scripted.begin(), report.scripted.end(), longest);
    return report;
}

std::string csv_name(int year, int month, int day, int hour, int minute, int second) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "script_profile_%04d-%02d-%02d_%02d%02d%02d.csv", year, month, day, hour, minute,
                  second);
    return buf;
}

std::string csv(const Report& r) {
    const Span& s = r.span;
    const double f = s.ticks_per_second;
    std::string out;
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "# format=1 plugin=anvil-script-profiler version=%s build=%s status=%s started=%s first_day=%s "
                  "last_day=%s days=%u wall_s=%.3f threads=%u root_calls=%llu nested_triggers=%llu "
                  "nested_effects=%llu nested_scripted_triggers=%llu dropped_calls=%llu multiplayer=%d ironman=%d\n",
                  s.anvil_version.c_str(), s.game_build.c_str(), ending_key(s.ending), s.started_at.c_str(),
                  s.first_date.c_str(), s.last_date.c_str(), s.days, s.seconds, r.threads,
                  static_cast<unsigned long long>(r.root_calls), static_cast<unsigned long long>(r.nested_triggers),
                  static_cast<unsigned long long>(r.nested_effects), static_cast<unsigned long long>(r.nested_scripted),
                  static_cast<unsigned long long>(r.dropped_calls),
                  s.multiplayer ? 1 : 0, s.ironman ? 1 : 0);
    out += buf;
    out += "# Root blocks only (the outermost block on a thread), merged by owner and role; times summed over all "
           "threads, main_thread_ms is the part on the game's main thread. A trigger root inside an effect counts in "
           "both lists. scripted_trigger rows: every call of a scripted trigger by name, the outermost on its thread, also "
           "inside other blocks, so their time is in the trigger list too. caller_rva: for blocks without an owner, "
           "where eu4.exe called them.\n";
    out += "list,rank,owner,name,option,role,calls,total_ms,mean_us,max_us,main_thread_ms,caller_rva\n";
    for (const std::vector<Row>* list : {&r.triggers, &r.effects, &r.scripted}) {
        std::size_t rank = 0;
        for (const Row& row : *list) {
            const double ms = ms_of(row.ticks, f);
            char rva[16] = "";
            if (row.kind == OwnerKind::none) std::snprintf(rva, sizeof(rva), "0x%X", row.caller);
            std::snprintf(buf, sizeof(buf), "%s,%zu,%s,%s,%s,%s,%llu,%.4f,%.3f,%.3f,%.4f,%s\n",
                          row.list == Kind::effect ? "effect" : row.list == Kind::scripted ? "scripted_trigger" : "trigger", ++rank, kind_key(row.kind), csv_field(row.name).c_str(),
                          row.option > 0 ? std::to_string(row.option).c_str() : "", csv_field(row.role).c_str(),
                          static_cast<unsigned long long>(row.calls), ms,
                          row.calls ? 1000.0 * ms / static_cast<double>(row.calls) : 0.0,
                          1000.0 * ms_of(row.max, f), ms_of(row.main_ticks, f), rva);
            out += buf;
        }
    }
    return out;
}

std::vector<PanelRow> panel_rows(const PanelState& st, const Report* last, const Texts& t) {
    std::vector<PanelRow> rows;
    const auto text = [&](std::string label, std::string value = {}, bool has_value = true) {
        PanelRow r;
        r.kind = 1;
        r.label = std::move(label);
        r.has_value = has_value && !value.empty();
        r.value = std::move(value);
        rows.push_back(std::move(r));
    };
    const auto button = [&](std::string label, std::uint64_t id) {
        PanelRow r;
        r.kind = 2;
        r.label = std::move(label);
        r.id = id;
        rows.push_back(std::move(r));
    };
    const auto separator = [&] {
        PanelRow r;
        r.kind = 3;
        rows.push_back(std::move(r));
    };
    const std::string seconds = fmt("%.1f", st.seconds_so_far);
    if (st.measuring) {
        text(t("State"), st.days_to_measure > 0
                             ? t.f("Measuring: {0} of {1} days, {2} s",
                                   {std::to_string(st.days_so_far), std::to_string(st.days_to_measure), seconds})
                             : t.f("Measuring: {0} days, {1} s", {std::to_string(st.days_so_far), seconds}));
    } else {
        text(t("State"), t("Not measuring: nothing in the game is hooked."));
    }
    if (st.multiplayer) {
        text(t("In multiplayer the measurement slows your game down, and every player waits for the slowest."));
    }
    if (st.measuring) {
        button(t("Stop"), kStop);
    } else if (st.in_session) {
        button(t("Start"), kStart);
        button(t.f("Measure {0} days", {std::to_string(st.measure_days)}), kMeasureDays);
    } else {
        text(t("Load a game to measure."));
    }
    if (last == nullptr) {
        if (!st.measuring) {
            text(t("Times the game's triggers and effects by the event, decision, on_action, casus belli, triggered "
                   "modifier or mission they belong to. The game runs slower while it measures."));
        }
        return rows;
    }
    separator();
    const Span& s = last->span;
    const std::string wall = fmt("%.1f", s.seconds);
    std::string what = s.days > 0 ? t.f("{0} days ({1} to {2}), {3} s", {std::to_string(s.days), s.first_date,
                                                                          s.last_date, wall})
                                  : t.f("no day passed, {0} s", {wall});
    text(t("Last measurement"), what);
    if (s.ending == Ending::interrupted_session) {
        text(t("Interrupted: the game was loaded or left. The results are what was measured until then."));
    }
    if (!st.csv_path.empty()) {
        text(st.csv_failed ? t.f("Could not write {0}", {st.csv_path}) : t.f("Saved to {0}", {st.csv_path}));
    }
    const double f = s.ticks_per_second;
    for (const std::vector<Row>* list : {&last->triggers, &last->scripted, &last->effects}) {
        const bool effects = list == &last->effects;
        const bool scripted = list == &last->scripted;
        if (scripted && list->empty()) continue;
        std::uint64_t ticks = 0, calls = 0;
        for (const Row& r : *list) {
            ticks += r.ticks;
            calls += r.calls;
        }
        separator();
        text(effects ? t("Effects") : scripted ? t("Scripted triggers") : t("Triggers"),
             t.f("{0} in {1} calls", {format_ms(ms_of(ticks, f)), format_count(calls)}));
        const std::size_t n = std::min(list->size(), st.top);
        for (std::size_t i = 0; i < n; ++i) {
            const Row& r = (*list)[i];
            std::string label = t(kind_text(r.kind));
            if (r.kind != OwnerKind::none) {
                label += " " + (r.name.empty() ? std::string("?") : r.name);
                if (r.option > 0) label += " #" + std::to_string(r.option);
            }
            if (!r.role.empty()) label += " \xC2\xB7 " + r.role;
            const double ms = ms_of(r.ticks, f);
            text(std::move(label),
                 t.f("{0}, {1} calls, mean {2}, max {3}",
                     {format_ms(ms), format_count(r.calls),
                      format_ms(r.calls ? ms / static_cast<double>(r.calls) : 0.0), format_ms(ms_of(r.max, f))}));
        }
    }
    return rows;
}

}  // namespace script_profiler
