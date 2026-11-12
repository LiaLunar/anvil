#include "runtime.hpp"

#include "benchmark.hpp"
#include "benchmark_record.hpp"
#include "discovery.hpp"
#include "files.hpp"
#include "first_opens.hpp"
#include "mod_plugins.hpp"
#include "plugin_hash.hpp"
#include "game_build.hpp"
#include "gameplay_policy.hpp"
#include "host.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "platform.hpp"
#include "changes.hpp"
#include "control.hpp"
#include "crash.hpp"
#include "crash_history.hpp"
#include "plugin_symbols.hpp"
#include "resolver.hpp"
#include "safe_mode.hpp"
#include "seh_guard.hpp"
#include "services.hpp"
#include "plugin_translation.hpp"
#include "settings.hpp"
#include "symbol_check.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;  // version.dll

namespace anvil {
namespace {

struct Runtime {
    std::filesystem::path game_dir;
    std::filesystem::path anvil_dir;
    std::filesystem::path exe_path;
    std::string exe_name;  // e.g. eu4.exe, for crash reports
    PeIdentity identity;
    const KnownBuild* build = nullptr;
    AnvilGameInfo game{};
    std::unique_ptr<ChangeRegistry> changes;
    bool profile = false;  // anvil.toml: count calls per change
    ServiceBus bus;
    std::unique_ptr<GameplayPolicy> policy;  // design §13, fed by anvil-game's session event
    HostContext context;  // what every plugin's host calls act on
    AnvilHost host{};
    std::vector<std::unique_ptr<AnvilPlugin>> plugins;  // resolved, in load order
    std::vector<Skipped> skipped;
    StartMarker start_marker;  // safe mode: starts that ended before game_ready
    ModDiscovery mods;  // plugins in the playset's mods (design §14)
    std::unique_ptr<Control> control;  // anvil.control, the overlay's view of the core (design §15)
    std::unique_ptr<Benchmark> benchmark;  // anvil.benchmark (docs/spec/benchmark-v1.md §5)
    LARGE_INTEGER start{};
    LARGE_INTEGER frequency{};
    uint64_t entry_filetime = 0;  // the system time at `start`
    uint64_t entry_tick_ms = 0;   // GetTickCount64 at `start`
    DWORD entry_tick = 0;         // GetTickCount at `start`, the clock of GetLastInputInfo

    // The start-up benchmark (docs/spec/benchmark-v1.md §2, §6); `arm` is set
    // only in a benchmark start.
    std::optional<BenchmarkArm> arm;
    FirstOpens first_opens;
    bool first_opens_metered = false;
    // The previous start's history, read and replaced by the main phase.
    std::optional<StartHistory> previous_start;
    std::atomic<bool> history_read{false};
    LARGE_INTEGER ready{};             // anvil-game.ready
    std::optional<bool> input_during_start;
    std::optional<bool> first_start_since_boot, definitions_changed;  // computed for the ready record
    std::atomic<bool> plugins_loaded{false};  // the main phase has processed every plugin
};

Runtime* g_runtime = nullptr;

// Whose change a registry message is about.
std::string owner_name(OwnerId owner) {
    const auto& plugins = g_runtime->plugins;
    return owner < plugins.size() ? plugins[owner]->manifest.id : "anvil";
}

double elapsed_ms() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_runtime->start.QuadPart) * 1000.0 /
           static_cast<double>(g_runtime->frequency.QuadPart ? g_runtime->frequency.QuadPart : 1);
}

// The ms from the entry point to `at` (a QueryPerformanceCounter value).
double ms_since_entry(LARGE_INTEGER at) {
    return static_cast<double>(at.QuadPart - g_runtime->start.QuadPart) * 1000.0 /
           static_cast<double>(g_runtime->frequency.QuadPart ? g_runtime->frequency.QuadPart : 1);
}

