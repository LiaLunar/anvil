#include "benchmark_record.hpp"

#include "plugin_set.hpp"

#include <windows.h>
#include <winevt.h>

#include <charconv>
#include <cstdio>
#include <fstream>

namespace anvil {
namespace {

constexpr uint64_t kTicksPerMs = 10'000;           // FILETIME units
constexpr uint64_t kBootTolerance = 2 * 60 * 1000;  // ms

// ---- JSON ------------------------------------------------------------------------

void put_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", u);
            out += escape;
        } else {
            out += c;
        }
    }
    out += '"';
}

void put_number(std::string& out, double value) {
    char text[32];
    const auto r = std::to_chars(text, text + sizeof(text), value);
    out.append(text, r.ptr);
}

template <class T> void put_integer(std::string& out, T value) { out += std::to_string(value); }

void put_bool(std::string& out, std::optional<bool> value) { out += !value ? "null" : *value ? "true" : "false"; }

const char* kind_text(AnvilBenchmarkKind kind) {
    switch (kind) {
    case ANVIL_BENCHMARK_INT: return "int";
    case ANVIL_BENCHMARK_NUMBER: return "number";
    case ANVIL_BENCHMARK_ENUM: return "enum";
    case ANVIL_BENCHMARK_FLAG: return "flag";
    }
    return "int";
}

const char* unit_text(AnvilBenchmarkUnit unit) {
    switch (unit) {
    case ANVIL_BENCHMARK_UNIT_NONE: return nullptr;
    case ANVIL_BENCHMARK_UNIT_MS: return "ms";
    case ANVIL_BENCHMARK_UNIT_COUNT: return "count";
    case ANVIL_BENCHMARK_UNIT_BYTES: return "bytes";
    case ANVIL_BENCHMARK_UNIT_MB: return "mb";
    case ANVIL_BENCHMARK_UNIT_PERCENT: return "percent";
    }
    return nullptr;
}

void put_open_cost(std::string& out, const std::optional<BenchmarkOpenCost>& cost) {
    if (!cost) {
        out += "null";
        return;
    }
    out += "{\"files\":";
    put_integer(out, cost->files);
    out += ",\"ms\":";
    put_number(out, cost->ms);
    out += '}';
}

void put_value(std::string& out, const BenchmarkField& field, const BenchmarkValue& value) {
    if (const auto* i = std::get_if<int64_t>(&value)) {
        put_integer(out, *i);
    } else if (const auto* d = std::get_if<double>(&value)) {
        put_number(out, *d);
    } else if (const auto* o = std::get_if<BenchmarkOption>(&value)) {
        put_string(out, o->index < field.options.size() ? field.options[o->index] : std::string());
    } else if (const auto* b = std::get_if<bool>(&value)) {
        out += *b ? "true" : "false";
    } else {
        out += "null";
    }
}

void put_participant(std::string& out, const BenchmarkParticipant& p) {
    out += ",\"participant\":true,\"done\":";
    out += p.done ? "true" : "false";
    out += ",\"learning\":";
    if (p.learning.empty()) {
        out += "null";
    } else {
        put_string(out, p.learning);
    }
    out += ",\"open_cost\":";
    put_open_cost(out, p.open_cost);
    out += ",\"schema\":[";
    for (size_t i = 0; i < p.fields.size(); ++i) {
        const BenchmarkField& f = p.fields[i];
        out += i ? ",{\"name\":" : "{\"name\":";
        put_string(out, f.name);
        out += ",\"kind\":";
        put_string(out, kind_text(f.kind));
        out += ",\"unit\":";
        if (const char* unit = unit_text(f.unit)) {
            put_string(out, unit);
        } else {
            out += "null";
        }
        out += ",\"label\":";
        if (f.label.empty()) {
            out += "null";
        } else {
            put_string(out, f.label);
        }
        if (f.kind == ANVIL_BENCHMARK_ENUM) {
            out += ",\"options\":[";
            for (size_t k = 0; k < f.options.size(); ++k) {
                if (k) out += ',';
                put_string(out, f.options[k]);
            }
            out += ']';
        }
        out += '}';
    }
    out += "],\"fields\":{";
    for (size_t i = 0; i < p.fields.size(); ++i) {
        if (i) out += ',';
        put_string(out, p.fields[i].name);
        out += ':';
        put_value(out, p.fields[i], i < p.values.size() ? p.values[i] : BenchmarkValue{});
    }
    out += '}';
}

