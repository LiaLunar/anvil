// Crash capture tests (design §10, §12): attribution of addresses to changes
// and plugins, the stack walk through Anvil's stubs, the four levels, the
// report text, the safe-mode start marker, and real crashes in a child
// process (this executable with --child), which must write the report, chain
// to the filter it set itself and, on a stack overflow, a minidump.
//
// Usage: anvil_crash_tests              (runs everything)
//        anvil_crash_tests --child <mode> <dir>   (internal)
#include "core/changes.hpp"
#include "core/crash.hpp"
#include "core/crash_history.hpp"
#include "core/crash_report.hpp"
#include "core/log.hpp"
#include "core/safe_mode.hpp"
#include "plugin_set_fixture.hpp"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

// Executable scratch memory standing in for eu4.exe.
struct Scratch {
    uint8_t* base;
    size_t size;
    explicit Scratch(size_t bytes) : size(bytes) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        std::memset(base, 0xCC, bytes);
    }
    ~Scratch() { VirtualFree(base, 0, MEM_RELEASE); }
    uintptr_t at(size_t offset) const { return reinterpret_cast<uintptr_t>(base) + offset; }
    void put(size_t offset, const std::vector<uint8_t>& code) {
        std::memcpy(base + offset, code.data(), code.size());
        FlushInstructionCache(GetCurrentProcess(), base + offset, code.size());
    }
};

// int f(int x) { return x + 1; }: lea eax, [rcx+1]; 9x nop; ret
const std::vector<uint8_t> kAddOne = {0x8D, 0x41, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90,
                                      0x90, 0x90, 0x90, 0x90, 0xC3};
using Fn = int (*)(int);

ChangeTarget in_image(uintptr_t address) { return {address, true, nullptr, nullptr, 0}; }

Fn g_next_a = nullptr, g_next_b = nullptr;
int detour_a(int x) { return g_next_a(x) + 100; }
int detour_b(int x) { return g_next_b(x) * 2; }

void** slot(Fn& next) { return reinterpret_cast<void**>(&next); }

uintptr_t stub_target(void* next) {
    uintptr_t target = 0;
    std::memcpy(&target, static_cast<uint8_t*>(next) + 8, sizeof(target));
    return target;
}

// This executable as a plugin's entry DLL (plugin "crasher" below), so a crash
// in its code is a crash in the plugin's code.
extern "C" __declspec(dllexport) AnvilResult anvil_plugin_load(const AnvilHost*, AnvilPlugin*) { return ANVIL_OK; }

fs::path this_executable() {
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    return self;
}

// A plugin of the start with plugin.toml's `extra`; its entry is mod-probe's
// DLL, so it loads, unless `entry` names another (an absolute path).
anvil::ResolvedPlugin& add_plugin(TestStart& start, const std::string& id, const std::string& extra = "",
                                  const fs::path& entry = mod_probe_dir() / "mod-probe.dll") {
    anvil::ResolvedPlugin& r = start.add(plugin_toml(id, extra));
    r.manifest.entry = narrow(entry.wstring());
    return r;
}

// One symbol `name` at `rva` with `size` declared bytes.
PluginSymbols symbols(uintptr_t base, const char* name, uint32_t rva, size_t size) {
    SymbolFile file;
    Symbol s;
    s.name = name;
    s.kind = SymbolKind::Function;
    s.rva = rva;
    s.bytes.assign(size, 0x90);
    s.mask.assign(size, 0xFF);
    file.symbols.push_back(s);
    SymbolVerification verified;
    verified.symbols.push_back({rva, {}});
    return PluginSymbols(file, verified, base);
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// A strict enough JSON check: one value, strings, numbers, literals, nesting.
struct Json {
    std::string_view s;
    size_t i = 0;
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool literal(std::string_view word) {
        if (s.substr(i, word.size()) != word) return false;
        i += word.size();
        return true;
    }
    bool string() {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') return ++i, true;
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++i >= s.size()) return false;
                if (s[i] == 'u') {
                    if (i + 4 >= s.size()) return false;
                    i += 4;
                } else if (std::string_view("\"\\/bfnrt").find(s[i]) == std::string_view::npos) {
                    return false;
                }
            }
        }
        return false;
    }
    bool number() {
        const size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+')) ++i;
        return i > start;
    }
    bool value() {
        ws();
        if (i >= s.size()) return false;
        if (s[i] == '{') {
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') return ++i, true;
            for (;;) {
                ws();
                if (!string()) return false;
                ws();
                if (i >= s.size() || s[i++] != ':') return false;
                if (!value()) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                return i < s.size() && s[i++] == '}';
            }
        }
        if (s[i] == '[') {
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') return ++i, true;
            for (;;) {
                if (!value()) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                return i < s.size() && s[i++] == ']';
            }
        }
        if (s[i] == '"') return string();
        if (literal("true") || literal("false") || literal("null")) return true;
        return number();
    }
    static bool valid(std::string_view text) {
        Json j{text};
        if (!j.value()) return false;
        j.ws();
        return j.i == text.size();
    }
};

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// ---- a mid hook that walks its own stack -------------------------------------

Frame g_walked[32];
size_t g_walked_count = 0;
const CrashSources* g_walk_sources = nullptr;

void walk_here(AnvilCpuContext*, void*) {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    g_walked_count = walk_stack(ctx, reinterpret_cast<uintptr_t>(tib->StackLimit),
                                reinterpret_cast<uintptr_t>(tib->StackBase), *g_walk_sources, g_walked, 32);
}

__declspec(noinline) int call_scratch(Fn f, int x) { return f(x) + 1; }

void quiet(AnvilCpuContext*, void*) {}

// ---- the child process ---------------------------------------------------------

volatile int* volatile g_null = nullptr;
volatile bool g_stop = false;
fs::path g_child_dir;

__declspec(noinline) void crash_here() { *g_null = 1; }

// A plugin's mid-hook callback that crashes: the walk leaves it through the
// context stub.
void crash_in_callback(AnvilCpuContext*, void*) { crash_here(); }

// The size of a CONTEXT with every extended state the host enables: what
// Windows puts on the crashed thread's stack before it calls any handler.
DWORD exception_context_bytes() {
    DWORD length = 0;
    InitializeContext2(nullptr, CONTEXT_ALL | CONTEXT_XSTATE, nullptr, &length, GetEnabledXStateFeatures());
    return length;
}

