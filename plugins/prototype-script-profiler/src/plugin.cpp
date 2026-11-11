// PROTOTYPE (eu4-engine-lab#154), throwaway: branch prototype/script-profiler,
// never merged. A script profiler on the game's trigger and effect blocks.
//
// CAndTrigger::Evaluate (0xB98200) evaluates every AND block: the root of each
// trigger (a CRootTrigger member of the event, decision, modifier...), every
// limit and every AND =. CEffect::Execute (0x5ED800) runs every effect block.
// Both detours keep a per-thread depth: only the outermost block on a thread
// (a root) is timed, inclusive of what it calls; inner blocks are only
// counted. A root is keyed by its address. Its owner is found when the panel
// or the CSV needs it: the block is a member at a known offset of its owner
// (owners.inc, from the GOG PDB layouts), and the owner's RTTI says which
// class sits there. The owner's name is its _DebugKey/_Tag/_Key/_Name.
//
// Modes (setting): 0 no hooks, 1 hooks that only pass on, 2 count roots,
// 3 count and time them (rdtsc). Optionally each log line written while a
// root runs on the same thread counts for that root (file sink slot swap, the
// one anvil-log-throttle uses; the throttle's own swap stays in the chain).
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_panel.h>

#include <windows.h>
#include <intrin.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "settings.gen.hpp"
#include "symbols.gen.hpp"

#pragma intrinsic(_ReturnAddress)