double ms_between(int64_t from, int64_t to, int64_t frequency) {
    return frequency > 0 ? static_cast<double>(to - from) * 1000.0 / static_cast<double>(frequency) : 0.0;
}

// ---- Defender's event log, loaded only when asked ---------------------------------

struct EventLog {
    decltype(&EvtQuery) query = nullptr;
    decltype(&EvtNext) next = nullptr;
    decltype(&EvtCreateRenderContext) context = nullptr;
    decltype(&EvtRender) render = nullptr;
    decltype(&EvtClose) close = nullptr;

    bool load() {
        HMODULE m = LoadLibraryExW(L"wevtapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m == nullptr) return false;
        query = reinterpret_cast<decltype(query)>(GetProcAddress(m, "EvtQuery"));
        next = reinterpret_cast<decltype(next)>(GetProcAddress(m, "EvtNext"));
        context = reinterpret_cast<decltype(context)>(GetProcAddress(m, "EvtCreateRenderContext"));
        render = reinterpret_cast<decltype(render)>(GetProcAddress(m, "EvtRender"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(m, "EvtClose"));
        return query && next && context && render && close;
    }
};

}  // namespace

StartHistory current_start_history(uint64_t entry_filetime, uint64_t tick_ms) {
    StartHistory h;
    h.entry_filetime = entry_filetime;
    h.tick_ms = tick_ms;
    h.boot_filetime = entry_filetime - tick_ms * kTicksPerMs;
    return h;
}

std::optional<StartHistory> read_start_history(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    StartHistory h;
    int found = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        uint64_t value = 0;
        const std::string_view text = std::string_view(line).substr(eq + 1);
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc()) continue;
        const std::string_view key = std::string_view(line).substr(0, eq);
        if (key == "boot_filetime") h.boot_filetime = value, found |= 1;
        if (key == "tick_ms") h.tick_ms = value, found |= 2;
        if (key == "entry_filetime") h.entry_filetime = value, found |= 4;
    }
    if (found != 7) return std::nullopt;
    return h;
}

std::string start_history_text(const StartHistory& h) {
    return "# Written by Anvil at every start, for the start-up benchmark's cold-start labels.\n"
           "boot_filetime=" + std::to_string(h.boot_filetime) + "\ntick_ms=" + std::to_string(h.tick_ms) +
           "\nentry_filetime=" + std::to_string(h.entry_filetime) + "\n";
}

bool rebooted_since(const StartHistory& previous, const StartHistory& now) {
    if (now.tick_ms < previous.tick_ms) return true;
    const uint64_t a = previous.boot_filetime, b = now.boot_filetime;
    return (a > b ? a - b : b - a) > kBootTolerance * kTicksPerMs;
}

std::optional<uint64_t> newest_definitions_update() {
    EventLog log;
    if (!log.load()) return std::nullopt;
    const EVT_HANDLE results = log.query(nullptr, L"Microsoft-Windows-Windows Defender/Operational",
                                         L"*[System[EventID=2000]]", EvtQueryChannelPath | EvtQueryReverseDirection);
    if (results == nullptr) return std::nullopt;
    std::optional<uint64_t> newest;
    EVT_HANDLE event = nullptr;
    DWORD returned = 0;
    if (!log.next(results, 1, &event, 2000, 0, &returned)) {
        if (GetLastError() == ERROR_NO_MORE_ITEMS) newest = 0;
    } else if (const EVT_HANDLE context = log.context(0, nullptr, EvtRenderContextSystem)) {
        DWORD used = 0, count = 0;
        log.render(context, event, EvtRenderEventValues, 0, nullptr, &used, &count);
        std::vector<uint8_t> buffer(used);
        if (used > 0 && log.render(context, event, EvtRenderEventValues, used, buffer.data(), &used, &count) &&
            count > EvtSystemTimeCreated) {
            const auto* values = reinterpret_cast<const EVT_VARIANT*>(buffer.data());
            if (values[EvtSystemTimeCreated].Type == EvtVarTypeFileTime) newest = values[EvtSystemTimeCreated].FileTimeVal;
        }
        log.close(context);
    }
    if (event != nullptr) log.close(event);
    log.close(results);
    return newest;
}