// The stack guarantee a thread has: SetThreadStackGuarantee with 0 reads it.
ULONG current_guarantee() {
    ULONG size = 0;
    SetThreadStackGuarantee(&size);
    return size;
}

// Runs `body` on a thread with a stack reservation of `reserve` bytes.
DWORD on_thread(SIZE_T reserve, DWORD(WINAPI* body)(LPVOID)) {
    HANDLE thread = CreateThread(nullptr, reserve, body, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) return 0;
    WaitForSingleObject(thread, 60000);
    DWORD result = 0;
    GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    return result;
}

__declspec(noinline) int recurse(int depth) {
    volatile char pad[256];
    pad[0] = static_cast<char>(depth);
    if (g_stop) return pad[0];
    return recurse(depth + 1) + pad[0];
}

// The game's own filter in the child: proves Anvil's filter called it.
LONG WINAPI child_next_filter(EXCEPTION_POINTERS*) {
    const HANDLE file = CreateFileW((g_child_dir / "next-filter.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    TerminateProcess(GetCurrentProcess(), 0xC0DE);
    return EXCEPTION_EXECUTE_HANDLER;
}

// As EU4's filter after a crash on one of its other threads: it starts its
// crash reporter and keeps the crashed thread, while the main thread goes on
// to the menu (seen in the game, #82).
HANDLE g_handed_over = nullptr;
LONG WINAPI child_keep_running_filter(EXCEPTION_POINTERS*) {
    SetEvent(g_handed_over);
    Sleep(INFINITE);
    return EXCEPTION_EXECUTE_HANDLER;
}

// The non-pending minidumps in the crash folder.
size_t kept_dumps(const fs::path& crashes) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(crashes)) {
        if (entry.path().extension() == L".dmp" && entry.path().filename().wstring().rfind(L"pending-", 0) != 0 &&
            fs::file_size(entry.path()) > 0) {
            ++count;
        }
    }
    return count;
}

int child(const std::string& mode, const fs::path& dir) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (mode == "dllmain") {
        // version.dll in a game process: from then on, new threads get the
        // guarantee this host needs (none without AMX), unless their stack is
        // under 256 KB.
        wchar_t dll[MAX_PATH * 2] = {};
        GetEnvironmentVariableW(L"ANVIL_TEST_VERSION_DLL", dll, static_cast<DWORD>(std::size(dll)));
        const auto guarantee = [](LPVOID) -> DWORD { return current_guarantee(); };
        const DWORD before = on_thread(256 * 1024, guarantee);
        SetEnvironmentVariableW(L"ANVIL_ANY_EXE", L"1");
        if (LoadLibraryExW(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr) return 15;
        const ULONG expected = crash::thread_guarantee();
        const DWORD large = on_thread(256 * 1024, guarantee);
        const DWORD small = on_thread(128 * 1024, guarantee);
        std::printf("expected %lu; default %lu, 256 KB thread %lu, 128 KB thread %lu\n", expected, before, large, small);
        const bool ok = (expected == 0 ? large == before : large >= expected) && small == before;
        return ok ? 0 : 16;
    }
    g_child_dir = dir;
    static Scratch image(0x10000);
    image.put(0x100, kAddOne);
    image.put(0x200, kAddOne);
    image.put(0x300, kAddOne);
    // The plugins of the start: crasher, whose DLL is this executable, and
    // patcher, which declares the symbol it patches.
    static TestStart& plugins = *new TestStart(image.at(0), 0x10000);
    add_plugin(plugins, "crasher", "issues = \"https://example.org/crasher/issues\"\n", this_executable());
    add_plugin(plugins, "patcher", "issues = \"https://example.org/patcher/issues\"\n").symbols =
        symbols(image.at(0), "add_one", 0x200, kAddOne.size());
    plugins.make();
    if (plugins.load("crasher") == nullptr || plugins.load("patcher") == nullptr) return 17;
    ChangeRegistry& registry = plugins.changes;
    const OwnerId crasher = plugins.handle("crasher")->owner;
    const OwnerId patcher = plugins.handle("patcher")->owner;
    const uint8_t ud2[] = {0x0F, 0x0B};
    if (registry.patch_bytes(patcher, in_image(image.at(0x200)), ud2, sizeof(ud2)) != ChangeStatus::Ok) return 10;
    if (mode == "mid") {
        // Mid hooks at two sites share the context stub; the crash is under the second.
        if (registry.mid_hook(patcher, in_image(image.at(0x300)), quiet, nullptr) != ChangeStatus::Ok) return 15;
        if (registry.mid_hook(crasher, in_image(image.at(0x100)), crash_in_callback, nullptr) != ChangeStatus::Ok) {
            return 16;
        }
    }

    std::ofstream(dir / "anvil.log") << "[    1.0 ms] info  [anvil] line one\r\n[    2.0 ms] info  [anvil] line two\r\n";
    crash::Setup setup;
    setup.crash_dir = dir / "crashes";
    setup.log_file = dir / "anvil.log";
    setup.image = GetModuleHandleW(nullptr);
    setup.sources.changes = &registry;
    setup.sources.plugins = &plugins.set();
    setup.sources.image_base = image.at(0);
    setup.sources.image_size = 0x10000;
    setup.sources.image_name = "game.exe";
    setup.sources.build_id = "test-build";
    setup.anvil_version = "9.9.9";
    QueryPerformanceCounter(&setup.start);
    // The start marker as the runtime keeps it: counted in before install.
    static StartMarker marker;
    const StartRecord start = marker.begin(dir / "unfinished-starts.txt");
    setup.unfinished_starts = start.unfinished;
    std::string error;
    if (!crash::install(setup, error)) {
        std::printf("install: %s\n", error.c_str());
        return 11;
    }
    // As the game does after Anvil's entry: through its own import.
    const bool keep_running = mode == "survive";
    g_handed_over = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetUnhandledExceptionFilter(keep_running ? child_keep_running_filter : child_next_filter);

    DWORD (WINAPI *body)(LPVOID) = nullptr;
    if (mode == "av" || mode == "survive") {
        body = [](LPVOID) -> DWORD {
            crash_here();
            return 0;
        };
    } else if (mode == "change" || mode == "mid") {
        body = [](LPVOID p) -> DWORD {
            return static_cast<DWORD>(call_scratch(reinterpret_cast<Fn>(p), 1));
        };
    } else if (mode == "overflow") {
        body = [](LPVOID) -> DWORD {
            // What version.dll's DllMain does as one of the game's threads
            // starts: a guarantee only where the host's exception context
            // does not fit in the default (AMX).
            crash::guard_thread(crash::thread_guarantee());
            return static_cast<DWORD>(recurse(0));
        };
    } else {
        return 12;
    }
    // A thread of its own with the default stack guarantee, like the game's
    // (but see crash::thread_guarantee).
    HANDLE thread = CreateThread(nullptr, 256 * 1024, body,
                                 reinterpret_cast<void*>(image.at(mode == "mid" ? 0x100 : 0x200)),
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!keep_running) {
        WaitForSingleObject(thread, 60000);
        return 13;  // not reached: the crash ends the process
    }
    // The game goes on after the hand-over: readiness, then a normal exit,
    // as runtime.cpp handles them.
    if (WaitForSingleObject(g_handed_over, 60000) != WAIT_OBJECT_0) return 14;
    if (kept_dumps(dir / "crashes") == 1) std::ofstream(dir / "dump-while-running.txt") << "yes";
    marker.game_ready(crash::reported());
    if (marker.active() && !crash::crashed()) marker.clean_exit();
    ExitProcess(0x5AFE);
}