namespace {

struct OwnerEntry {
    const char* cls;
    int offset;
    const char* member;
    int effect;
    int name_offset;
};
const OwnerEntry kOwners[] = {
#include "owners.inc"
};

// --- per-thread statistics --------------------------------------------------

struct Stat {
    std::uint64_t calls = 0;
    std::uint64_t ticks = 0;
    std::uint64_t errors = 0;
    std::uintptr_t ret = 0;       // first caller seen
    const void* parent = nullptr; // the root effect a root trigger ran in, first seen
    bool effect = false;
};

struct ThreadStats {
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<const void*, Stat> roots;
    std::uint64_t nested_triggers = 0;
    std::uint64_t nested_effects = 0;
    std::uint64_t root_ticks = 0;
    std::uint64_t errors_inside = 0;
    std::uint64_t errors_outside = 0;
    DWORD thread_id = 0;
    // Owner thread only:
    int trigger_depth = 0;
    int effect_depth = 0;
    const void* current = nullptr;     // innermost root running
    const void* effect_root = nullptr; // root effect running
};

std::mutex g_threads_mutex;
std::vector<ThreadStats*> g_threads;
thread_local ThreadStats* t_stats = nullptr;

ThreadStats& stats() {
    if (t_stats == nullptr) {
        auto* s = new ThreadStats();
        s->thread_id = GetCurrentThreadId();
        std::lock_guard lock(g_threads_mutex);
        g_threads.push_back(s);
        t_stats = s;
    }
    return *t_stats;
}

std::atomic<bool> g_measuring{false};
std::atomic<int> g_mode{0};
std::uintptr_t g_base = 0;
std::uintptr_t g_image_end = 0;

void record(ThreadStats& s, const void* key, std::uint64_t ticks, std::uintptr_t ret, bool effect, const void* parent) {
    AcquireSRWLockExclusive(&s.lock);
    Stat& st = s.roots[key];
    if (st.calls == 0) {
        st.ret = ret - g_base;
        st.parent = parent;
        st.effect = effect;
    }
    ++st.calls;
    st.ticks += ticks;
    s.root_ticks += ticks;
    ReleaseSRWLockExclusive(&s.lock);
}

// --- detours ----------------------------------------------------------------

anvil::HookOf<sym::and_trigger_evaluate> g_and;
anvil::HookOf<sym::effect_execute> g_effect;

bool on_and(void* trigger, void* scope) noexcept {
    if (!g_measuring.load(std::memory_order_relaxed)) return g_and.next(trigger, scope);
    ThreadStats& s = stats();
    if (s.trigger_depth != 0) {
        ++s.trigger_depth;
        ++s.nested_triggers;
        const bool r = g_and.next(trigger, scope);
        --s.trigger_depth;
        return r;
    }
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    s.trigger_depth = 1;
    const void* previous = s.current;
    s.current = trigger;
    bool r;
    std::uint64_t ticks = 0;
    if (g_mode.load(std::memory_order_relaxed) >= 3) {
        const std::uint64_t a = __rdtsc();
        r = g_and.next(trigger, scope);
        ticks = __rdtsc() - a;
    } else {
        r = g_and.next(trigger, scope);
    }
    s.current = previous;
    s.trigger_depth = 0;
    record(s, trigger, ticks, ret, false, s.effect_root);
    return r;
}

void on_effect(void* effect, void* scope, std::int32_t flags) noexcept {
    if (!g_measuring.load(std::memory_order_relaxed)) return g_effect.next(effect, scope, flags);
    ThreadStats& s = stats();
    if (s.effect_depth != 0) {
        ++s.effect_depth;
        ++s.nested_effects;
        g_effect.next(effect, scope, flags);
        --s.effect_depth;
        return;
    }
    // A root effect inside a root trigger's evaluation (rare) is still its
    // own root: triggers and effects have separate depths.
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    s.effect_depth = 1;
    const void* previous = s.current;
    const int saved_trigger_depth = s.trigger_depth;
    s.current = effect;
    s.effect_root = effect;
    s.trigger_depth = 0;  // a limit inside the effect is a root trigger with this effect as parent
    std::uint64_t ticks = 0;
    if (g_mode.load(std::memory_order_relaxed) >= 3) {
        const std::uint64_t a = __rdtsc();
        g_effect.next(effect, scope, flags);
        ticks = __rdtsc() - a;
    } else {
        g_effect.next(effect, scope, flags);
    }
    s.trigger_depth = saved_trigger_depth;
    s.effect_root = nullptr;
    s.current = previous;
    s.effect_depth = 0;
    record(s, effect, ticks, ret, true, nullptr);
}

// --- log lines inside a block -------------------------------------------------

using WriteFn = void(void* sink, int channel, const void* file, int line, const void* message);
anvil::PointerSwap<WriteFn> g_write;
std::atomic<bool> g_count_errors{false};

void on_write(void* sink, int channel, const void* file, int line, const void* message) {
    if (g_measuring.load(std::memory_order_relaxed) && g_count_errors.load(std::memory_order_relaxed)) {
        ThreadStats& s = stats();
        AcquireSRWLockExclusive(&s.lock);
        if (s.current != nullptr) {
            auto it = s.roots.find(s.current);
            if (it == s.roots.end()) {
                // The root records itself when it ends; count it now.
                Stat& st = s.roots[s.current];
                st.errors = 1;
                st.effect = s.effect_depth != 0 && s.current == s.effect_root;
            } else {
                ++it->second.errors;
            }
            ++s.errors_inside;
        } else {
            ++s.errors_outside;
        }
        ReleaseSRWLockExclusive(&s.lock);
    }
    g_write.previous()(sink, channel, file, line, message);
}

// --- owners -------------------------------------------------------------------

bool safe_read(std::uintptr_t at, void* out, std::size_t n) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool in_image(std::uintptr_t p) { return p >= g_base && p < g_image_end; }

// eu4.exe has no RTTI for its own classes (/GR-): a class is told by slots 0
// and 1 of its vtable, as Steam RVAs (vtables.inc, from the GOG PDB).
struct VtableEntry {
    std::uint32_t slot0;
    std::uint32_t slot1;
    const char* name;
};
const VtableEntry kVtables[] = {
#include "vtables.inc"
};

bool signature_of(std::uintptr_t obj, std::uint32_t& s0, std::uint32_t& s1) {
    std::uintptr_t vt = 0;
    if (!safe_read(obj, &vt, 8) || !in_image(vt)) return false;
    std::uintptr_t slots[2] = {};
    if (!safe_read(vt, slots, sizeof(slots)) || !in_image(slots[0]) || !in_image(slots[1])) return false;
    s0 = static_cast<std::uint32_t>(slots[0] - g_base);
    s1 = static_cast<std::uint32_t>(slots[1] - g_base);
    return true;
}

// The class whose vtable the object at `obj` has, or "" (first of classes
// that share both slots, with "~" after it).
std::string class_of(std::uintptr_t obj) {
    std::uint32_t s0 = 0, s1 = 0;
    if (!signature_of(obj, s0, s1)) return {};
    const VtableEntry* lo = std::lower_bound(std::begin(kVtables), std::end(kVtables), std::pair{s0, s1},
                                             [](const VtableEntry& e, std::pair<std::uint32_t, std::uint32_t> k) {
                                                 return std::pair{e.slot0, e.slot1} < k;
                                             });
    if (lo == std::end(kVtables) || lo->slot0 != s0 || lo->slot1 != s1) return {};
    const bool shared = (lo + 1) != std::end(kVtables) && lo[1].slot0 == s0 && lo[1].slot1 == s1;
    return std::string(lo->name) + (shared ? "~" : "");
}

std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> g_signature_of_class;

bool is_class(std::uintptr_t obj, const char* cls) {
    const auto it = g_signature_of_class.find(cls);
    if (it == g_signature_of_class.end()) return false;
    std::uint32_t s0 = 0, s1 = 0;
    return signature_of(obj, s0, s1) && s0 == it->second.first && s1 == it->second.second;
}

void init_signatures() {
    for (const VtableEntry& e : kVtables) g_signature_of_class.emplace(e.name, std::pair{e.slot0, e.slot1});
}

std::string game_string(std::uintptr_t at) {
    struct {
        char buf[16];
        std::uint64_t size, cap;
    } s{};
    if (!safe_read(at, &s, sizeof(s)) || s.size > 512) return {};
    if (s.cap <= 15) return std::string(s.buf, std::min<std::uint64_t>(s.size, 15));
    std::uintptr_t p = 0;
    std::memcpy(&p, s.buf, 8);
    std::string out(static_cast<std::size_t>(s.size), '\0');
    if (!safe_read(p, out.data(), out.size())) return {};
    return out;
}

struct Owner {
    std::string self;    // the block's own class
    std::string owner;   // owner class, or "" when unknown
    std::string member;
    std::string name;
};

constexpr int kEventImmediate = 304;   // CEvent::_Immediate (CEventOption)
constexpr int kEventAfter = 1120;      // CEvent::_AfterEffect (CEventOption)
constexpr int kEventDebugKey = 32;

Owner resolve(const void* block, bool effect) {
    Owner o;
    const auto p = reinterpret_cast<std::uintptr_t>(block);
    o.self = class_of(p);
    for (const OwnerEntry& e : kOwners) {
        if ((e.effect != 0) != effect) continue;
        const std::uintptr_t owner = p - static_cast<std::uintptr_t>(e.offset);
        if (!is_class(owner, e.cls)) continue;
        o.owner = e.cls;
        o.member = e.member;
        if (e.name_offset >= 0) o.name = game_string(owner + static_cast<std::uintptr_t>(e.name_offset));
        if (o.owner == "CEventOption") {
            for (int off : {kEventImmediate, kEventAfter}) {
                const std::uintptr_t ev = owner - static_cast<std::uintptr_t>(off);
                if (is_class(ev, "CEvent")) {
                    o.owner = "CEvent";
                    o.member = off == kEventImmediate ? "immediate" : "after";
                    o.name = game_string(ev + kEventDebugKey);
                }
            }
        }
        break;
    }
    return o;
}

std::mutex g_owner_mutex;
std::unordered_map<const void*, Owner> g_owner_cache;

const Owner& owner_of(const void* block, bool effect) {
    std::lock_guard lock(g_owner_mutex);
    auto it = g_owner_cache.find(block);
    if (it != g_owner_cache.end()) return it->second;
    return g_owner_cache.emplace(block, resolve(block, effect)).first->second;
}

std::string describe(const Owner& o) {
    if (o.owner.empty()) return o.self.empty() ? "?" : o.self + " (owner not found)";
    std::string s = o.owner + " " + (o.name.empty() ? "?" : o.name) + " " + o.member;
    return s;
}

// --- reports ------------------------------------------------------------------

struct Row {
    const void* key;
    Stat stat;
};

struct Totals {
    std::uint64_t roots = 0, root_calls = 0, nested_triggers = 0, nested_effects = 0, root_ticks = 0;
    std::uint64_t errors_inside = 0, errors_outside = 0;
    std::size_t threads = 0;
};

std::vector<Row> collect(Totals& t) {
    std::unordered_map<const void*, Stat> all;
    std::vector<ThreadStats*> threads;
    {
        std::lock_guard lock(g_threads_mutex);
        threads = g_threads;
    }
    for (ThreadStats* s : threads) {
        AcquireSRWLockShared(&s->lock);
        if (!s->roots.empty()) ++t.threads;
        for (const auto& [k, v] : s->roots) {
            Stat& a = all[k];
            if (a.calls == 0 && a.ret == 0) {
                a.ret = v.ret;
                a.parent = v.parent;
                a.effect = v.effect;
            }
            a.calls += v.calls;
            a.ticks += v.ticks;
            a.errors += v.errors;
        }
        t.nested_triggers += s->nested_triggers;
        t.nested_effects += s->nested_effects;
        t.root_ticks += s->root_ticks;
        t.errors_inside += s->errors_inside;
        t.errors_outside += s->errors_outside;
        ReleaseSRWLockShared(&s->lock);
    }
    std::vector<Row> rows;
    rows.reserve(all.size());
    for (const auto& [k, v] : all) {
        rows.push_back({k, v});
        t.root_calls += v.calls;
    }
    t.roots = rows.size();
    const bool timed = g_mode.load() >= 3;
    std::sort(rows.begin(), rows.end(), [timed](const Row& a, const Row& b) {
        return timed ? a.stat.ticks > b.stat.ticks : a.stat.calls > b.stat.calls;
    });
    return rows;
}

void reset_all() {
    std::vector<ThreadStats*> threads;
    {
        std::lock_guard lock(g_threads_mutex);
        threads = g_threads;
    }
    for (ThreadStats* s : threads) {
        AcquireSRWLockExclusive(&s->lock);
        s->roots.clear();
        s->nested_triggers = s->nested_effects = s->root_ticks = 0;
        s->errors_inside = s->errors_outside = 0;
        ReleaseSRWLockExclusive(&s->lock);
    }
}

// rdtsc to seconds, calibrated against QueryPerformanceCounter over the span.
std::atomic<std::uint64_t> g_tsc0{0}, g_qpc0{0}, g_tsc_stop{0}, g_qpc_stop{0};
std::atomic<double> g_measured_seconds_before{0.0};
double g_qpf = 1.0;

std::uint64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}