std::vector<BenchmarkRecordPlugin> record_plugins(const PluginSet& plugins, std::vector<BenchmarkParticipant> participants) {
    std::vector<BenchmarkRecordPlugin> out;
    for (const PluginRecord& p : plugins) {
        BenchmarkRecordPlugin r;
        r.id = p.id();
        const Outcome* o = p.outcome();
        if (o != nullptr) r.outcome = o->kind;
        if (p.handle() != nullptr) {
            r.version = p.manifest().version.to_string();
            for (auto& joined : participants) {
                if (joined.plugin == p.handle()) r.participant = std::move(joined);
            }
        } else {
            r.disabled_by_arm = o->by_benchmark_arm;
            r.skip_reason = skip_kind_name(o->skip);
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::string benchmark_record_json(const BenchmarkRecord& r) {
    std::string out = "{\"format\":1,\"write\":";
    put_string(out, r.write);
    out += ",\"anvil\":";
    put_string(out, r.anvil_version);
    out += ",\"arm\":{\"with\":";
    out += r.arm.with ? "true" : "false";
    out += ",\"plugin\":";
    put_string(out, r.arm.plugin);
    out += "},\"process_id\":";
    put_integer(out, r.process_id);

    out += ",\n\"timing\":{\"qpc_frequency\":";
    put_integer(out, r.qpc_frequency);
    out += ",\"entry_qpc\":";
    put_integer(out, r.entry_qpc);
    out += ",\"ready_qpc\":";
    if (r.ready_qpc != 0) {
        put_integer(out, r.ready_qpc);
    } else {
        out += "null";
    }
    out += ",\"creation_filetime\":";
    put_integer(out, r.creation_filetime);
    out += ",\"entry_filetime\":";
    put_integer(out, r.entry_filetime);
    const double creation_to_entry =
        r.entry_filetime >= r.creation_filetime
            ? static_cast<double>(r.entry_filetime - r.creation_filetime) / static_cast<double>(kTicksPerMs)
            : 0.0;
    out += ",\"creation_to_entry_ms\":";
    put_number(out, creation_to_entry);
    out += ",\"entry_to_ready_ms\":";
    if (r.ready_qpc != 0) {
        put_number(out, ms_between(r.entry_qpc, r.ready_qpc, r.qpc_frequency));
    } else {
        out += "null";
    }
    out += ",\"time_ms\":";
    if (r.ready_qpc != 0) {
        put_number(out, creation_to_entry + ms_between(r.entry_qpc, r.ready_qpc, r.qpc_frequency));
    } else {
        out += "null";
    }
    out += '}';

    out += ",\n\"plugins\":[";
    for (size_t i = 0; i < r.plugins.size(); ++i) {
        const BenchmarkRecordPlugin& p = r.plugins[i];
        out += i ? ",\n{\"id\":" : "\n{\"id\":";
        put_string(out, p.id);
        if (!p.version.empty()) {
            out += ",\"version\":";
            put_string(out, p.version);
        }
        out += ",\"state\":";
        put_string(out, p.outcome ? outcome_name(*p.outcome) : "pending");
        if (p.outcome == Outcome::Kind::disabled) out += p.disabled_by_arm ? ",\"by\":\"arm\"" : ",\"by\":\"player\"";
        if (p.outcome == Outcome::Kind::skipped) {
            out += ",\"reason\":";
            put_string(out, p.skip_reason);
        }
        if (p.participant) {
            put_participant(out, *p.participant);
        } else {
            out += ",\"participant\":false";
        }
        out += '}';
    }
    out += "],\n\"game_first_opens\":";
    put_open_cost(out, r.game_first_opens);
    out += ",\n\"labels\":{\"first_start_since_boot\":";
    put_bool(out, r.first_start_since_boot);
    out += ",\"definitions_changed\":";
    put_bool(out, r.definitions_changed);
    out += "},\"input_during_start\":";
    put_bool(out, r.input_during_start);
    out += "}\n";
    return out;
}

}  // namespace anvil