std::string fmt_ms(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

// ---- the start-up benchmark's record (docs/spec/benchmark-v1.md §6) ------------

std::filesystem::path benchmark_record_file() {
    return g_runtime->anvil_dir / "data" / "anvil" / "benchmark-start.json";
}

// What the record says now.
BenchmarkRecord benchmark_record(const char* write) {
    BenchmarkRecord r;
    r.write = write;
    r.anvil_version = kAnvilVersion;
    r.arm = *g_runtime->arm;
    r.process_id = GetCurrentProcessId();
    r.qpc_frequency = g_runtime->frequency.QuadPart;
    r.entry_qpc = g_runtime->start.QuadPart;
    r.ready_qpc = g_runtime->ready.QuadPart;
    r.entry_filetime = g_runtime->entry_filetime;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        r.creation_filetime = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    std::vector<BenchmarkParticipant> participants = g_runtime->benchmark->participants();
    for (const auto& p : g_runtime->plugins) {
        BenchmarkRecordPlugin out;
        out.id = p->manifest.id;
        out.version = p->manifest.version.to_string();
        out.state = p->state;
        for (auto& joined : participants) {
            if (joined.plugin == p.get()) out.participant = std::move(joined);
        }
        r.plugins.push_back(std::move(out));
    }
    for (const auto& s : g_runtime->skipped) {
        BenchmarkRecordPlugin out;
        out.id = s.id;
        out.state = s.disabled ? "disabled" : "skipped";
        out.disabled_by_arm = s.benchmark_arm;
        out.skip_reason = skip_kind_name(s.kind);
        r.plugins.push_back(std::move(out));
    }
    if (g_runtime->first_opens_metered) r.game_first_opens = g_runtime->first_opens.total(r.qpc_frequency);
    r.first_start_since_boot = g_runtime->first_start_since_boot;
    r.definitions_changed = g_runtime->definitions_changed;
    r.input_during_start = g_runtime->input_during_start;
    return r;
}

bool write_benchmark_record(const BenchmarkRecord& record, std::string& error) {
    return write_file_atomic(benchmark_record_file(), benchmark_record_json(record), error);
}

// Keyboard or mouse input since the entry point (GetLastInputInfo, no
// elevation); nullopt when it cannot be asked.
std::optional<bool> input_since_entry() {
    using Fn = BOOL(WINAPI*)(PLASTINPUTINFO);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto fn = user32 != nullptr ? reinterpret_cast<Fn>(GetProcAddress(user32, "GetLastInputInfo")) : nullptr;
    LASTINPUTINFO info{sizeof(info), 0};
    if (fn == nullptr || !fn(&info)) return std::nullopt;
    return static_cast<int32_t>(info.dwTime - g_runtime->entry_tick) > 0;  // 32-bit ticks wrap
}

// After ready: waits until every plugin is loaded and every participant has
// called done(), at most ANVIL_BENCHMARK_QUIT_LIMIT_MS after ready; then the
// end record and ANVIL_BENCHMARK_EVENT_QUIT, on which anvil-game quits the
// game (docs/spec/benchmark-v1.md §2).
void quit_when_done() {
    const double ready_ms = ms_since_entry(g_runtime->ready);
    std::vector<std::string> waiting;
    for (;;) {
        waiting.clear();
        if (!g_runtime->plugins_loaded.load(std::memory_order_acquire)) waiting.push_back("(plugins still loading)");
        for (const auto& p : g_runtime->benchmark->participants()) {
            if (!p.done) waiting.push_back(p.id);
        }
        if (waiting.empty() || elapsed_ms() - ready_ms >= ANVIL_BENCHMARK_QUIT_LIMIT_MS) break;
        Sleep(20);
    }
    // The last write: the game ends its process without Anvil's exit handler
    // running (two lab starts), so nothing is written at exit.
    if (std::string error; !write_benchmark_record(benchmark_record("end"), error)) {
        log_warning("benchmark: cannot write the start record: " + error);
    }
    const std::string after = fmt_ms(elapsed_ms() - ready_ms) + " after ready";
    if (waiting.empty()) {
        log_info("benchmark: every participant is done " + after + "; quitting");
    } else {
        std::string list;
        for (const auto& id : waiting) list += (list.empty() ? "" : ", ") + id;
        log_warning("benchmark: still not done " + after + ": " + list + "; quitting anyway");
    }
    g_runtime->bus.publish(ANVIL_BENCHMARK_EVENT_QUIT, nullptr);
}

// The ready record, off the game's main thread: the labels need the event log.
// Then the wait for the participants, on the same thread.
DWORD WINAPI write_ready_record(LPVOID) {
    try {
        if (g_runtime->history_read.load(std::memory_order_acquire) && g_runtime->previous_start) {
            const StartHistory& previous = *g_runtime->previous_start;
            const StartHistory now = current_start_history(g_runtime->entry_filetime, g_runtime->entry_tick_ms);
            g_runtime->first_start_since_boot = rebooted_since(previous, now);
            if (const auto update = newest_definitions_update()) {
                g_runtime->definitions_changed = *update > previous.entry_filetime;
            }
        }
        std::string error;
        if (write_benchmark_record(benchmark_record("ready"), error)) {
            log_info("benchmark: start record written at ready (" + fmt_ms(elapsed_ms()) + ")");
        } else {
            log_warning("benchmark: cannot write the start record: " + error);
        }
    } catch (const std::exception& e) {
        log_warning(std::string("benchmark: cannot write the start record: ") + e.what());
    }
    try {
        quit_when_done();
    } catch (const std::exception& e) {
        log_warning(std::string("benchmark: ") + e.what() + "; quitting now");
        g_runtime->bus.publish(ANVIL_BENCHMARK_EVENT_QUIT, nullptr);
    }
    return 0;
}

// anvil-game.ready in a benchmark start: the time, the end of the first
// opens and of the input check, then the record.
void benchmark_ready() {
    QueryPerformanceCounter(&g_runtime->ready);
    g_runtime->first_opens.stop();
    g_runtime->input_during_start = input_since_entry();
    if (HANDLE t = CreateThread(nullptr, 0, write_ready_record, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        log_warning("benchmark: cannot start the thread that writes the start record");
    }
}

// The game is ready (anvil-game.ready): logged, and this start no longer
// counts for safe mode, unless it already wrote a crash report. Without
// anvil-game there is no such event.
void on_game_ready(const char*, const void*, void*) {
    if (g_runtime->arm) benchmark_ready();
    log_info("game ready at " + fmt_ms(elapsed_ms()));
    const bool reported = crash::reported();
    if (reported && g_runtime->start_marker.active()) {
        log_warning("the game is ready after a crash report; this start still counts for safe mode");
    }
    g_runtime->start_marker.game_ready(reported);
    crash::set_game_ready();
    std::string by;
    if (!crash::check_filter(by)) {
        log_warning("the unhandled-exception filter was replaced by " + by +
                    "; Anvil's crash reports depend on that filter calling the one it replaced");
    }
}

// ---- loading ---------------------------------------------------------------

// Empty when every required dependency loaded (they come earlier in the
// load order); otherwise why the plugin is not loaded either.
i18n::Message failed_dependency(const AnvilPlugin& plugin) {
    for (const auto& d : plugin.manifest.dependencies) {
        if (d.optional) continue;
        for (const auto& p : g_runtime->plugins) {
            if (p->manifest.id == d.id && p->state != "loaded") return i18n::Message("dependency_failed", {d.id});
        }
    }
    return {};
}

// The plugin and its modules leave the gameplay policy (a failed load).
void forget_in_policy(const AnvilPlugin& plugin) {
    g_runtime->policy->forget(plugin.owner);
    for (const auto& m : plugin.modules) g_runtime->policy->forget(m->owner);
}

void set_failed(AnvilPlugin& plugin, i18n::Message why) {
    plugin.reason = i18n::english(why);
    plugin.why = std::move(why);
    plugin.state = "failed";
}

void load_plugin(AnvilPlugin& plugin) {
    const double begin = elapsed_ms();
    const std::filesystem::path dll = std::filesystem::path(plugin.plugin_dir) / plugin.manifest.entry;
    const std::string label = plugin.manifest.id + " " + plugin.manifest.version.to_string();
    if (i18n::Message reason = failed_dependency(plugin); !reason.empty()) {
        set_failed(plugin, std::move(reason));
        forget_in_policy(plugin);
        log_error(label + ": not loaded; " + plugin.reason);
        return;
    }
    // A plugin from a mod: its approved copy must still have the approved
    // hash. Its files stay open without write or delete sharing until the
    // DLL is mapped, so nothing can change between the check and the load.
    FileLocks locks;
    if (!plugin.approved_hash.empty()) {
        const FolderHash now = hash_plugin_folder(plugin.plugin_dir, &locks);
        if (now.sha256 != plugin.approved_hash) {
            set_failed(plugin, i18n::Message("copy_changed", {now.error.empty() ? i18n::Message("hash", {short_hash(now.sha256)})
                                                                                : i18n::Message(now.error),
                                                              short_hash(plugin.approved_hash)}));
            forget_in_policy(plugin);
            log_error(label + ": not loaded; " + plugin.reason);
            return;
        }
    }
    plugin.module = LoadLibraryExW(dll.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    locks.release();
    if (plugin.module == nullptr) {
        const DWORD error = GetLastError();
        set_failed(plugin, i18n::Message("load_error", {plugin.manifest.entry, std::to_string(error)}));
        forget_in_policy(plugin);
        log_error(label + ": cannot load " + narrow(dll.wstring()) + " (error " + std::to_string(error) + ")");
        return;
    }
    const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(plugin.module, "anvil_plugin_load"));
    if (fn == nullptr) {
        set_failed(plugin, i18n::Message("no_load_export", {}));
        forget_in_policy(plugin);
        log_error(label + ": entry DLL does not export anvil_plugin_load");
        FreeLibrary(plugin.module);
        plugin.module = nullptr;
        return;
    }
    // Changes requested during anvil_plugin_load are staged and enabled together
    // after it returns ANVIL_OK; on failure none are (design §8.4).
    ChangeRegistry& changes = *g_runtime->changes;
    const LoadOutcome outcome = run_plugin_load(plugin, g_runtime->host, fn);
    if (outcome.commit_failed) {
        log_error(label + ": its " + std::to_string(outcome.staged) + " change(s) could not be enabled");
    }
    if (outcome.result != ANVIL_OK) {
        char code[32] = {};
        if (outcome.exception != 0) std::snprintf(code, sizeof(code), "0x%08lX", outcome.exception);
        set_failed(plugin, outcome.exception != 0 ? i18n::Message("load_exception", {code})
                                                  : i18n::Message("load_returned", {result_name(outcome.result)}));
        forget_in_policy(plugin);
        g_runtime->benchmark->forget(&plugin);
        log_error(label + ": " + plugin.reason + "; " + std::to_string(outcome.staged) +
                  " change(s) rolled back; plugin stays inactive");
        // The DLL is not unloaded: threads it may have started could still run.
        return;
    }
    plugin.state = "loaded";
    std::string summary = label + ": loaded (" + (plugin.manifest.phase == Phase::Early ? "early" : "main") + ", " +
                          fmt_ms(elapsed_ms() - begin) + ", " + std::to_string(changes.count(plugin.owner)) +
                          " change(s))";
    if (plugin.manifest.affects_gameplay) summary += " — declares gameplay changes";
    log_info(summary);
    for (const auto& line : changes.describe(plugin.owner)) {
        Log::instance().write(ANVIL_LOG_DEBUG, plugin.manifest.id, line);
    }
    for (const auto& m : plugin.modules) {
        if (!m->enabled) continue;
        log_info(m->manifest.id + ": module " + std::to_string(changes.count(m->owner)) + " change(s)" +
                 (m->manifest.affects_gameplay ? " — declares gameplay changes" : ""));
        for (const auto& line : changes.describe(m->owner)) {
            Log::instance().write(ANVIL_LOG_DEBUG, m->manifest.id, line);
        }
    }
}

void write_summary() {
    size_t loaded = 0, failed = 0;
    for (const auto& p : g_runtime->plugins) {
        if (p->state == "loaded") ++loaded;
        else if (p->state == "failed") ++failed;
    }
    log_info("status: " + std::to_string(loaded) + " loaded, " + std::to_string(failed) + " failed, " +
             std::to_string(g_runtime->skipped.size()) + " skipped");
    for (const auto& s : g_runtime->bus.describe()) log_info("service " + s);
}

DWORD WINAPI main_phase(LPVOID) {
    // Every start replaces the previous start's history, so the first
    // benchmark start after a normal one is not taken for the first since
    // boot (docs/spec/benchmark-v1.md §3). Off the game's main thread.
    const std::filesystem::path history = g_runtime->anvil_dir / "data" / "anvil" / "last-start.txt";
    g_runtime->previous_start = read_start_history(history);
    g_runtime->history_read.store(true, std::memory_order_release);
    for (auto& p : g_runtime->plugins) {
        if (p->manifest.phase == Phase::Main) load_plugin(*p);
    }
    // The end of the core start-up of design §17 (entry point -> end of loading).
    log_info("plugins loaded at " + fmt_ms(elapsed_ms()));
    g_runtime->plugins_loaded.store(true, std::memory_order_release);
    g_runtime->control->loading_done();
    g_runtime->bus.publish(ANVIL_EVENT_PLUGINS_LOADED, nullptr);
    if (std::string error; !write_file_atomic(
            history, start_history_text(current_start_history(g_runtime->entry_filetime, g_runtime->entry_tick_ms)),
            error)) {
        log_warning("cannot write " + narrow(history.wstring()) + ": " + error);
    }
    write_summary();
    if (g_runtime->build != nullptr) {
        const double begin = elapsed_ms();
        std::string actual;
        const bool ok = verify_build_file(*g_runtime->build, g_runtime->exe_path,
                                          g_runtime->anvil_dir / "data" / "anvil" / "build-cache.txt", actual);
        if (ok) {
            std::atomic_ref<const char*>(g_runtime->game.sha256).store(g_runtime->build->sha256);
            log_info("eu4.exe SHA-256 verified (" + fmt_ms(elapsed_ms() - begin) + ", after loading)");
        } else {
            log_warning("eu4.exe SHA-256 " + (actual.empty() ? std::string("could not be computed") : actual) +
                        " differs from the known build; every change is still checked against expected bytes");
        }
    }
    return 0;
}

using ExitProcessFn = void(WINAPI*)(UINT);
void* g_exit_next = nullptr;  // the rest of the chain, written by the registry

// The game's import of ExitProcess: its C runtime calls it when main returns
// (the menu's Exit, SDL_QUIT). The exit work runs here, while every thread
// still runs: ExitProcess then stops the other threads and detaches the DLLs,
// and a DLL that detaches before Anvil can end the process there (in the lab,
// a graphics driver behind the Epic overlay's detach; anvil#164).
void WINAPI exit_process(UINT code) {
    shutdown();
    reinterpret_cast<ExitProcessFn>(g_exit_next)(code);
}

void startup_impl(const std::filesystem::path& game_dir) {
    auto runtime = std::make_unique<Runtime>();
    // Time 0 of every "at ... ms" in the log: AnvilEntry, before the game's
    // C runtime starts (the start-up budget of design §17 counts from here).
    QueryPerformanceFrequency(&runtime->frequency);
    QueryPerformanceCounter(&runtime->start);
    // The same moment on the system clock and the tick counters, for the
    // start-up benchmark's record and labels (docs/spec/benchmark-v1.md §6).
    FILETIME entry{};
    GetSystemTimePreciseAsFileTime(&entry);
    runtime->entry_filetime = (static_cast<uint64_t>(entry.dwHighDateTime) << 32) | entry.dwLowDateTime;
    runtime->entry_tick_ms = GetTickCount64();
    runtime->entry_tick = GetTickCount();
    runtime->game_dir = game_dir;
    runtime->anvil_dir = game_dir / "anvil";
    g_runtime = runtime.release();  // lives for the process

    if (!std::filesystem::is_directory(g_runtime->anvil_dir)) return;  // not installed: pure proxy, no files
    Log::instance().open(g_runtime->anvil_dir / "anvil.log");
    log_info(std::string("Anvil ") + kAnvilVersion + " (plugin API " + std::to_string(kManifestApi) + ")");

    wchar_t disable[8] = {};
    if (GetEnvironmentVariableW(L"ANVIL_DISABLE", disable, 8) == 1 && disable[0] == L'1') {
        log_info("ANVIL_DISABLE=1: no plugins loaded");
        return;
    }
    const AnvilSettings settings = read_anvil_settings(g_runtime->anvil_dir / "anvil.toml");
    if (!settings.error.empty()) log_warning("anvil.toml ignored: " + settings.error);
    Log::instance().set_min_level(settings.log_level);
    if (!settings.enabled) {
        log_info("disabled in anvil.toml: no plugins loaded");
        return;
    }

    // A benchmark start (docs/spec/benchmark-v1.md §2): the arm is in the
    // environment the Manager started the game with; anvil.toml is never
    // written for it.
    std::optional<BenchmarkArm>& arm = g_runtime->arm;
    {
        wchar_t text[96] = {};
        const DWORD n = GetEnvironmentVariableW(kBenchmarkVariable, text, static_cast<DWORD>(std::size(text)));
        if (n > 0) {
            std::string error;
            const std::string value = n < std::size(text) ? narrow(text) : std::string("(too long)");
            arm = n < std::size(text) ? parse_benchmark_arm(value, error) : std::nullopt;
            if (arm) {
                log_info("benchmark start: arm " + arm->text());
                // A start that never reaches ready leaves no record, not the last one's.
                std::error_code ignored;
                std::filesystem::remove(benchmark_record_file(), ignored);
            } else {
                log_warning("ANVIL_BENCHMARK=" + value + " ignored, so this is not a benchmark start: " +
                            (error.empty() ? std::string("too long") : error));
            }
        }
    }

    HMODULE exe = GetModuleHandleW(nullptr);
    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(exe, exePath, static_cast<DWORD>(std::size(exePath)));
    g_runtime->exe_path = exePath;
    const auto identity = read_pe_identity(exe);
    if (!identity) {
        log_error("cannot read the game executable header; no plugins loaded");
        return;
    }
    g_runtime->identity = *identity;
    g_runtime->build = find_known_build(*identity);
    g_runtime->changes = std::make_unique<ChangeRegistry>(identity->image_base, identity->size_of_image);
    g_runtime->context.changes = g_runtime->changes.get();
    g_runtime->context.bus = &g_runtime->bus;
    g_runtime->context.image = exe;
    if (settings.profile) {
        g_runtime->profile = true;
        g_runtime->changes->set_profile(true);
        log_info("profile = true: calls per change are counted and written here when the game exits");
    }
    g_runtime->changes->set_refusal_sink([](OwnerId owner, const std::string& message) {
        // Names both owners of a conflict; the host call adds its own line.
        // Owner ids are the indices of g_runtime->plugins (both in load order);
        // the core's own owner comes after them.
        Log::instance().write(ANVIL_LOG_WARNING, owner_name(owner), message);
    });
    g_runtime->changes->set_notice_sink([](OwnerId owner, const std::string& message) {
        // On the game's thread, e.g. the first mid hook per address that changes rip.
        Log::instance().write(ANVIL_LOG_INFO, owner_name(owner), message);
    });

    // Safe mode (design §10): counted before anything can crash.
    const StartRecord record =
        g_runtime->start_marker.begin(g_runtime->anvil_dir / "data" / "anvil" / "unfinished-starts.txt");
    if (record.safe_mode) {
        log_warning("safe mode: the last " + std::to_string(record.unfinished) +
                    " starts ended before the game was ready (a crash, a hang or a killed process); no plugins "
                    "are loaded on this start, the next one loads them again (reports: anvil/crashes)");
    } else if (record.unfinished > 0) {
        log_warning(std::to_string(record.unfinished) + " previous start(s) ended before the game was ready; safe mode "
                    "after " + std::to_string(kSafeModeAfter));
    }
    // The newest crash report of an earlier start, for the overlay; and in
    // safe mode the reports of the starts that led to it: a standard plugin
    // the crash was in is left out, so the safe start cannot end the same way.
    const std::filesystem::path crash_dir = g_runtime->anvil_dir / "crashes";
    const std::vector<std::string> crash_names = crash_report_names(crash_dir);
    std::vector<CrashSummary> safe_mode_reports;
    if (record.safe_mode) safe_mode_reports = safe_mode_crashes(crash_dir, record.unfinished);
    // Crash capture (design §12): the vectored handler, the filter ahead of
    // the game's and the helper thread, before any plugin code runs.
    crash::Setup capture;
    capture.crash_dir = crash_dir;
    capture.log_file = g_runtime->anvil_dir / "anvil.log";
    capture.image = exe;
    capture.sources.changes = g_runtime->changes.get();
    capture.sources.plugins = &g_runtime->plugins;
    capture.sources.image_base = identity->image_base;
    capture.sources.image_size = identity->size_of_image;
    g_runtime->exe_name = narrow(g_runtime->exe_path.filename().wstring());
    capture.sources.image_name = g_runtime->exe_name.c_str();
    capture.sources.build_id = g_runtime->build ? g_runtime->build->id : nullptr;
    capture.sources.anvil_base = reinterpret_cast<uintptr_t>(&__ImageBase);
    capture.anvil_version = kAnvilVersion;
    capture.start = g_runtime->start;
    capture.unfinished_starts = record.unfinished;
    capture.safe_mode = record.safe_mode;
    if (std::string error; !crash::install(capture, error)) log_warning("crash capture: " + error);
    if (const ULONG guarantee = crash::thread_guarantee(); guarantee != 0) {
        log_info("crash capture: the CPU state saved with an exception does not fit in a thread's default stack "
                 "guarantee (AMX); new threads with 256 KB of stack or more get " +
                 std::to_string(guarantee / 1024) + " KB");
    }
    AnvilGameInfo& game = g_runtime->game;
    game.size = sizeof(AnvilGameInfo);
    game.build_id = g_runtime->build ? g_runtime->build->id : nullptr;
    game.image_base = identity->image_base;
    game.image_size = identity->size_of_image;
    game.sha256 = nullptr;  // set when the file hash is verified, after loading
    g_runtime->host = make_host(&game);
    // The game's readiness and its sessions come from anvil-game (design §4,
    // §13); the core only listens. Its subscriptions come first, so the
    // policy acts before any plugin hears of a session.
    g_runtime->policy = std::make_unique<GameplayPolicy>(
        *g_runtime->changes, g_runtime->bus, GameplayOptIns{settings.allow_ironman, settings.allow_multiplayer},
        [](AnvilLogLevel level, const std::string& id, const std::string& message) {
            Log::instance().write(level, id, message);
        });
    g_runtime->policy->subscribe();
    g_runtime->bus.subscribe("anvil", ANVIL_GAME_EVENT_READY, on_game_ready, nullptr);
    char header[160];
    std::snprintf(header, sizeof(header), "timestamp 0x%08X, image 0x%X, checksum 0x%08X, entry 0x%X",
                  identity->pe_timestamp, identity->size_of_image, identity->pe_checksum, identity->entry_point_rva);
    log_info(std::string("game build: ") + (game.build_id ? game.build_id : "unknown") + " (" + header + ")");
    // For reports from Linux: the Wine and the Direct3D 9 the game runs on
    // (eu4.exe imports d3d9.dll, so it is loaded by now). Log only.
    log_info(wine_log_line(detect_wine()));
    log_info(d3d9_log_line(GetModuleHandleW(L"d3d9.dll"), game_dir));
    const i18n::Message unknown_build = game.build_id ? i18n::Message() : unknown_build_reason(game_dir);
    if (unknown_build.code == "build_unknown_gog") log_warning(i18n::english(unknown_build));

    // Discovery: anvil/plugins/<folder>/plugin.toml
    ResolveInput input;
    input.host_api = kManifestApi;
    input.host_version = Version::parse(kAnvilVersion);
    input.game_build = game.build_id;
    input.disabled = settings.disabled_plugins;
    if (arm && !arm->with) input.benchmark_without = arm->plugin;
    // symbols.toml: its build and every symbol are checked here, before any
    // plugin is loaded. The image is read through the registry, so bytes a
    // plugin has already changed read in their original form (design §7).
    // The verified symbols are kept per plugin folder for address_of and the
    // symbol change calls.
    std::map<std::wstring, PluginSymbols> verified_symbols;
    // settings.toml: a plugin whose declarations do not parse is skipped, as
    // it could not read its settings (design §11).
    std::map<std::wstring, SettingsFile> declared_settings;
    input.verify = [&](const Manifest& m) {
        if (!m.symbols.empty()) {
            SymbolVerification verified;
            SymbolFile declared;
            i18n::Message reason = check_plugin_symbols(m.folder / m.symbols, game.build_id, *g_runtime->changes,
                                                        identity->image_base, &verified, &declared, unknown_build);
            if (!reason.empty()) return reason;
            verified_symbols[m.folder.wstring()] = PluginSymbols(declared, verified, identity->image_base);
            log_info(m.id + ": " + std::to_string(verified.symbols.size()) + " symbol(s) verified");
        }
        if (!m.settings.empty()) {
            SettingsFileResult parsed = load_settings(m.folder / m.settings);
            if (!parsed.file) return i18n::Message(parsed.error);
            declared_settings[m.folder.wstring()] = std::move(*parsed.file);
        }
        return i18n::Message();
    };
    const auto plugins_dir = g_runtime->anvil_dir / "plugins";
    const std::vector<std::filesystem::path> folders = plugin_folders(plugins_dir);
    // A local plugin wins over a plugin with its id in any mod (design §14),
    // also when it is disabled or broken (then by its folder's name).
    std::set<std::string> local_ids;
    for (const auto& folder : folders) {
        const auto parsed = load_manifest(folder);
        local_ids.insert(parsed.manifest ? parsed.manifest->id : narrow(folder.filename().wstring()));
        if (!parsed.manifest) {
            g_runtime->skipped.push_back(skipped_for(narrow(folder.filename().wstring()), parsed.error, folder));
            continue;
        }
        if (i18n::Message reason = missing_entry_reason(*parsed.manifest); !reason.empty()) {
            g_runtime->skipped.push_back(skipped_for(parsed.manifest->id, std::move(reason), folder));
            continue;
        }
        if (record.safe_mode && !loads_in_safe_mode(parsed.manifest->id)) {
            g_runtime->skipped.push_back(
                skipped_for(parsed.manifest->id, i18n::Message("safe_mode", {}), folder, SkipKind::safe_mode));
            continue;
        }
        if (record.safe_mode) {
            if (i18n::Message reason = safe_mode_exclusion(parsed.manifest->id, safe_mode_reports); !reason.empty()) {
                g_runtime->skipped.push_back(skipped_for(parsed.manifest->id, std::move(reason), folder, SkipKind::safe_mode));
                continue;
            }
        }
        input.manifests.push_back(*parsed.manifest);
    }
    log_info("discovered " + std::to_string(folders.size()) + " plugin folder(s) in " + narrow(plugins_dir.wstring()));

    // Plugins in the mods of the playset (design §14): only approved ones
    // load, always from their approved copy, whose hash is checked again
    // right before LoadLibrary (load_plugin).
    std::map<std::wstring, std::string> approved_hashes;  // by the copy's folder
    // The game's own choice: -userdir first (a private profile, as Etch's live game has).
    std::filesystem::path user_dir = eu4_user_dir(game_dir);
    if (const auto named = user_dir_argument(GetCommandLineW())) {
        std::error_code ec;
        if (std::filesystem::is_directory(*named, ec)) {
            user_dir = *named;
            log_info("EU4's user folder from -userdir: " + narrow(user_dir.wstring()));
        }
    }
    {
        g_runtime->mods = discover_mod_plugins(g_runtime->anvil_dir, user_dir, local_ids);
        ModDiscovery& mods = g_runtime->mods;
        for (const auto& w : mods.warnings) log_warning(w);
        prune_approved_copies(g_runtime->anvil_dir, mods.trust);
        std::map<std::string, std::vector<std::string>> shadowed;  // id -> the mods' names
        for (ModPlugin& p : mods.plugins) {
            const std::string label = p.id + " from mod '" + p.mod.name + "' (" +
                                      (p.mod.workshop_id.empty() ? std::string("local mod") : "Workshop " + p.mod.workshop_id) + ")";
            const i18n::Message source = p.mod.workshop_id.empty() ? i18n::Message("local_mod", {})
                                                                   : i18n::Message("workshop_item", {p.mod.workshop_id});
            const auto skip = [&](const i18n::Message& reason, SkipKind kind = SkipKind::other) {
                Skipped s = skipped_for(p.id, i18n::Message("from_mod", {p.mod.name, source, reason}), p.folder);
                s.mod = p.mod.name;
                s.awaiting_approval = !p.loads() && (p.state == ModPluginState::Pending || p.state == ModPluginState::Conflict);
                s.kind = kind;
                g_runtime->skipped.push_back(std::move(s));
            };
            if (p.state == ModPluginState::Shadowed) {
                shadowed[p.id].push_back(p.mod.name);
                continue;
            }
            if (!p.loads()) {
                skip(p.why, p.state == ModPluginState::Pending    ? SkipKind::approval
                               : p.state == ModPluginState::Conflict ? SkipKind::conflict
                                                                      : SkipKind::other);
                continue;
            }
            if (record.safe_mode) {
                skip(i18n::Message("safe_mode", {}), SkipKind::safe_mode);
                continue;
            }
            if (p.copy_missing) {
                if (std::string error; !make_approved_copy(g_runtime->anvil_dir, p, error)) {
                    skip(i18n::Message("copy_failed", {error}));
                    continue;
                }
                log_info(label + ": made the approved copy");
            }
            log_info(label + ": " + p.reason + "; loads from " + narrow(p.load_folder.wstring()));
            const auto parsed = load_manifest(p.load_folder);
            if (!parsed.manifest) {
                skip(i18n::Message("copy_broken", {parsed.error}));
                continue;
            }
            if (i18n::Message reason = missing_entry_reason(*parsed.manifest); !reason.empty()) {
                skip(i18n::Message("copy_broken", {reason}));
                continue;
            }
            approved_hashes[p.load_folder.wstring()] = p.approval->hash;
            input.manifests.push_back(*parsed.manifest);
        }
        for (const auto& [id, names] : shadowed) {
            std::string list;
            for (const auto& n : names) list += (list.empty() ? "'" : ", '") + n + "'";
            log_info(id + ": the local plugin in anvil/plugins wins over the same id in mod(s) " + list);
        }
        if (!mods.plugins.empty() || !mods.playset.mods.empty()) {
            log_info("found " + std::to_string(mods.plugins.size()) + " plugin(s) in " +
                     std::to_string(mods.playset.mods.size()) + " enabled mod(s) at " + fmt_ms(elapsed_ms()));
        }
    }
    ResolveResult resolved = resolve(input);
    g_runtime->skipped.insert(g_runtime->skipped.end(), resolved.skipped.begin(), resolved.skipped.end());
    for (const auto& s : g_runtime->skipped) log_warning(s.id + ": skipped — " + s.reason);
    log_info("resolved " + std::to_string(resolved.order.size()) + " plugin(s) to load at " + fmt_ms(elapsed_ms()));
    // The overlay speaks the language of EU4's settings, or with Mei, which
    // shows a translation mod's Chinese, Japanese or Korean, Steam's or
    // Windows' (#158).
    const bool cjk_text = std::any_of(resolved.order.begin(), resolved.order.end(),
                                      [](const Manifest& m) { return m.id == i18n::kCjkTextPlugin; });
    const i18n::LanguageChoice language = i18n::choose_language(user_dir, cjk_text);
    const std::string player_language = i18n::language_code(language.language);
    log_info("the player's language: " + player_language + " (" + language.source + ")");
    // No reallocation later: the crash writer may read the list from another thread.
    g_runtime->plugins.reserve(resolved.order.size());
    for (auto& m : resolved.order) {
        auto plugin = std::make_unique<AnvilPlugin>();
        plugin->context = &g_runtime->context;
        // Owners in load order: the earlier plugin is outermost in every chain.
        plugin->owner = g_runtime->changes->add_owner(m.id);
        if (const auto it = verified_symbols.find(m.folder.wstring()); it != verified_symbols.end()) {
            plugin->symbols = std::move(it->second);
        }
        plugin->plugin_dir = m.folder.wstring();
        if (const auto it = approved_hashes.find(m.folder.wstring()); it != approved_hashes.end()) {
            plugin->approved_hash = it->second;
        }
        plugin->data_dir = (g_runtime->anvil_dir / "data" / m.id).wstring();
        // The player's values and the ANVIL_<ID>_<NAME> overrides, before any plugin loads.
        if (const auto it = declared_settings.find(m.folder.wstring()); it != declared_settings.end()) {
            const auto notes = plugin->settings.init(std::move(it->second),
                                                     std::filesystem::path(plugin->data_dir) / "settings.toml", m.id);
            for (const auto& note : notes) Log::instance().write(note.level, m.id, note.message);
        }
        plugin->manifest = std::move(m);
        g_runtime->policy->add(plugin->manifest, plugin->owner);
        // Its modules (ADR 0011): owners right after the plugin's, and a
        // policy entry for each module that is on at this start.
        add_modules(*plugin, *g_runtime->changes, [&](const std::string& key, bool by_default) {
            return module_enabled(settings, key, by_default);
        });
        for (const auto& mod : plugin->modules) {
            if (mod->enabled) {
                g_runtime->policy->add(mod->manifest, mod->owner);
            } else {
                log_info(mod->manifest.id + ": module off at this start (anvil.toml)");
            }
        }
        g_runtime->plugins.push_back(std::move(plugin));
    }
    // The core's own changes: its owner comes after every plugin's, so each
    // is the innermost link of its chain.
    const OwnerId core = g_runtime->changes->add_owner("anvil");
    if (const ChangeStatus s = g_runtime->changes->replace_import(core, exe, "KERNEL32.dll", "ExitProcess",
                                                                  reinterpret_cast<void*>(&exit_process), &g_exit_next);
        s != ChangeStatus::Ok) {
        log_warning(std::string("cannot replace the game's ExitProcess (") + result_name(to_result(s)) +
                    "); the exit work waits for DLL_PROCESS_DETACH, which a normal exit may skip");
    }
    // A benchmark start times the game's first opens of its files until
    // ready; the innermost link times the call itself.
    if (arm) {
        const ChangeStatus s = install_first_open_meter(*g_runtime->changes, core, exe, g_runtime->first_opens);
        g_runtime->first_opens_metered = s == ChangeStatus::Ok;
        if (!g_runtime->first_opens_metered) {
            log_warning(std::string("benchmark: cannot time the game's first opens (CreateFileW): ") +
                        result_name(to_result(s)));
        }
    }

    // anvil.control (src/control/anvil_control.h): what the overlay shows and
    // changes. Registered before any plugin loads; the overlay is early.
    ControlSources sources;
    sources.anvil_dir = g_runtime->anvil_dir;
    sources.anvil_version = kAnvilVersion;
    sources.game_build = game.build_id;
    sources.plugins = &g_runtime->plugins;
    sources.skipped = &g_runtime->skipped;
    sources.policy = g_runtime->policy.get();
    sources.disabled = settings.disabled_plugins;
    sources.safe_mode = record.safe_mode;
    sources.unfinished_starts = record.unfinished;
    if (!crash_names.empty()) sources.previous_crash = crash_names.front();
    sources.mods = &g_runtime->mods;
    sources.language = player_language;
    // The plugins' own texts in that language (design §5.4). A file that
    // does not parse is left out with a warning; the plugin loads as before.
    const auto translation = [&](const std::filesystem::path& folder, const std::string& id) {
        PluginTranslationResult t = load_plugin_translation(folder, player_language);
        if (!t.error.empty()) {
            log_warning(id + ": " + kTranslationsFolder + "/" + player_language + ".toml" +
                        (t.line != 0 ? ":" + std::to_string(t.line) : std::string()) + " ignored: " + t.error);
        }
        return t.translation;
    };
    if (player_language != "en") {
        for (const auto& p : g_runtime->plugins) {
            if (auto t = translation(p->manifest.folder, p->manifest.id)) sources.translations.emplace(p->manifest.id, std::move(*t));
        }
        for (uint32_t i = 0; i < g_runtime->mods.plugins.size(); ++i) {
            const ModPlugin& p = g_runtime->mods.plugins[i];
            if (!p.manifest) continue;
            if (auto t = translation(p.folder, p.id)) sources.mod_translations.emplace(i, std::move(*t));
        }
    }
    g_runtime->control = std::make_unique<Control>(std::move(sources));
    if (g_runtime->bus.register_service("anvil", ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION,
                                        g_runtime->control->service()) != ANVIL_OK) {
        log_warning("cannot register " ANVIL_CONTROL_SERVICE);
    }
    // anvil.benchmark: plugins declare their fields inside anvil_plugin_load,
    // so it is there before any plugin loads too.
    g_runtime->benchmark = std::make_unique<Benchmark>(arm.has_value(), [](const AnvilPlugin& plugin, const std::string& message) {
        Log::instance().write(ANVIL_LOG_WARNING, plugin.manifest.id, message);
    });
    if (g_runtime->bus.register_service("anvil", ANVIL_BENCHMARK_SERVICE, ANVIL_BENCHMARK_SERVICE_VERSION,
                                        g_runtime->benchmark->service()) != ANVIL_OK) {
        log_warning("cannot register " ANVIL_BENCHMARK_SERVICE);
    }

    // Early plugins: here, before the game's runtime starts.
    for (auto& p : g_runtime->plugins) {
        if (p->manifest.phase == Phase::Early) load_plugin(*p);
    }
    log_info("early phase done at " + fmt_ms(elapsed_ms()));

    if (HANDLE t = CreateThread(nullptr, 0, main_phase, nullptr, 0, nullptr)) {
        CloseHandle(t);
    } else {
        log_error("cannot start the main-phase thread; main plugins not loaded");
    }
}

// The call counters (profile = true), one line per change under its plugin.
void write_call_counts() {
    std::vector<CallCount> counts;
    if (!g_runtime->changes->call_counts(counts, false)) {
        Log::instance().try_write(ANVIL_LOG_WARNING, "anvil", "call counters: the registry was busy at exit; not written");
        return;
    }
    if (!Log::instance().try_write(ANVIL_LOG_INFO, "anvil",
                                   "call counters at exit (profile = true): " + std::to_string(counts.size()) +
                                       " counted change(s)")) {
        return;
    }
    const auto& plugins = g_runtime->plugins;
    for (const CallCount& c : counts) {
        const std::string& id = c.owner < plugins.size() ? plugins[c.owner]->manifest.id : std::string("?");
        if (!Log::instance().try_write(ANVIL_LOG_INFO, id, "calls: " + describe_call_count(c))) return;
    }
}

}  // namespace

PluginSettings* plugin_settings(std::string_view plugin_id) {
    if (g_runtime == nullptr) return nullptr;
    for (const auto& p : g_runtime->plugins) {
        if (p->manifest.id == plugin_id) return &p->settings;
    }
    return nullptr;
}

void shutdown() {
    // Once: from the game's ExitProcess, and again from DLL_PROCESS_DETACH if it comes.
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    try {
        if (g_runtime != nullptr && g_runtime->changes && g_runtime->profile) write_call_counts();
        // Closed before the game was ready, without a crash: not counted.
        if (g_runtime != nullptr && g_runtime->start_marker.active() && !crash::crashed()) {
            g_runtime->start_marker.clean_exit();
        }
    } catch (...) {
        // Never throw out of ExitProcess or DllMain.
    }
}

void startup(const std::filesystem::path& game_dir) {
    try {
        startup_impl(game_dir);
    } catch (const std::exception& e) {
        log_error(std::string("startup aborted: ") + e.what());
    } catch (...) {
        log_error("startup aborted by an unknown exception");
    }
}

}  // namespace anvil