double tsc_per_second() {
    const std::uint64_t t1 = g_measuring ? __rdtsc() : g_tsc_stop.load();
    const std::uint64_t q1 = g_measuring ? qpc() : g_qpc_stop.load();
    const double dq = static_cast<double>(q1 - g_qpc0.load()) / g_qpf;
    if (dq <= 0.01) return 3.0e9;
    return static_cast<double>(t1 - g_tsc0.load()) / dq;
}

double span_seconds() {
    const std::uint64_t q1 = g_measuring ? qpc() : g_qpc_stop.load();
    if (g_qpc0.load() == 0) return 0;
    return static_cast<double>(q1 - g_qpc0.load()) / g_qpf;
}

std::atomic<std::uint32_t> g_days{0};
std::string g_first_date, g_last_date;
std::mutex g_date_mutex;

void start() {
    if (g_measuring) return;
    reset_all();
    {
        std::lock_guard lock(g_owner_mutex);
        g_owner_cache.clear();
    }
    g_days = 0;
    g_tsc0 = __rdtsc();
    g_qpc0 = qpc();
    g_measuring = true;
    anvil::Host{}.info("measuring (mode {})", g_mode.load());
}

void stop() {
    if (!g_measuring) return;
    g_measuring = false;
    g_tsc_stop = __rdtsc();
    g_qpc_stop = qpc();
    anvil::Host{}.info("stopped after {:.1f} s, {} days", span_seconds(), g_days.load());
}