// ---- the parent side -------------------------------------------------------------

struct ChildResult {
    std::string mode;
    int failures_before = 0;  // g_failures when the child had ended
    std::string output;       // the child's stdout and stderr
    std::string listing;      // the crash folder after the child
    DWORD exit = 0;
    std::string json, text;
    std::vector<fs::path> reports;  // *.json in the crash folder
    bool dump = false;
    bool next_filter = false;
    bool pending_left = false;
};

ChildResult run_child(const char* mode, const fs::path& dir) {
    ChildResult r;
    fs::remove_all(dir);
    fs::create_directories(dir / "crashes");
    std::ofstream(dir / "unfinished-starts.txt") << "1\n";  // one unfinished start before this one
    // Older reports: the oldest go, 10 stay.
    for (int i = 0; i < 12; ++i) {
        char stem[32];
        std::snprintf(stem, sizeof(stem), "2000-01-01_00-00-%02d", i);
        std::ofstream(dir / "crashes" / (std::string(stem) + ".json")) << "{}";
        std::ofstream(dir / "crashes" / (std::string(stem) + ".txt")) << "old";
    }
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child " + widen(mode) + L" \"" + dir.wstring() + L"\"";
    // The child's output goes to a file, printed when a check fails.
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const fs::path output = dir / "child-output.txt";
    HANDLE out = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof(si)};
    if (out != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = out;
        si.hStdError = out;
    }
    PROCESS_INFORMATION pi{};
    r.mode = mode;
    const BOOL started =
        CreateProcessW(nullptr, command.data(), nullptr, nullptr, out != INVALID_HANDLE_VALUE, 0, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!started) return r;
    if (WaitForSingleObject(pi.hProcess, 90000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);
    GetExitCodeProcess(pi.hProcess, &r.exit);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.failures_before = g_failures;
    r.output = read_file(output);
    for (const auto& entry : fs::directory_iterator(dir / "crashes")) {
        const auto name = entry.path().filename().wstring();
        if (name.rfind(L"2000-", 0) != 0) {
            r.listing += " " + entry.path().filename().string() + " (" + std::to_string(fs::file_size(entry.path())) + " B)";
        }
        if (entry.path().extension() == L".json") r.reports.push_back(entry.path());
        if (name.rfind(L"pending-", 0) == 0) r.pending_left = true;
    }
    r.dump = kept_dumps(dir / "crashes") == 1;
    for (const auto& report : r.reports) {
        if (report.filename().wstring().rfind(L"2000-", 0) == 0) continue;
        r.json = read_file(report);
        auto text = report;
        r.text = read_file(text.replace_extension(L".txt"));
    }
    r.next_filter = fs::exists(dir / "next-filter.txt");
    return r;
}

