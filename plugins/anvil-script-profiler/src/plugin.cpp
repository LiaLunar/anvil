// anvil-script-profiler: the standard plugin that times the game's triggers
// and effects by their owner (docs/script-profiler.md; eu4-engine-lab#154,
// #158, #163).
//
// Loaded and idle by default: no hook and no thread until the modder starts a
// profiling span in its panel. A span hooks the trigger gate
// CAndTrigger::Evaluate (every AND block: the root of each trigger, each limit)
// and CEffect::Execute (every effect block), and from v1.1 CScriptedTrigger::Evaluate
// (every call of a scripted trigger); each game thread times only its
// outermost block of each kind, its root, in a table of its own
// (recorder.hpp). Stop, the end of "Measure N days", a load, leaving to the
// menu or a normal exit ends the span: the hooks go, the tables are summed,
// merged by owner and role (owners.hpp, report.hpp), shown in the panel and
// written to logs/script_profile_<date>.csv in the user folder.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_panel.h>

#include <windows.h>
#include <intrin.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings.gen.hpp"
#include "symbols.gen.hpp"

#include "owners.hpp"
#include "recorder.hpp"
#include "report.hpp"
#include "texts.hpp"

#pragma intrinsic(_ReturnAddress)

namespace sp = script_profiler;

namespace {

// ---- the hooks (game threads) ------------------------------------------------

anvil::HookOf<sym::and_trigger_evaluate> g_and;
anvil::HookOf<sym::effect_execute> g_effect;
anvil::HookOf<sym::scripted_trigger_evaluate> g_scripted;
std::uintptr_t g_base = 0;

std::uint32_t rva_of(void* return_address) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(return_address) - g_base);
}

bool on_and(void* trigger, void* scope) noexcept {
    sp::Thread* t = sp::this_thread();
    if (t == nullptr) return g_and.next(trigger, scope);
    if (t->trigger_depth != 0) {
        ++t->trigger_depth;
        t->table.nested(sp::Kind::trigger);
        const bool r = g_and.next(trigger, scope);
        --t->trigger_depth;
        return r;
    }
    const std::uint32_t caller = rva_of(_ReturnAddress());
    t->trigger_depth = 1;
    const std::uint64_t start = __rdtsc();
    const bool r = g_and.next(trigger, scope);
    const std::uint64_t ticks = __rdtsc() - start;
    t->trigger_depth = 0;
    t->table.record(trigger, sp::Kind::trigger, ticks, caller);
    return r;
}

void on_effect(void* effect, void* scope, std::int32_t flags) noexcept {
    sp::Thread* t = sp::this_thread();
    if (t == nullptr) return g_effect.next(effect, scope, flags);
    if (t->effect_depth != 0) {
        ++t->effect_depth;
        t->table.nested(sp::Kind::effect);
        g_effect.next(effect, scope, flags);
        --t->effect_depth;
        return;
    }
    // A limit inside the effect is a root trigger of its own: triggers and
    // effects have separate depths.
    const std::uint32_t caller = rva_of(_ReturnAddress());
    const int trigger_depth = t->trigger_depth;
    t->effect_depth = 1;
    t->trigger_depth = 0;
    const std::uint64_t start = __rdtsc();
    g_effect.next(effect, scope, flags);
    const std::uint64_t ticks = __rdtsc() - start;
    t->trigger_depth = trigger_depth;
    t->effect_depth = 0;
    t->table.record(effect, sp::Kind::effect, ticks, caller);
}

// Every call of a scripted trigger, also inside another block: only the outermost scripted
// trigger on the thread is timed. The CScriptedTrigger is also an AND block that the gate
// sees, so its row here is kept at its address + 1.
bool on_scripted(void* trigger, void* scope) noexcept {
    sp::Thread* t = sp::this_thread();
    if (t == nullptr) return g_scripted.next(trigger, scope);
    if (t->scripted_depth != 0) {
        ++t->scripted_depth;
        t->table.nested(sp::Kind::scripted);
        const bool r = g_scripted.next(trigger, scope);
        --t->scripted_depth;
        return r;
    }
    const std::uint32_t caller = rva_of(_ReturnAddress());
    t->scripted_depth = 1;
    const std::uint64_t start = __rdtsc();
    const bool r = g_scripted.next(trigger, scope);
    const std::uint64_t ticks = __rdtsc() - start;
    t->scripted_depth = 0;
    t->table.record(static_cast<const char*>(trigger) + 1, sp::Kind::scripted, ticks, caller);
    return r;
}