std::filesystem::path g_csv;

std::string csv_field(std::string s) {
    for (char& c : s) {
        if (c == ',' || c == '\n' || c == '\r' || c == '"') c = ' ';
    }
    return s;
}

void write_csv(const std::vector<Row>& rows, const Totals& t) {
    const double f = tsc_per_second();
    const std::filesystem::path tmp = g_csv.native() + L".tmp";
    FILE* out = nullptr;
    if (_wfopen_s(&out, tmp.c_str(), L"wb") != 0) out = nullptr;
    if (out == nullptr) return;
    std::string first, last;
    {
        std::lock_guard lock(g_date_mutex);
        first = g_first_date;
        last = g_last_date;
    }
    std::fprintf(out, "# mode=%d span_s=%.3f days=%u dates=%s..%s roots=%llu root_calls=%llu nested_triggers=%llu nested_effects=%llu root_ms=%.3f threads=%zu errors_inside=%llu errors_outside=%llu tsc_per_s=%.0f\n",
                 g_mode.load(), span_seconds(), g_days.load(), first.c_str(), last.c_str(), t.roots, t.root_calls,
                 t.nested_triggers, t.nested_effects, 1000.0 * static_cast<double>(t.root_ticks) / f, t.threads,
                 t.errors_inside, t.errors_outside, f);
    std::fprintf(out, "rank,kind,calls,total_ms,mean_us,errors,self,owner,member,name,caller_rva,parent\n");
    const std::size_t n = std::min<std::size_t>(rows.size(), 2000);
    for (std::size_t i = 0; i < n; ++i) {
        const Row& r = rows[i];
        const Owner& o = owner_of(r.key, r.stat.effect);
        std::string parent;
        if (r.stat.parent != nullptr) parent = describe(owner_of(r.stat.parent, true));
        const double ms = 1000.0 * static_cast<double>(r.stat.ticks) / f;
        std::fprintf(out, "%zu,%s,%llu,%.4f,%.3f,%llu,%s,%s,%s,%s,0x%llX,%s\n", i + 1, r.stat.effect ? "effect" : "trigger",
                     r.stat.calls, ms, r.stat.calls ? 1000.0 * ms / static_cast<double>(r.stat.calls) : 0.0, r.stat.errors,
                     csv_field(o.self).c_str(), csv_field(o.owner).c_str(), csv_field(o.member).c_str(),
                     csv_field(o.name).c_str(), static_cast<unsigned long long>(r.stat.ret), csv_field(parent).c_str());
    }
    std::fclose(out);
    MoveFileExW(tmp.c_str(), g_csv.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// --- panel --------------------------------------------------------------------

const AnvilPanelService* g_panels = nullptr;
AnvilPanel* g_panel = nullptr;
std::mutex g_panel_mutex;
std::int64_t g_top = 30;

enum : std::uint64_t { kStart = 1, kStop = 2, kReset = 3, kCsv = 4 };

std::string fmt_count(std::uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), " ");
    return s;
}