// After a child test's checks: when one failed, what the child did, so that a
// failure explains itself (#98).
void explain(const ChildResult& r) {
    if (g_failures == r.failures_before) return;
    std::printf("  child %s: exit 0x%08lX, next filter %s, crash folder:%s\n", r.mode.c_str(), r.exit,
                r.next_filter ? "called" : "not called", r.listing.empty() ? " (nothing new)" : r.listing.c_str());
    if (!r.output.empty()) std::printf("  child output:\n%s\n", r.output.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--child") return child(argv[2], argv[3]);

    run("crash: addresses in changed bytes, next stubs and trampolines name their changes", [] {
        Scratch image(0x10000);
        image.put(0x100, kAddOne);
        ChangeRegistry r(image.at(0), 0x10000);
        const OwnerId a = r.add_owner("a"), b = r.add_owner("b");
        ChangeTarget t = in_image(image.at(0x100));
        t.symbol = "add_one";
        ChangeId ia = 0, ib = 0;
        CHECK(r.hook_function(a, t, reinterpret_cast<void*>(&detour_a), slot(g_next_a), &ia) == ChangeStatus::Ok);
        CHECK(r.hook_function(b, t, reinterpret_cast<void*>(&detour_b), slot(g_next_b), &ib) == ChangeStatus::Ok);
        CHECK(reinterpret_cast<Fn>(image.at(0x100))(1) == 104);

        AddressAttribution at;
        CHECK(r.attribute(image.at(0x102), at));
        CHECK(at.role == AddressRole::ChangedBytes);
        CHECK(at.site == image.at(0x100));
        CHECK(at.count == 2 && at.total == 2);
        CHECK(at.changes[0].owner == a && at.changes[0].id == ia && at.changes[1].owner == b);
        CHECK(at.changes[0].kind == ChangeKind::FunctionHook);
        CHECK(std::string(at.changes[0].symbol) == "add_one");
        CHECK(contains(at.changes[0].label, "function hook"));

        // A link's own next stub: that link only.
        CHECK(r.attribute(reinterpret_cast<uintptr_t>(g_next_a) + 3, at));
        CHECK(at.role == AddressRole::Stub && at.count == 1 && at.changes[0].owner == a);
        CHECK(!at.context_return);

        // The innermost next leads to the trampoline: the copy of 0x100.
        const uintptr_t trampoline = stub_target(reinterpret_cast<void*>(g_next_b));
        CHECK(r.attribute(trampoline, at));
        CHECK(at.role == AddressRole::Trampoline && at.original == image.at(0x100) && at.count == 2);
        CHECK(r.attribute(trampoline + 3, at));
        CHECK(at.original == image.at(0x103));

        // Outside everything.
        CHECK(r.attribute(image.at(0x400), at));
        CHECK(at.role == AddressRole::None && at.count == 0);
        CHECK(r.attribute(reinterpret_cast<uintptr_t>(&detour_a), at));
        CHECK(at.role == AddressRole::None);

        // A removed link's stub stays its own; its bytes are no longer changed.
        void* next_a = reinterpret_cast<void*>(g_next_a);
        CHECK(r.remove(b, ib) == ChangeStatus::Ok);
        CHECK(r.remove(a, ia) == ChangeStatus::Ok);
        CHECK(r.attribute(reinterpret_cast<uintptr_t>(next_a), at));
        CHECK(at.role == AddressRole::Stub && at.count == 1 && at.changes[0].owner == a);
        CHECK(r.attribute(image.at(0x102), at));
        CHECK(at.role == AddressRole::None);
    });

    run("crash: byte patches shared by two owners list both", [] {
        Scratch image(0x10000);
        image.put(0x100, kAddOne);
        ChangeRegistry r(image.at(0), 0x10000);
        const OwnerId a = r.add_owner("a"), b = r.add_owner("b");
        const uint8_t nop[] = {0x90};
        CHECK(r.patch_bytes(a, in_image(image.at(0x101)), nop, 1) == ChangeStatus::Ok);
        CHECK(r.patch_bytes(b, in_image(image.at(0x101)), nop, 1) == ChangeStatus::Ok);
        AddressAttribution at;
        CHECK(r.attribute(image.at(0x101), at));
        CHECK(at.role == AddressRole::ChangedBytes && at.count == 2);
        CHECK(at.changes[0].kind == ChangeKind::BytePatch);
        CHECK(r.attribute(image.at(0x102), at));
        CHECK(at.role == AddressRole::None);
    });

    run("crash: the stack walk passes a mid hook's context stub to the hooked site and its caller", [] {
        Scratch image(0x10000);
        image.put(0x100, kAddOne);
        image.put(0x200, kAddOne);
        TestStart start(image.at(0), 0x10000);
        add_plugin(start, "m").symbols = symbols(image.at(0), "add_one", 0x100, kAddOne.size());
        start.make();
        ChangeRegistry& r = start.changes;
        const OwnerId m = start.handle("m")->owner;
        CrashSources sources;
        sources.changes = &r;
        sources.plugins = &start.set();
        sources.image_base = image.at(0);
        sources.image_size = 0x10000;
        g_walk_sources = &sources;
        // Another site first: both enter one shared context stub, and the
        // walk must still find the site it came from.
        CHECK(r.mid_hook(m, in_image(image.at(0x200)), quiet, nullptr) == ChangeStatus::Ok);
        CHECK(r.mid_hook(m, in_image(image.at(0x100)), walk_here, nullptr) == ChangeStatus::Ok);
        CHECK(reinterpret_cast<Fn>(image.at(0x200))(1) == 2);
        g_walked_count = 0;
        CHECK(call_scratch(reinterpret_cast<Fn>(image.at(0x100)), 1) == 3);
        size_t stub = g_walked_count;
        for (size_t i = 0; i < g_walked_count; ++i) {
            if (g_walked[i].where.change.context_return) {
                stub = i;
                break;
            }
        }
        CHECK(stub + 2 < g_walked_count);
        if (stub + 2 < g_walked_count) {
            const Frame& stub_frame = g_walked[stub];
            CHECK(stub_frame.where.change.role == AddressRole::Stub);
            CHECK(stub_frame.where.change.site == image.at(0x100));
            CHECK(stub_frame.where.change.count == 1 && stub_frame.where.change.changes[0].owner == m);
            CHECK(contains(stub_frame.where.change.changes[0].label, "mid hook"));
            // The shared stub alone names no site; an unknown state is none.
            AddressAttribution shared;
            CHECK(r.attribute(stub_frame.where.address, shared));
            CHECK(shared.role == AddressRole::Stub && shared.context_return && shared.count == 0);
            CHECK(r.attribute(stub_frame.where.address - 1, shared));
            CHECK(shared.role == AddressRole::Stub && !shared.context_return);
            CHECK(!r.attribute_context(image.at(0x100), shared));
            const Frame& site = g_walked[stub + 1];
            CHECK(site.source == FrameSource::ContextStub);
            CHECK(site.where.address == image.at(0x100));
            CHECK(site.where.symbol != nullptr && site.where.symbol_offset == 0 && site.where.inside_symbol);
            CHECK(site.where.change.role == AddressRole::ChangedBytes);
            // The game's caller: call_scratch, found through the site's
            // return address (the scratch "game" has no unwind data).
            const Frame& caller = g_walked[stub + 2];
            CHECK(caller.source == FrameSource::Unwind);
            CHECK(caller.where.address > reinterpret_cast<uintptr_t>(&call_scratch) &&
                  caller.where.address < reinterpret_cast<uintptr_t>(&call_scratch) + 64);
        }
        const CrashVerdict v = assess(g_walked, g_walked_count, sources);
        CHECK(v.level == CrashLevel::OnStack && v.count == 1 && v.handles[0] == start.handle("m"));
    });

    run("crash: the nearest declared symbol of any plugin", [] {
        Scratch image(0x10000);
        TestStart start(image.at(0), 0x10000);
        add_plugin(start, "a").symbols = symbols(image.at(0), "low", 0x100, 8);
        add_plugin(start, "b").symbols = symbols(image.at(0), "high", 0x200, 8);
        add_plugin(start, "c").symbols = symbols(image.at(0), "high_too", 0x200, 4);
        CrashSources sources;
        sources.plugins = &start.make();
        sources.image_base = image.at(0);
        sources.image_size = 0x10000;
        Located at;
        locate(image.at(0x104), sources, at);
        CHECK(at.module.base == image.at(0) && std::string(at.module.name) == "eu4.exe");
        CHECK(at.symbol != nullptr && at.symbol->declaration.name == "low" && at.symbol_plugin == &start.plugin("a"));
        CHECK(at.symbol_offset == 4 && at.inside_symbol && at.also_declared == 0);
        locate(image.at(0x180), sources, at);
        CHECK(at.symbol != nullptr && at.symbol->declaration.name == "low" && !at.inside_symbol);
        locate(image.at(0x202), sources, at);
        CHECK(at.symbol != nullptr && at.symbol->declaration.name == "high" && at.also_declared == 1);
        locate(image.at(0x80), sources, at);
        CHECK(at.symbol == nullptr);
        locate(reinterpret_cast<uintptr_t>(&detour_a), sources, at);
        CHECK(at.symbol == nullptr && at.module.base == reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
        CHECK(at.plugin == nullptr);
    });

    run("crash: the four levels", [] {
        TestStart start;
        for (const char* id : {"p0", "p1", "p2", "p3"}) add_plugin(start, id);
        CrashSources sources;
        sources.plugins = &start.make();
        const auto owner = [&](const char* id) { return start.handle(id)->owner; };
        Frame frames[3];
        CHECK(assess(frames, 3, sources).level == CrashLevel::NoPlugin);
        frames[2].where.plugin = &start.plugin("p2");
        CrashVerdict v = assess(frames, 3, sources);
        CHECK(v.level == CrashLevel::OnStack && v.count == 1 && v.handles[0] == start.handle("p2"));
        frames[1].where.change.role = AddressRole::Stub;
        frames[1].where.change.count = 1;
        frames[1].where.change.changes[0].owner = owner("p3");
        v = assess(frames, 3, sources);
        CHECK(v.level == CrashLevel::OnStack && v.count == 2 && v.handles[0] == start.handle("p3") &&
              v.handles[1] == start.handle("p2"));
        frames[0].where.change.role = AddressRole::ChangedBytes;
        frames[0].where.change.count = 2;
        frames[0].where.change.changes[0].owner = owner("p1");
        frames[0].where.change.changes[1].owner = owner("p0");
        v = assess(frames, 3, sources);
        CHECK(v.level == CrashLevel::InChange && v.count == 2 && v.handles[0] == start.handle("p1") &&
              v.handles[1] == start.handle("p0"));
        // The core's own owner is no plugin's.
        frames[0].where.change.changes[1].owner = start.set().core_owner();
        v = assess(frames, 3, sources);
        CHECK(v.level == CrashLevel::InChange && v.count == 1);
        frames[0].where.change.changes[1].owner = owner("p0");
        frames[0].where.plugin = &start.plugin("p2");
        v = assess(frames, 3, sources);
        CHECK(v.level == CrashLevel::InCode && v.count == 1 && v.handles[0] == start.handle("p2"));
        CHECK(std::string(crash_level_key(CrashLevel::InCode)) == "in_plugin_code");
        CHECK(std::string(crash_level_key(CrashLevel::NoPlugin)) == "no_plugin");

        char text[128];
        TextBuffer out(text, sizeof(text));
        frames[0].where.plugin = nullptr;
        describe_verdict(assess(frames, 3, sources), sources, out);
        CHECK(out.view() == "in plugin p1's change (also p0)");
        out.clear();
        describe_verdict(CrashVerdict{}, sources, out);
        CHECK(out.view() == "no plugin involved");
    });

    run("crash: the report in JSON and text, with issues, plugins, registers and the log", [] {
        Scratch image(0x10000);
        image.put(0x100, kAddOne);
        TestStart start(image.at(0), 0x10000);
        add_plugin(start, "quiet");
        add_plugin(start, "patcher", "issues = \"https://example.org/p\\\"q/issues\"\n").symbols =
            symbols(image.at(0), "add_one", 0x100, kAddOne.size());
        start.make();
        CHECK(start.load("quiet") != nullptr && start.load("patcher") != nullptr);
        ChangeRegistry& r = start.changes;
        const OwnerId p = start.handle("patcher")->owner;
        const uint8_t ud2[] = {0x0F, 0x0B};
        CHECK(r.patch_bytes(p, in_image(image.at(0x103)), ud2, 2) == ChangeStatus::Ok);
        CrashSources sources;
        sources.changes = &r;
        sources.plugins = &start.set();
        sources.image_base = image.at(0);
        sources.image_size = 0x10000;
        sources.build_id = "eu4-test";
        EXCEPTION_RECORD record{};
        record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        record.ExceptionAddress = reinterpret_cast<void*>(image.at(0x103));
        CONTEXT ctx{};
        RtlCaptureContext(&ctx);
        ctx.Rip = image.at(0x103);
        SeenException seen{EXCEPTION_ACCESS_VIOLATION, 7, image.at(0x10), {0, 0}, 2, 0};
        ReportInput in;
        in.record = &record;
        in.context = &ctx;
        in.thread = 42;
        in.sources = &sources;
        in.anvil_version = "1.2.3";
        in.time.wYear = 2026, in.time.wMonth = 10, in.time.wDay = 19;
        in.seen = &seen;
        in.seen_count = 1;
        in.log_tail = "[ 1.0 ms] info  [anvil] \"quoted\" line\r\n[ 2.0 ms] WARN  [x] tab\there\r\n";
        static char json_data[256 * 1024], text_data[192 * 1024];
        TextBuffer json(json_data, sizeof(json_data)), text(text_data, sizeof(text_data));
        const CrashVerdict v = build_report(in, json, text);
        CHECK(v.level == CrashLevel::InChange && v.count == 1 && v.handles[0] == start.handle("patcher"));
        CHECK(!json.truncated() && !text.truncated());
        CHECK(Json::valid(json.view()));
        const std::string_view j = json.view(), t = text.view();
        CHECK(contains(j, "\"level\": \"in_plugin_change\""));
        CHECK(contains(j, "\"summary\": \"in plugin patcher's change\""));
        CHECK(contains(j, "\"code\": \"0xC000001D\""));
        CHECK(contains(j, "\"game_build\": \"eu4-test\""));
        CHECK(contains(j, "{\"plugin\": \"patcher\", \"url\": \"https://example.org/p\\\"q/issues\"}"));
        CHECK(contains(j, "\"role\": \"changed bytes\""));
        CHECK(contains(j, "\"kind\": \"byte patch\""));
        CHECK(contains(j, "\"symbol\": {\"name\": \"add_one\", \"offset\": \"0x3\", \"declared_by\": \"patcher\""));
        CHECK(contains(j, "\"module\": \"eu4.exe\", \"offset\": \"0x103\""));
        CHECK(contains(j, "\"rip\": \"0x"));
        CHECK(contains(j, "{\"id\": \"quiet\", \"version\": \"1.0.0\", \"state\": \"loaded\""));
        CHECK(contains(j, "\"earlier_exceptions\": [{\"code\": \"0xC0000005\", \"thread\": 7"));
        CHECK(contains(j, "\"[ 1.0 ms] info  [anvil] \\\"quoted\\\" line\""));
        CHECK(contains(j, "tab\\there"));
        CHECK(contains(t, "Verdict: in plugin patcher's change."));
        CHECK(contains(t, "Exception: 0xC000001D illegal instruction"));
        CHECK(contains(t, "Address: eu4.exe+0x103"));
        CHECK(contains(t, "Nearest declared symbol: add_one+0x3 (declared by patcher, inside its declared bytes)"));
        CHECK(contains(t, "Report it to patcher: https://example.org/p\"q/issues"));
        CHECK(contains(t, "changed bytes of patcher's byte patch rva 0x103 (2 B)"));
        CHECK(contains(t, "  quiet 1.0.0, loaded, main phase"));
        CHECK(contains(j, "\"modules_involved\": []") && !contains(t, "Through module"));
        CHECK(contains(t, "End of anvil.log:"));

        // An access violation in code of no plugin, a plugin on the stack.
        record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        record.NumberParameters = 2;
        record.ExceptionInformation[0] = 1;
        record.ExceptionInformation[1] = 0x10;
        record.ExceptionAddress = reinterpret_cast<void*>(image.at(0x400));
        ctx.Rip = image.at(0x400);
        json.clear();
        text.clear();
        in.context = nullptr;  // only the address
        const CrashVerdict none = build_report(in, json, text);
        CHECK(none.level == CrashLevel::NoPlugin);
        CHECK(Json::valid(json.view()));
        CHECK(contains(json.view(), "\"access\": \"write\", \"access_address\": \"0x10\""));
        CHECK(contains(json.view(), "\"level\": \"no_plugin\""));
        CHECK(contains(json.view(), "\"issues\": []"));
        CHECK(contains(text.view(), "Verdict: no plugin involved."));
    });

    // ADR 0011: every module that is on has its own owner, right after its
    // plugin's, so owners are no longer positions in the list of plugins.
    run("crash: a crash in a module's change names the module and its plugin, and a later plugin keeps its name", [] {
        Scratch image(0x10000);
        TestStart start(image.at(0), 0x10000);
        add_plugin(start, "smithy", "issues = \"https://example.org/smithy\"\n"
                                    "\n[[modules]]\nid = \"ui_fix\"\nname = \"UI fix\"\naffects_gameplay = false\n"
                                    "\n[[modules]]\nid = \"chase\"\nname = \"Chase\"\naffects_gameplay = false\ndefault = false\n"
                                    "\n[[modules]]\nid = \"drill\"\nname = \"Drill\"\naffects_gameplay = false\n");
        add_plugin(start, "tongs", "issues = \"https://example.org/tongs\"\n");
        start.make();
        CHECK(start.load("smithy") != nullptr && start.load("tongs") != nullptr);
        AnvilPlugin* ui_fix = start.module("smithy", "smithy.ui_fix");
        AnvilPlugin* drill = start.module("smithy", "smithy.drill");
        AnvilPlugin* tongs = start.handle("tongs");
        CHECK(ui_fix != nullptr && drill != nullptr && start.module("smithy", "smithy.chase") == nullptr);
        if (ui_fix == nullptr || drill == nullptr) return;
        // A byte patch of each, as the host makes it for the handle that asks.
        const uint8_t ud2[] = {0x0F, 0x0B};
        CHECK(start.changes.patch_bytes(ui_fix->owner, in_image(image.at(0x100)), ud2, 2) == ChangeStatus::Ok);
        CHECK(start.changes.patch_bytes(drill->owner, in_image(image.at(0x200)), ud2, 2) == ChangeStatus::Ok);
        CHECK(start.changes.patch_bytes(tongs->owner, in_image(image.at(0x300)), ud2, 2) == ChangeStatus::Ok);
        CrashSources sources;
        sources.changes = &start.changes;
        sources.plugins = &start.set();
        sources.image_base = image.at(0);
        sources.image_size = 0x10000;
        static char json_data[64 * 1024], text_data[64 * 1024];
        TextBuffer json(json_data, sizeof(json_data)), text(text_data, sizeof(text_data));
        const auto report = [&](size_t offset) {
            EXCEPTION_RECORD record{};
            record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
            record.ExceptionAddress = reinterpret_cast<void*>(image.at(offset));
            ReportInput in;
            in.record = &record;
            in.sources = &sources;
            json.clear();
            text.clear();
            return build_report(in, json, text);
        };

        CrashVerdict v = report(0x100);
        CHECK(v.level == CrashLevel::InChange && v.count == 1 && v.handles[0] == ui_fix);
        CHECK(Json::valid(json.view()));
        // The summary and plugins_involved name the plugin, as before modules:
        // safe mode and the overlay read them; the module is named beside them.
        CHECK(contains(json.view(), "\"summary\": \"in plugin smithy's change\""));
        CHECK(contains(json.view(), "\"plugins_involved\": [\"smithy\"],\n  \"modules_involved\": [\"smithy.ui_fix\"]"));
        CHECK(contains(json.view(), "\"plugin\": \"smithy\", \"module\": \"smithy.ui_fix\", \"kind\": \"byte patch\""));
        CHECK(contains(json.view(), "{\"plugin\": \"smithy\", \"url\": \"https://example.org/smithy\"}"));
        CHECK(contains(text.view(), "Verdict: in plugin smithy's change."));
        CHECK(contains(text.view(), "Through module smithy.ui_fix of plugin smithy."));
        CHECK(contains(text.view(), "changed bytes of smithy.ui_fix's byte patch"));
        CHECK(contains(text.view(), "Report it to smithy: https://example.org/smithy"));
        CHECK(!contains(text.view(), "tongs's") && !contains(json.view(), "\"?\""));
        const CrashSummary summary = parse_crash_summary(json.view());
        CHECK(summary.plugins == std::vector<std::string>{"smithy"} && summary.issues == "https://example.org/smithy");
        CHECK(crash_in_plugin(summary, "smithy") && !crash_in_plugin(summary, "tongs"));
        CHECK(i18n::crash_message(summary.level, summary.plugins, summary.summary).code == "crash_in_change");

        v = report(0x200);
        CHECK(v.level == CrashLevel::InChange && v.count == 1 && v.handles[0] == drill);
        CHECK(contains(json.view(), "\"modules_involved\": [\"smithy.drill\"]"));
        CHECK(contains(text.view(), "Through module smithy.drill of plugin smithy."));

        // The plugin after the one with modules.
        v = report(0x300);
        CHECK(v.level == CrashLevel::InChange && v.count == 1 && v.handles[0] == tongs);
        CHECK(contains(json.view(), "\"summary\": \"in plugin tongs's change\""));
        CHECK(contains(json.view(), "\"plugins_involved\": [\"tongs\"],\n  \"modules_involved\": []"));
        CHECK(contains(json.view(), "\"plugin\": \"tongs\", \"kind\": \"byte patch\""));
        CHECK(contains(text.view(), "Report it to tongs: https://example.org/tongs") && !contains(text.view(), "Through module"));
        CHECK(crash_in_plugin(parse_crash_summary(json.view()), "tongs"));
        // The list of plugins: the two of this start, not their modules.
        CHECK(contains(text.view(), "Plugins (2):\r\n  smithy 1.0.0, loaded, main phase"));
    });

    run("crash: text buffers cut what does not fit", [] {
        char small[8];
        TextBuffer b(small, sizeof(small));
        b.put("12345");
        b.putf("%d", 678);
        CHECK(b.view() == "12345678" && !b.truncated());
        b.put("9");
        CHECK(b.view() == "12345678" && b.truncated());
        char json[32];
        TextBuffer j(json, sizeof(json));
        j.json("a\"\\\x01");
        CHECK(j.view() == "\"a\\\"\\\\\\u0001\"");
    });

    run("safe mode: two starts that ended before game_ready, then one safe start", [] {
        const fs::path dir = fs::temp_directory_path() / "anvil-safe-mode-test";
        fs::remove_all(dir);
        const fs::path file = dir / "data" / "anvil" / "unfinished-starts.txt";
        {
            StartMarker m;
            const StartRecord s = m.begin(file);
            CHECK(s.unfinished == 0 && !s.safe_mode && m.active());
            CHECK(read_file(file) == "1\n");
        }  // crashed: nothing else happens
        {
            StartMarker m;
            const StartRecord s = m.begin(file);
            CHECK(s.unfinished == 1 && !s.safe_mode);
            CHECK(read_file(file) == "2\n");
        }
        {
            StartMarker m;
            const StartRecord s = m.begin(file);
            CHECK(s.unfinished == 2 && s.safe_mode && !m.active());
            CHECK(!fs::exists(file));  // the next start loads the plugins again
            m.game_ready(false);
            m.clean_exit();
            CHECK(!fs::exists(file));
        }
        {
            StartMarker m;
            CHECK(!m.begin(file).safe_mode);
            m.game_ready(false);  // a good start forgets the earlier ones
            CHECK(!fs::exists(file) && !m.active());
            m.clean_exit();
            CHECK(!fs::exists(file));
        }
        {
            StartMarker m;
            m.begin(file);
            StartMarker n;
            CHECK(n.begin(file).unfinished == 1);
            n.clean_exit();  // closed before game_ready without a crash: back to 1
            CHECK(read_file(file) == "1\n");
            StartMarker o;
            CHECK(o.begin(file).unfinished == 1);
            o.game_ready(true);  // ready after a crash report: still counted
            CHECK(read_file(file) == "2\n" && !o.active());
            o.clean_exit();
            CHECK(read_file(file) == "2\n");
        }
        std::ofstream(file) << "garbage";
        StartMarker g;
        CHECK(g.begin(file).unfinished == 0);
        CHECK(!loads_in_safe_mode("tongs") && loads_in_safe_mode("anvil-game") && loads_in_safe_mode("anvil-overlay"));
        fs::remove_all(dir);
    });

    const fs::path root = fs::temp_directory_path() / "anvil-crash-tests";

    run("crash capture: an access violation in a plugin's code (child process)", [&] {
        const ChildResult r = run_child("av", root / "av");
        CHECK(r.exit == 0xC0DE);  // Anvil's filter called the game's
        CHECK(r.next_filter);
        CHECK(r.reports.size() == 10);  // pruned to the last 10, the new one included
        CHECK(!r.json.empty() && Json::valid(r.json));
        CHECK(contains(r.json, "\"code\": \"0xC0000005\""));
        CHECK(contains(r.json, "\"level\": \"in_plugin_code\""));
        CHECK(contains(r.json, "\"summary\": \"in plugin crasher's code\""));
        CHECK(contains(r.json, "\"url\": \"https://example.org/crasher/issues\""));
        CHECK(contains(r.json, "\"access\": \"write\""));
        CHECK(contains(r.json, "\"anvil\": \"9.9.9\""));
        CHECK(contains(r.json, "\"unfinished_starts\": 1"));
        CHECK(contains(r.json, "\"main_thread\": false"));
        CHECK(contains(r.json, "line two"));
        CHECK(contains(r.json, "\"minidump\": \"20"));  // <time>.dmp
        CHECK(contains(r.text, "Verdict: in plugin crasher's code."));
        CHECK(contains(r.text, "Minidump: 20"));
        CHECK(r.dump && !r.pending_left);
        explain(r);
    });

    // #82: EU4's filter kept the crashed thread and the game reached the menu;
    // readiness reset the safe-mode count, and an access violation got no
    // minidump (only a stack overflow had one).
    run("crash capture: the game goes on to the menu after a crash on another thread (child process)", [&] {
        const fs::path dir = root / "survive";
        const ChildResult r = run_child("survive", dir);
        CHECK(r.exit == 0x5AFE);
        CHECK(contains(r.json, "\"unfinished_starts\": 1"));
        CHECK(contains(r.json, "\"before_game_ready\": true"));
        CHECK(read_file(dir / "unfinished-starts.txt") == "2\n");  // still counted: the next start is safe
        CHECK(fs::exists(dir / "dump-while-running.txt"));       // the dump was kept before readiness
        CHECK(contains(r.json, "\"minidump\": \"20"));
        CHECK(r.dump && !r.pending_left);
        explain(r);
    });

    run("crash capture: an illegal instruction in a plugin's byte patch (child process)", [&] {
        const ChildResult r = run_child("change", root / "change");
        CHECK(r.exit == 0xC0DE && r.next_filter);
        CHECK(Json::valid(r.json));
        CHECK(contains(r.json, "\"code\": \"0xC000001D\""));
        CHECK(contains(r.json, "\"level\": \"in_plugin_change\""));
        CHECK(contains(r.json, "\"module\": \"game.exe\", \"offset\": \"0x200\""));
        CHECK(contains(r.json, "\"name\": \"add_one\""));
        CHECK(contains(r.text, "Verdict: in plugin patcher's change."));
        // The caller is in the test executable, "crasher"'s module.
        CHECK(contains(r.text, "[plugin crasher]"));
        explain(r);
    });

    run("crash capture: a crash in a mid hook's callback is reported through its context stub (child process)", [&] {
        const ChildResult r = run_child("mid", root / "mid");
        CHECK(r.exit == 0xC0DE && r.next_filter);
        CHECK(Json::valid(r.json));
        CHECK(contains(r.json, "\"code\": \"0xC0000005\""));
        CHECK(contains(r.json, "\"level\": \"in_plugin_code\""));
        // The context stub is shared: its spare slot names the site and change
        // of the return into it, the mid hook at 0x100, not the one at 0x300.
        CHECK(contains(r.json, "\"source\": \"context stub\""));
        CHECK(contains(r.json, "\"kind\": \"mid hook\", \"label\": \"mid hook rva 0x100\""));
        CHECK(!contains(r.json, "mid hook rva 0x300"));
        CHECK(contains(r.json, "\"module\": \"game.exe\", \"offset\": \"0x100\""));
        explain(r);
    });

    run("crash: threads get a stack guarantee only where the exception context does not fit (AMX)", [] {
        CHECK(crash::thread_guarantee_for(1663) == 0);  // AVX
        CHECK(crash::thread_guarantee_for(3263) == 0);  // AVX-512
        CHECK(crash::thread_guarantee_for(8 * 1024) == 0);
        CHECK(crash::thread_guarantee_for(11567) == 20 * 1024);  // AMX (Xeon 8573C, 6973P-C)
        CHECK(crash::thread_guarantee_for(8 * 1024 + 1) == 20 * 1024);
        CHECK(crash::thread_guarantee_for(13 * 1024) == 24 * 1024);
        CHECK(crash::thread_guarantee() == crash::thread_guarantee_for(exception_context_bytes()));
        // A thread of 256 KB or more gets it; a smaller one keeps the default.
        CHECK(on_thread(256 * 1024, [](LPVOID) -> DWORD {
                  return crash::guard_thread(20 * 1024) ? current_guarantee() : 0;
              }) >= 20 * 1024);
        if (under_wine()) {
            wine_skip("a thread of 128 KB keeps the default guarantee (Wine gives every thread at least 1 MB)");
        } else {
            CHECK(on_thread(128 * 1024, [](LPVOID) -> DWORD {
                      return crash::guard_thread(20 * 1024) ? 1 : current_guarantee() < 20 * 1024 ? 2 : 3;
                  }) == 2);
        }
        CHECK(on_thread(1024 * 1024, [](LPVOID) -> DWORD {
                  return crash::guard_thread(0) ? 1 : current_guarantee() < 20 * 1024 ? 2 : 3;
              }) == 2);
    });

    run("crash: version.dll in the game gives new threads the guarantee where the host needs it (child process)", [&] {
        if (argc < 2) {
            std::printf("  (skipped: pass the built version.dll path)\n");
            return;
        }
        SetEnvironmentVariableW(L"ANVIL_TEST_VERSION_DLL", widen(argv[1]).c_str());
        const ChildResult r = run_child("dllmain", root / "dllmain");
        CHECK(r.exit == 0);
        explain(r);
    });

    run("crash capture: a stack overflow writes the report and a minidump (child process)", [&] {
        if (const ULONG guarantee = crash::thread_guarantee(); guarantee != 0) {
            std::printf("  note: the exception context is %lu bytes here; the overflowing thread gets a %lu KB guarantee\n",
                        exception_context_bytes(), guarantee / 1024);
        }
        const ChildResult r = run_child("overflow", root / "overflow");
        // Wine ends the process with its own status once the filter returns; the report and dump are still checked.
        CHECK(under_wine() ? r.exit != 0 : (r.exit == 0xC0DE || r.exit == EXCEPTION_STACK_OVERFLOW));
        CHECK(Json::valid(r.json));
        CHECK(contains(r.json, "\"code\": \"0xC00000FD\""));
        CHECK(contains(r.json, "\"level\": \"in_plugin_code\""));
        CHECK(contains(r.json, ".dmp\""));
        // The recursion is one frame with its repeats; the walk reaches the thread's start.
        CHECK(contains(r.json, "\"repeats\": "));
        CHECK(contains(r.text, "KERNEL32.dll+"));
        CHECK(r.dump && !r.pending_left);
        explain(r);
    });
    fs::remove_all(root);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