// ---- the span (the game's main thread) ---------------------------------------

const AnvilGameService* g_game = nullptr;
const AnvilPanelService* g_panels = nullptr;
AnvilPanel* g_panel = nullptr;
std::unique_ptr<sp::Resolver> g_resolver;
std::string g_anvil_version;
std::string g_build;

std::atomic<std::int64_t> g_measure_days{30};
std::atomic<std::int64_t> g_top{20};

struct State {
    bool measuring = false;
    bool in_session = false;
    bool multiplayer = false;
    bool ironman = false;
    std::uint32_t days = 0;
    std::uint32_t target_days = 0;  // 0: until Stop
    std::string first_date, last_date;
    std::uint64_t tsc0 = 0, qpc0 = 0;
    std::string started_at;
    std::string csv_name;
    std::filesystem::path logs;  // the user folder's logs/
    std::vector<std::uint32_t> cursors;  // per thread: blocks already given an owner
    std::unique_ptr<sp::Report> last;
    std::string csv_path;
    bool csv_failed = false;
    std::uint64_t published_ms = 0;
};
std::mutex g_mutex;  // the main thread, the lab's control thread and the exit
State g_state;

std::uint64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}
double qpf() {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return static_cast<double>(v.QuadPart);
}

std::string date_text(const AnvilGameDate& d) {
    return std::to_string(d.year) + "." + std::to_string(d.month) + "." + std::to_string(d.day);
}