void publish() {
    if (g_panels == nullptr || g_panel == nullptr) return;
    Totals t;
    const std::vector<Row> rows = collect(t);
    const double f = tsc_per_second();
    std::vector<std::string> labels, values;
    std::vector<AnvilPanelRow> out;
    auto text = [&](std::string label, std::string value) {
        labels.push_back(std::move(label));
        values.push_back(std::move(value));
        out.push_back({ANVIL_PANEL_TEXT, 0, nullptr, nullptr, 0});
    };
    auto button = [&](std::string label, std::uint64_t id) {
        labels.push_back(std::move(label));
        values.emplace_back();
        out.push_back({ANVIL_PANEL_BUTTON, 0, nullptr, nullptr, id});
    };
    char buf[256];
    std::snprintf(buf, sizeof(buf), "mode %d, %s, %.1f s, %u days", g_mode.load(), g_measuring ? "measuring" : "stopped",
                  span_seconds(), g_days.load());
    text("State", buf);
    std::snprintf(buf, sizeof(buf), "%s roots, %s calls, %s nested", fmt_count(t.roots).c_str(),
                  fmt_count(t.root_calls).c_str(), fmt_count(t.nested_triggers + t.nested_effects).c_str());
    text("Blocks", buf);
    if (g_mode.load() >= 3) {
        std::snprintf(buf, sizeof(buf), "%.1f ms on %zu threads", 1000.0 * static_cast<double>(t.root_ticks) / f, t.threads);
        text("Time in roots", buf);
    }
    button(g_measuring ? "Stop" : "Start", g_measuring ? kStop : kStart);
    button("Reset", kReset);
    button("Write CSV", kCsv);
    out.push_back({ANVIL_PANEL_SEPARATOR, 0, nullptr, nullptr, 0});
    labels.emplace_back();
    values.emplace_back();
    const std::size_t n = std::min<std::size_t>(rows.size(), static_cast<std::size_t>(g_top));
    for (std::size_t i = 0; i < n; ++i) {
        const Row& r = rows[i];
        const Owner& o = owner_of(r.key, r.stat.effect);
        std::string label = (r.stat.effect ? "E " : "T ") + describe(o);
        if (label.size() > 70) label = label.substr(0, 34) + "…" + label.substr(label.size() - 34);
        const double ms = 1000.0 * static_cast<double>(r.stat.ticks) / f;
        if (g_mode.load() >= 3) {
            std::snprintf(buf, sizeof(buf), "%.2f ms, %s×", ms, fmt_count(r.stat.calls).c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%s×", fmt_count(r.stat.calls).c_str());
        }
        std::string value = buf;
        if (r.stat.errors) value += ", " + fmt_count(r.stat.errors) + " log";
        text(std::move(label), std::move(value));
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i].label = out[i].kind == ANVIL_PANEL_SEPARATOR ? nullptr : labels[i].c_str();
        out[i].value = values[i].empty() ? nullptr : values[i].c_str();
    }
    std::lock_guard lock(g_panel_mutex);
    g_panels->set_rows(g_panel, out.data(), static_cast<std::uint32_t>(out.size()), sizeof(AnvilPanelRow));
}

void dump() {
    Totals t;
    const std::vector<Row> rows = collect(t);
    write_csv(rows, t);
}

void on_click(void*, std::uint64_t id) {
    switch (id) {
        case kStart: start(); break;
        case kStop: stop(); dump(); break;
        case kReset: reset_all(); g_qpc0 = qpc(); g_tsc0 = __rdtsc(); g_days = 0; break;
        case kCsv: dump(); break;
        default: break;
    }
    publish();
}

// --- runtime level, for the overhead series -------------------------------------
//
// <userdir>/script_profiler_control.txt: "<level> <tag>", read every 2 s.
// Level 0 removes the hooks, 1 installs them passing on, 2 counts, 3 times.
// At each change the span before it is written to logs/script_profile_<tag>.csv
// of the old tag, and a level of 2 or 3 starts a new span.
std::filesystem::path g_control;
std::filesystem::path g_logs;
std::string g_control_text;
bool g_installed = false;
bool g_hook_effects = true;

void install(bool on) {
    if (on == g_installed) return;
    const anvil::Host host;
    try {
        if (on) {
            host.group([&] {
                host.hook(g_and, sym::and_trigger_evaluate, on_and);
                if (g_hook_effects) host.hook(g_effect, sym::effect_execute, on_effect);
                if (g_count_errors) host.swap(g_write, sym::file_sink_write_slot, &on_write);
            });
        } else {
            g_and.remove();
            g_effect.remove();
            g_write.remove();
        }
        g_installed = on;
        host.info("hooks {}", on ? "installed" : "removed");
    } catch (const std::exception& e) {
        host.warn("hooks {}: {}", on ? "install" : "remove", e.what());
    }
}