// EU4's user folder: -userdir=<path> on the command line, else Documents.
std::filesystem::path user_dir() {
    const std::wstring line = GetCommandLineW();
    const std::wstring key = L"-userdir=";
    if (const std::size_t at = line.find(key); at != std::wstring::npos) {
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

sp::Texts texts() {
    return sp::Texts{sp::lang_of(g_panels != nullptr && g_panels->language != nullptr ? g_panels->language() : "en")};
}

double seconds_since(std::uint64_t qpc0) { return static_cast<double>(qpc() - qpc0) / qpf(); }

// Under g_mutex.
void publish_locked() {
    if (g_panels == nullptr || g_panel == nullptr) return;
    sp::PanelState ps;
    ps.measuring = g_state.measuring;
    ps.days_so_far = g_state.days;
    ps.days_to_measure = g_state.target_days;
    ps.measure_days = g_measure_days.load();
    ps.seconds_so_far = g_state.measuring ? seconds_since(g_state.qpc0) : 0;
    ps.in_session = g_state.in_session;
    ps.multiplayer = g_state.in_session && g_state.multiplayer;
    ps.csv_path = g_state.csv_path;
    ps.csv_failed = g_state.csv_failed;
    ps.top = static_cast<std::size_t>(g_top.load());
    const std::vector<sp::PanelRow> rows = sp::panel_rows(ps, g_state.last.get(), texts());
    std::vector<AnvilPanelRow> out(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        out[i].kind = rows[i].kind;
        out[i].label = rows[i].kind == ANVIL_PANEL_SEPARATOR ? nullptr : rows[i].label.c_str();
        out[i].value = rows[i].has_value ? rows[i].value.c_str() : nullptr;
        out[i].id = rows[i].id;
    }
    g_panels->set_rows(g_panel, out.data(), static_cast<std::uint32_t>(out.size()), sizeof(AnvilPanelRow));
    g_state.published_ms = GetTickCount64();
}

std::uintptr_t* g_aspect_db = nullptr;  // where the game keeps CChurchAspectDatabase*
bool g_aspects_added = false;

// Gives the blocks recorded since the last call their owner while the game's
// data is certainly alive (a day's end, the end of a span). Under g_mutex.
void name_new_blocks_locked() {
    if (!g_aspects_added && g_aspect_db != nullptr) {
        std::uintptr_t db = 0;
        if (sp::read_memory(reinterpret_cast<std::uintptr_t>(g_aspect_db), &db, sizeof(db)) && db != 0) {
            g_resolver->add_church_aspects(db);
            g_aspects_added = true;
        }
    }
    const std::size_t n = sp::thread_count();
    if (g_state.cursors.size() < n) g_state.cursors.resize(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const sp::ThreadTable& table = sp::thread_at(i)->table;
        const std::uint32_t used = table.used();
        for (std::uint32_t k = g_state.cursors[i]; k < used; ++k) {
            const sp::Slot& s = table.nth(k);
            const auto* block = reinterpret_cast<const void*>(s.key.load(std::memory_order_acquire));
            if (block != nullptr) {
                g_resolver->owner_of(block, static_cast<sp::Kind>(s.kind.load(std::memory_order_relaxed)));
            }
        }
        g_state.cursors[i] = used;
    }
}

bool write_file(const std::filesystem::path& path, const std::string& text) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    const std::filesystem::path tmp = path.native() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 &&
                    written == text.size();
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Ends the span. At exit (`exiting`) only the owners named so far are used and
// nothing is logged: the game's other threads are gone and its data may be.
// Under g_mutex.
void stop_locked(sp::Ending ending, bool exiting) {
    if (!g_state.measuring) return;
    g_state.measuring = false;
    if (!exiting) {
        g_and.remove();
        g_effect.remove();
        g_scripted.remove();
    }
    const std::uint64_t tsc1 = __rdtsc();
    const double seconds = seconds_since(g_state.qpc0);
    sp::Span span;
    span.ending = ending;
    span.first_date = g_state.first_date;
    span.last_date = g_state.last_date;
    span.days = g_state.days;
    span.seconds = seconds;
    if (seconds > 0.01) span.ticks_per_second = static_cast<double>(tsc1 - g_state.tsc0) / seconds;
    span.started_at = g_state.started_at;
    span.anvil_version = g_anvil_version;
    span.game_build = g_build;
    span.multiplayer = g_state.multiplayer;
    span.ironman = g_state.ironman;
    const std::uint32_t main_thread = g_game != nullptr ? g_game->main_thread_id() : 0;
    const sp::Snapshot snapshot = sp::collect(main_thread);
    if (!exiting) {
        name_new_blocks_locked();
        g_resolver->retry_unowned();
    }
    const sp::Report full = sp::make_report(snapshot, *g_resolver, span, true, !exiting);
    // Never over an earlier span's file (two spans in one second).
    std::filesystem::path path = g_state.logs / g_state.csv_name;
    std::error_code ignored;
    for (int n = 2; std::filesystem::exists(path, ignored) && n < 100; ++n) {
        const std::string stem = g_state.csv_name.substr(0, g_state.csv_name.size() - 4);
        path = g_state.logs / (stem + "_" + std::to_string(n) + ".csv");
    }
    g_state.csv_name = path.filename().string();
    const bool ok = write_file(path, sp::csv(full));
    if (exiting) return;
    g_state.last = std::make_unique<sp::Report>(sp::make_report(snapshot, *g_resolver, span, false));
    g_state.csv_path = (std::filesystem::path(L"logs") / g_state.csv_name).string();
    g_state.csv_failed = !ok;
    const anvil::Host host;
    host.info("profiling span ended ({}): {} days, {:.1f} s, {} root calls on {} threads; hooks removed; {} {}",
              ending == sp::Ending::complete ? "complete" : "interrupted: the session ended", span.days, seconds,
              full.root_calls, full.threads, ok ? "wrote" : "could not write", path.string());
    publish_locked();
}

// Under g_mutex, on the main thread.
void start_locked(std::uint32_t days) {
    if (g_state.measuring || !g_state.in_session) return;
    sp::clear_all();
    g_state.cursors.assign(sp::thread_count(), 0);
    g_state.days = 0;
    g_state.target_days = days;
    g_state.first_date.clear();
    g_state.last_date.clear();
    SYSTEMTIME now;
    GetLocalTime(&now);
    char at[32];
    std::snprintf(at, sizeof(at), "%04d-%02d-%02dT%02d:%02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond);
    g_state.started_at = at;
    g_state.csv_name = sp::csv_name(now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    // Found now: at exit the shell's folder lookup may not run.
    g_state.logs = user_dir() / L"logs";
    const anvil::Host host;
    try {
        host.group([&] {
            host.hook(g_and, sym::and_trigger_evaluate, on_and);
            host.hook(g_effect, sym::effect_execute, on_effect);
            if (host.present(sym::scripted_trigger_evaluate)) {
                host.hook(g_scripted, sym::scripted_trigger_evaluate, on_scripted);
            }
        });
    } catch (const std::exception& e) {
        host.warn("profiling span not started: {}", e.what());
        return;
    }
    g_state.tsc0 = __rdtsc();
    g_state.qpc0 = qpc();
    g_state.measuring = true;
    if (days > 0) {
        host.info("profiling span started for {} days: hooks on CAndTrigger::Evaluate, CEffect::Execute and "
                  "CScriptedTrigger::Evaluate",
                  days);
    } else {
        host.info("profiling span started: hooks on CAndTrigger::Evaluate, CEffect::Execute and "
                  "CScriptedTrigger::Evaluate");
    }
    publish_locked();
}

void on_click(void*, std::uint64_t id) {
    std::lock_guard lock(g_mutex);
    switch (id) {
    case sp::kStart: start_locked(0); break;
    case sp::kMeasureDays: start_locked(static_cast<std::uint32_t>(g_measure_days.load())); break;
    case sp::kStop: stop_locked(sp::Ending::complete, false); break;
    default: break;
    }
    publish_locked();
}

void on_day(const void* payload) {
    const auto* d = static_cast<const AnvilGameDateChange*>(payload);
    std::lock_guard lock(g_mutex);
    if (!g_state.measuring || d == nullptr) return;
    if (g_state.days == 0) g_state.first_date = date_text(d->date);
    g_state.last_date = date_text(d->date);
    g_state.days += d->days;
    name_new_blocks_locked();
    if (g_state.target_days > 0 && g_state.days >= g_state.target_days) {
        stop_locked(sp::Ending::complete, false);
    } else if (GetTickCount64() - g_state.published_ms >= 1000) {
        publish_locked();
    }
}

void on_session(const void* payload) {
    const auto* s = static_cast<const AnvilGameSession*>(payload);
    std::lock_guard lock(g_mutex);
    if (s == nullptr) return;
    if (s->change == ANVIL_GAME_SESSION_ENDED) {
        stop_locked(sp::Ending::interrupted_session, false);
        g_state.in_session = false;
    } else {
        g_state.in_session = true;
        g_state.multiplayer = s->multiplayer != 0;
        g_state.ironman = s->ironman != 0;
    }
    publish_locked();
}

// ---- the lab's control file (ANVIL_SCRIPT_PROFILER_CONTROL) ------------------
//
// For repeatable measurements without the panel (the lab's overhead series): a
// file of one line, "<n> start", "<n> measure <days>" or "<n> stop"; a new <n>
// is a new command, run on the main thread. Its answer goes to <file>.state:
// "<n> measuring" or "<n> idle <csv>". Without the variable there is no thread.

std::filesystem::path g_control;
std::atomic<std::uint64_t> g_command{0};
std::string g_command_text;  // the control thread's

void write_state(std::uint64_t n) {
    std::string text;
    {
        std::lock_guard lock(g_mutex);
        text = std::to_string(n) + (g_state.measuring ? " measuring" : " idle " + g_state.csv_path) + "\n";
    }
    write_file(g_control.native() + L".state", text);
}

void run_command(void* context) {
    const auto n = reinterpret_cast<std::uintptr_t>(context);
    std::string text;
    {
        std::lock_guard lock(g_mutex);
        text = g_command_text;
    }
    unsigned long long seq = 0;
    char verb[16] = {};
    unsigned days = 0;
    const int got = sscanf_s(text.c_str(), "%llu %15s %u", &seq, verb, static_cast<unsigned>(sizeof(verb)), &days);
    if (got >= 2) {
        std::lock_guard lock(g_mutex);
        const std::string v = verb;
        if (v == "start") start_locked(0);
        if (v == "measure" && got == 3) start_locked(days);
        if (v == "stop") stop_locked(sp::Ending::complete, false);
        publish_locked();
    }
    write_state(n);
}

void control_loop() {
    std::string seen;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_control.c_str(), L"rb") != 0 || f == nullptr) continue;
        char buf[128] = {};
        const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        std::string text(buf, n);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        if (text.empty() || text == seen) continue;
        seen = text;
        const std::uint64_t seq = std::strtoull(text.c_str(), nullptr, 10);
        {
            std::lock_guard lock(g_mutex);
            g_command_text = text;
        }
        g_game->run_on_main_thread(&run_command, reinterpret_cast<void*>(static_cast<std::uintptr_t>(seq)));
    }
}

void republish() {
    if (g_game == nullptr) return;
    g_game->run_on_main_thread(
        [](void*) {
            std::lock_guard lock(g_mutex);
            publish_locked();
        },
        nullptr);
}

std::uintptr_t vtable(const anvil::Host& host, const auto& symbol) {
    return host.present(symbol) ? reinterpret_cast<std::uintptr_t>(host.address(symbol)) : 0;
}

struct ScriptProfiler : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_base = host.game().image_base;
        g_anvil_version = std::string(host.anvil_version());
        g_build = host.game().build_id != nullptr ? host.game().build_id : "unknown";
        sp::Vtables v;
        v.event = vtable(host, sym::event_vtable);
        v.decision = vtable(host, sym::decision_vtable);
        v.on_action = vtable(host, sym::on_action_vtable);
        v.casus_belli = vtable(host, sym::casus_belli_vtable);
        v.country_modifier = vtable(host, sym::triggered_modifier_vtable);
        v.province_modifier = vtable(host, sym::province_triggered_modifier_vtable);
        v.mission = vtable(host, sym::mission_vtable);
        v.church_aspect = vtable(host, sym::church_aspect_vtable);
        v.scripted_trigger = vtable(host, sym::scripted_trigger_vtable);
        g_resolver = std::make_unique<sp::Resolver>(v);
        if (host.present(sym::church_aspect_db_site)) g_aspect_db = host.data(sym::aspect_db);
        g_measure_days = cfg::days.get();
        g_top = cfg::rows.get();
        g_game = host.service<AnvilGameService>(ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION);
        // A setting's callback runs inside the overlay's drawing, which must
        // not be called back: the panel is refreshed after the frame.
        host.subscribe_setting(cfg::days, [](std::int64_t d) {
            g_measure_days = d;
            republish();
        });
        host.subscribe_setting(cfg::rows, [](std::int64_t n) {
            g_top = n;
            republish();
        });
        host.subscribe(ANVIL_GAME_EVENT_DAY, &on_day);
        host.subscribe(ANVIL_GAME_EVENT_SESSION, &on_session);
        g_panels = host.service<AnvilPanelService>(ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION);
        if (g_panels != nullptr && g_panels->size >= sizeof(AnvilPanelService) &&
            g_panels->create(texts()("Script profiler"), &g_panel) == ANVIL_OK) {
            g_panels->on_click(g_panel, &on_click, nullptr);
            std::lock_guard lock(g_mutex);
            publish_locked();
        } else {
            g_panels = nullptr;
            host.info("no anvil-overlay.panel: the profiler has no panel");
        }
        wchar_t control[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ANVIL_SCRIPT_PROFILER_CONTROL", control, MAX_PATH) > 0 && g_game != nullptr) {
            g_control = control;
            host.info("ANVIL_SCRIPT_PROFILER_CONTROL: commands from {}", g_control.string());
            std::thread(control_loop).detach();
        }
        host.info("idle: no hooks until a profiling span starts in the panel ({} owner classes known)",
                  (v.event != 0) + (v.decision != 0) + (v.on_action != 0) + (v.casus_belli != 0) +
                      (v.country_modifier != 0) + (v.province_modifier != 0) + (v.mission != 0) +
                      (v.church_aspect != 0) + (v.scripted_trigger != 0));
    }
};

}  // namespace

ANVIL_PLUGIN(ScriptProfiler)

// A normal exit during a span writes what it measured, marked interrupted. The
// game's other threads have ended; owners already named are used, nothing is
// read from the game.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        if (g_mutex.try_lock()) {
            try {
                stop_locked(sp::Ending::interrupted_exit, true);
            } catch (...) {
            }
            g_mutex.unlock();
        }
    }
    return TRUE;
}