void poll_control() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_control.c_str(), L"rb") != 0 || f == nullptr) return;
    char buf[128] = {};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string text(buf, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (text == g_control_text) return;
    g_control_text = text;
    int level = 0;
    char tag[64] = {};
    if (sscanf_s(text.c_str(), "%d %63s", &level, tag, static_cast<unsigned>(sizeof(tag))) < 1) return;
    if (g_measuring) {
        stop();
        dump();
    }
    g_mode = level;
    install(level >= 1);
    g_csv = g_logs / (std::string("script_profile_") + (tag[0] ? tag : "prototype") + ".csv");
    anvil::Host{}.info("control: level {} tag {}", level, tag);
    if (level >= 2) start();
}

void report_loop() {
    int tick = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        poll_control();
        publish();
        if (g_measuring && (++tick % 5) == 0) dump();
    }
}

// --- paths --------------------------------------------------------------------

std::filesystem::path user_dir() {
    const std::wstring line = GetCommandLineW();
    const std::wstring key = L"-userdir=";
    const std::size_t at = line.find(key);
    if (at != std::wstring::npos) {
        std::size_t from = at + key.size();
        const bool quoted = from < line.size() && line[from] == L'"';
        if (quoted) ++from;
        std::size_t end = line.find(quoted ? L'"' : L' ', from);
        if (end == std::wstring::npos) end = line.size();
        return std::filesystem::path(line.substr(from, end - from));
    }
    PWSTR docs = nullptr;
    std::filesystem::path out;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) {
        out = std::filesystem::path(docs) / L"Paradox Interactive" / L"Europa Universalis IV";
    }
    CoTaskMemFree(docs);
    return out;
}

bool g_auto_start = false;

struct PrototypeScriptProfiler : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + static_cast<std::uintptr_t>(dos->e_lfanew));
        g_image_end = g_base + nt->OptionalHeader.SizeOfImage;
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        g_qpf = static_cast<double>(fq.QuadPart);
        init_signatures();
        g_mode = static_cast<int>(cfg::mode.get());
        g_auto_start = cfg::auto_start.get();
        g_top = cfg::top.get();
        g_count_errors = cfg::errors.get();
        g_logs = user_dir() / L"logs";
        g_control = user_dir() / L"script_profiler_control.txt";
        g_csv = g_logs / L"script_profile_prototype.csv";
        g_hook_effects = cfg::effects.get();
        if (g_mode.load() >= 1) {
            host.hook(g_and, sym::and_trigger_evaluate, on_and);
            if (g_hook_effects) host.hook(g_effect, sym::effect_execute, on_effect);
            if (g_count_errors) host.swap(g_write, sym::file_sink_write_slot, &on_write);
            g_installed = true;
        }
        if (g_mode.load() == 1) {
            host.info("mode 1: hooks pass on, nothing is measured");
        }
        host.subscribe(ANVIL_GAME_EVENT_DAY, [](const void* payload) {
            const auto* d = static_cast<const AnvilGameDateChange*>(payload);
            if (g_auto_start && g_mode.load() >= 2 && !g_measuring) start();
            if (!g_measuring) return;
            g_days += d->days;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u", d->date.year, d->date.month, d->date.day);
            std::lock_guard lock(g_date_mutex);
            if (g_days.load() == d->days) g_first_date = buf;
            g_last_date = buf;
        });
        host.subscribe(ANVIL_GAME_EVENT_SESSION, [](const void*) {
            if (g_measuring) {
                stop();
                dump();
            }
        });
        g_panels = host.service<AnvilPanelService>(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION);
        if (g_panels != nullptr && g_panels->create("Script profiler (PROTOTYPE)", &g_panel) == ANVIL_OK) {
            g_panels->on_click(g_panel, &on_click, nullptr);
            g_panels->show(g_panel, 1);
        }
        host.info("mode {}, {} owner entries, csv {}", g_mode.load(), std::size(kOwners), g_csv.string());
        std::thread(report_loop).detach();
    }
};

}  // namespace

ANVIL_PLUGIN(PrototypeScriptProfiler)
