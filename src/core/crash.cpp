#include "crash.hpp"

#include "changes.hpp"
#include "log.hpp"
#include "seh_guard.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace anvil::crash {
namespace {

constexpr DWORD kWaitMs = 30000;  // how long a crashed thread waits for its report
constexpr size_t kKeep = 10;      // reports kept in the crash folder
constexpr size_t kSeenMax = 8;

// ---- what the vectored handler records -------------------------------------
SeenException g_seen[kSeenMax];
volatile LONG g_seen_count = 0;
volatile LONG64 g_last_fatal = 0;  // GetTickCount64 of the last fatal-looking exception

// ---- set up at install, read by the helper ---------------------------------
const Setup* g_setup = nullptr;  // lives for the process
std::atomic<const PluginSet*> g_plugins{nullptr};  // attach_plugins

// What reports attribute against: the setup's, with the plugins attached so far.
CrashSources sources_now() {
    CrashSources s = g_setup->sources;
    if (const PluginSet* plugins = g_plugins.load(std::memory_order_acquire)) s.plugins = plugins;
    return s;
}
wchar_t g_dir[MAX_PATH * 2] = {};  // the crash folder, with a trailing backslash
wchar_t g_log[MAX_PATH * 2] = {};
DWORD g_main_thread = 0;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_game_ready{false};

// ---- the filter chain --------------------------------------------------------
using Filter = LPTOP_LEVEL_EXCEPTION_FILTER;
using SetFilterFn = Filter(WINAPI*)(Filter);
SetFilterFn g_real_set = nullptr;  // kernel32's SetUnhandledExceptionFilter
std::atomic<Filter> g_next{nullptr};  // the filter the game set, called after Anvil's

// ---- the helper thread -------------------------------------------------------
HANDLE g_wake = nullptr;  // auto-reset: a request is ready
HANDLE g_done = nullptr;  // manual reset: the report is written (or given up)
DWORD g_helper = 0;
volatile LONG g_claimed = 0;  // the first crash claims the helper; later ones wait
struct Request {
    EXCEPTION_POINTERS* pointers = nullptr;
    DWORD thread = 0;
    uintptr_t stack_low = 0, stack_high = 0;
};
Request g_request;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_write_dump = nullptr;
HANDLE g_dump = INVALID_HANDLE_VALUE;  // pending-<pid>.dmp, deleted on close unless kept

// The report's buffers and files (one report per process).
char g_json[256 * 1024];
char g_text[192 * 1024];
char g_tail[12 * 1024];
HANDLE g_json_file = INVALID_HANDLE_VALUE;
HANDLE g_text_file = INVALID_HANDLE_VALUE;
wchar_t g_stem[64] = {};
TextBuffer g_json_text(g_json, sizeof(g_json));
TextBuffer g_text_text(g_text, sizeof(g_text));

// ---- files -------------------------------------------------------------------

void path_of(wchar_t (&out)[MAX_PATH * 2 + 80], const wchar_t* stem, const wchar_t* extension) {
    std::swprintf(out, std::size(out), L"%ls%ls%ls", g_dir, stem, extension);
}

bool write_all(HANDLE file, std::string_view text) {
    while (!text.empty()) {
        DWORD written = 0;
        const DWORD chunk = text.size() > 0x10000000 ? 0x10000000 : static_cast<DWORD>(text.size());
        if (!WriteFile(file, text.data(), chunk, &written, nullptr) || written == 0) return false;
        text.remove_prefix(written);
    }
    return true;
}

// Keeps the newest `keep` reports: their names sort by time.
void prune(size_t keep) {
    static wchar_t stems[128][64];
    size_t count = 0;
    wchar_t pattern[MAX_PATH * 2 + 80];
    path_of(pattern, L"*", L".json");
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const size_t length = std::wcslen(found.cFileName);
        if (length <= 5 || length - 5 >= std::size(stems[0]) || count >= std::size(stems)) continue;
        std::wmemcpy(stems[count], found.cFileName, length - 5);
        stems[count][length - 5] = L'\0';
        ++count;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    // Newest first.
    for (size_t i = 1; i < count; ++i) {
        for (size_t k = i; k > 0 && std::wcscmp(stems[k - 1], stems[k]) < 0; --k) {
            wchar_t swap[64];
            std::wmemcpy(swap, stems[k], 64);
            std::wmemcpy(stems[k], stems[k - 1], 64);
            std::wmemcpy(stems[k - 1], swap, 64);
        }
    }
    for (size_t i = keep; i < count; ++i) {
        wchar_t file[MAX_PATH * 2 + 80];
        for (const wchar_t* extension : {L".json", L".txt", L".dmp"}) {
            path_of(file, stems[i], extension);
            DeleteFileW(file);
        }
    }
}

// The last lines of anvil.log, read from the file: no lock of the log.
std::string_view read_log_tail() {
    if (g_log[0] == L'\0') return {};
    const HANDLE file = CreateFileW(g_log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    DWORD read = 0;
    if (GetFileSizeEx(file, &size)) {
        LARGE_INTEGER from{};
        from.QuadPart = size.QuadPart > static_cast<LONGLONG>(sizeof(g_tail)) ? size.QuadPart - sizeof(g_tail) : 0;
        if (SetFilePointerEx(file, from, nullptr, FILE_BEGIN) && !ReadFile(file, g_tail, sizeof(g_tail), &read, nullptr)) {
            read = 0;
        }
        if (from.QuadPart == 0 && read >= 3 && std::memcmp(g_tail, "\xEF\xBB\xBF", 3) == 0) {
            std::memmove(g_tail, g_tail + 3, read - 3);
            read -= 3;
        }
    }
    CloseHandle(file);
    std::string_view tail(g_tail, read);
    if (read == sizeof(g_tail)) {  // the first line is cut
        const size_t line = tail.find('\n');
        tail = line == std::string_view::npos ? std::string_view() : tail.substr(line + 1);
    }
    // At most the last 40 lines.
    size_t lines = 0;
    for (size_t i = tail.size(); i > 0; --i) {
        if (tail[i - 1] == '\n' && i != tail.size() && ++lines == 40) return tail.substr(i);
    }
    return tail;
}

// The pending dump file becomes <stem>.dmp: kept on close, then renamed.
bool keep_dump(const wchar_t* stem, wchar_t (&name)[80]) {
    FILE_DISPOSITION_INFO keep{FALSE};
    if (!SetFileInformationByHandle(g_dump, FileDispositionInfo, &keep, sizeof(keep))) return false;
    std::swprintf(name, std::size(name), L"%ls.dmp", stem);
    alignas(FILE_RENAME_INFO) uint8_t buffer[sizeof(FILE_RENAME_INFO) + sizeof(wchar_t) * (MAX_PATH * 2 + 80)] = {};
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer);
    wchar_t target[MAX_PATH * 2 + 80];
    path_of(target, stem, L".dmp");
    const size_t length = std::wcslen(target);
    rename->ReplaceIfExists = TRUE;
    rename->FileNameLength = static_cast<DWORD>(length * sizeof(wchar_t));
    std::wmemcpy(rename->FileName, target, length);
    return SetFileInformationByHandle(g_dump, FileRenameInfo, rename,
                                      static_cast<DWORD>(sizeof(FILE_RENAME_INFO) + length * sizeof(wchar_t))) != 0;
}

// ---- the report ----------------------------------------------------------------

void close_reports() {
    for (HANDLE* file : {&g_json_file, &g_text_file}) {
        if (*file != INVALID_HANDLE_VALUE) CloseHandle(*file);
        *file = INVALID_HANDLE_VALUE;
    }
}

// Opens <time>.json and .txt under a name no report has yet.
bool open_reports() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    for (int attempt = 1; attempt < 10; ++attempt) {
        std::swprintf(g_stem, std::size(g_stem), attempt == 1 ? L"%04u-%02u-%02u_%02u-%02u-%02u" : L"%04u-%02u-%02u_%02u-%02u-%02u-%d",
                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, attempt);
        wchar_t path[MAX_PATH * 2 + 80];
        path_of(path, g_stem, L".json");
        g_json_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_json_file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    if (g_json_file == INVALID_HANDLE_VALUE) return false;
    wchar_t path[MAX_PATH * 2 + 80];
    path_of(path, g_stem, L".txt");
    g_text_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return true;
}

// Replaces what a report file holds so far.
void rewrite(HANDLE file, std::string_view text) {
    LARGE_INTEGER start{};
    if (file == INVALID_HANDLE_VALUE || !SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return;
    if (write_all(file, text)) SetEndOfFile(file);
}

// A small minidump of the whole process into the pending file, written from
// this thread (the crashed one may have no stack left), then kept as
// <stem>.dmp. `name` gets its name, or stays empty. Guarded on its own: a
// fault in the dump writer costs only the dump, not the report.
struct Dump {
    const Request* request;
    char name[80];
};

void write_dump(void* argument) {
    auto& dump = *static_cast<Dump*>(argument);
    if (g_write_dump == nullptr || g_dump == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION exception{dump.request->thread, dump.request->pointers, FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    wchar_t name[80] = {};
    if (!g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), g_dump, type, &exception, nullptr, nullptr) ||
        !keep_dump(g_stem, name)) {
        return;
    }
    for (size_t i = 0; i + 1 < std::size(dump.name) && name[i] != L'\0'; ++i) dump.name[i] = static_cast<char>(name[i]);
}

void write_report(void*) {
    const Request request = g_request;
    const EXCEPTION_RECORD* record = request.pointers->ExceptionRecord;
    if (!open_reports()) return;

    const Setup& setup = *g_setup;
    const CrashSources sources = sources_now();
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    ReportInput in;
    in.record = record;
    in.context = request.pointers->ContextRecord;
    in.thread = request.thread;
    in.main_thread = request.thread == g_main_thread;
    in.stack_low = request.stack_low;
    in.stack_high = request.stack_high;
    in.sources = &sources;
    in.anvil_version = setup.anvil_version;
    GetLocalTime(&in.time);
    in.seconds = frequency.QuadPart != 0 && setup.start.QuadPart != 0
                     ? static_cast<double>(now.QuadPart - setup.start.QuadPart) / static_cast<double>(frequency.QuadPart)
                     : 0.0;
    in.before_game_ready = !g_game_ready.load();
    in.unfinished_starts = setup.unfinished_starts;
    in.safe_mode = setup.safe_mode;
    in.seen = g_seen;
    in.seen_count = static_cast<size_t>(g_seen_count) < kSeenMax ? static_cast<size_t>(g_seen_count) : kSeenMax;
    in.log_tail = read_log_tail();
    in.minidump = nullptr;

    // The report first: a minidump writer stuck on a lock the crashed thread
    // holds costs only the dump. With the dump kept, the report names it.
    g_json_text.clear();
    g_text_text.clear();
    CrashVerdict verdict = build_report(in, g_json_text, g_text_text);
    write_all(g_json_file, g_json_text.view());
    if (g_text_file != INVALID_HANDLE_VALUE) write_all(g_text_file, g_text_text.view());
    Dump dump{&request, {}};
    unsigned long dump_fault = 0;
    if (call_guarded(&write_dump, &dump, &dump_fault) && dump.name[0] != '\0') {
        in.minidump = dump.name;
        g_json_text.clear();
        g_text_text.clear();
        verdict = build_report(in, g_json_text, g_text_text);
        rewrite(g_json_file, g_json_text.view());
        rewrite(g_text_file, g_text_text.view());
    }
    close_reports();
    prune(kKeep);

    // Last, in anvil.log too; the log may be held by the crashed thread.
    char line[512];
    TextBuffer message(line, sizeof(line));
    message.putf("crash 0x%08lX %s at ", record->ExceptionCode, exception_name(record->ExceptionCode));
    describe_address(reinterpret_cast<uintptr_t>(record->ExceptionAddress), sources, message);
    message.put(": ");
    describe_verdict(verdict, sources, message);
    message.putf("; report crashes\\%ls.txt", g_stem);
    Log::instance().try_write(ANVIL_LOG_ERROR, "anvil", message.view());
}

// After the report writer itself faulted: what was written so far, and why it stops.
void write_partial(unsigned long code) {
    char note[160];
    const int n = std::snprintf(note, sizeof(note), "\r\n[the report writer failed with exception 0x%08lX; the report is incomplete]\r\n", code);
    if (g_text_file != INVALID_HANDLE_VALUE) {
        rewrite(g_text_file, g_text_text.view());
        write_all(g_text_file, std::string_view(note, n > 0 ? static_cast<size_t>(n) : 0));
    }
    // The JSON is all or nothing: a stub that says so.
    if (g_json_file != INVALID_HANDLE_VALUE) {
        char json[160];
        const int k = std::snprintf(json, sizeof(json), "{\n  \"format\": 1,\n  \"incomplete\": true,\n  \"writer_exception\": \"0x%08lX\"\n}\n", code);
        rewrite(g_json_file, std::string_view(json, k > 0 ? static_cast<size_t>(k) : 0));
    }
    close_reports();
}

DWORD WINAPI helper_main(void*) {
    // Prepared before any crash: the minidump writer, loaded as the game would
    // load it (its own folder first, so a later LoadLibrary("dbghelp.dll") of
    // the game gets the same module), and an open file for the dump.
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr,
                                         LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        g_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }
    wchar_t pending[MAX_PATH * 2 + 80];
    wchar_t stem[32];
    std::swprintf(stem, std::size(stem), L"pending-%lu", GetCurrentProcessId());
    path_of(pending, stem, L".dmp");
    g_dump = CreateFileW(pending, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_dump != INVALID_HANDLE_VALUE) {
        FILE_DISPOSITION_INFO remove{TRUE};  // gone when the process ends, unless a dump is kept
        if (!SetFileInformationByHandle(g_dump, FileDispositionInfo, &remove, sizeof(remove))) {
            CloseHandle(g_dump);
            DeleteFileW(pending);
            g_dump = INVALID_HANDLE_VALUE;
        }
    }
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        unsigned long code = 0;
        if (!call_guarded(&write_report, nullptr, &code)) write_partial(code);
        SetEvent(g_done);
    }
}

// On the crashed thread: the helper writes the report while it waits. The
// first crash of the process claims the helper; a crash on another thread
// meanwhile waits for that report and goes on.
void hand_over(EXCEPTION_POINTERS* pointers) {
    if (g_wake == nullptr || GetCurrentThreadId() == g_helper) return;
    if (InterlockedCompareExchange(&g_claimed, 1, 0) != 0) {
        WaitForSingleObject(g_done, kWaitMs);
        return;
    }
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    g_request.pointers = pointers;
    g_request.thread = GetCurrentThreadId();
    g_request.stack_low = reinterpret_cast<uintptr_t>(tib->StackLimit);
    g_request.stack_high = reinterpret_cast<uintptr_t>(tib->StackBase);
    SetEvent(g_wake);
    WaitForSingleObject(g_done, kWaitMs);
}

// First chance, before any frame handler: records only.
LONG CALLBACK vectored(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    if (record == nullptr || !is_fatal_looking(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedExchange64(&g_last_fatal, static_cast<LONG64>(GetTickCount64()));
    const LONG index = InterlockedIncrement(&g_seen_count) - 1;
    if (index < static_cast<LONG>(kSeenMax)) {
        SeenException& seen = g_seen[index];
        seen.code = record->ExceptionCode;
        seen.thread = GetCurrentThreadId();
        seen.address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
        seen.parameters = record->NumberParameters;
        for (DWORD i = 0; i < 2 && i < record->NumberParameters; ++i) seen.info[i] = record->ExceptionInformation[i];
        seen.tick = GetTickCount64();
    }
    // The filter may never run on an overflowed stack: the report is written now.
    if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW) hand_over(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI filter(EXCEPTION_POINTERS* pointers) {
    hand_over(pointers);
    const Filter next = g_next.load();
    if (next != nullptr && next != &filter) return next(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Replaces eu4.exe's import: the game's filter becomes Anvil's next one, and
// the game gets back the filter it replaced, as from the real function.
Filter WINAPI set_filter(Filter replacement) {
    if (replacement == &filter) return g_next.load();
    return g_next.exchange(replacement);
}

// Points every import of `function` in `image` at `replacement`.
size_t replace_imports(HMODULE image, const char* function, void* replacement) {
    auto* base = reinterpret_cast<uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;
    size_t replaced = 0;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name != 0; ++desc) {
        if (desc->OriginalFirstThunk == 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            if (write_memory(&slots->u1.Function, &replacement, sizeof(replacement))) ++replaced;
        }
    }
    return replaced;
}

}  // namespace

bool install(const Setup& setup, std::string& error) {
    if (g_installed.exchange(true)) return true;
    g_setup = new Setup(setup);  // lives for the process
    g_main_thread = GetCurrentThreadId();
    std::error_code ignored;
    std::filesystem::create_directories(setup.crash_dir, ignored);
    std::swprintf(g_dir, std::size(g_dir), L"%ls\\", setup.crash_dir.wstring().c_str());
    std::swprintf(g_log, std::size(g_log), L"%ls", setup.log_file.wstring().c_str());
    prune(kKeep);

    bool ok = true;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE helper = g_wake != nullptr && g_done != nullptr
                        ? CreateThread(nullptr, 512 * 1024, helper_main, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_helper)
                        : nullptr;
    if (helper == nullptr) {
        error = "cannot start the crash helper thread; no crash reports";
        g_wake = nullptr;
        ok = false;
    } else {
        CloseHandle(helper);
    }
    if (AddVectoredExceptionHandler(1, vectored) == nullptr) {
        error = "cannot register the vectored exception handler";
        ok = false;
    }
    // Room for the handlers when this thread (the game's main thread)
    // overflows its stack. It also holds the CPU state Windows saves with the
    // exception, 11.5 KB with AMX, which the default leaves no room for; the
    // game's other threads get thread_guarantee() as they start.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);

    g_real_set = reinterpret_cast<SetFilterFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter"));
    if (g_real_set == nullptr) {
        error = "SetUnhandledExceptionFilter not found";
        return false;
    }
    const Filter previous = g_real_set(&filter);
    if (previous != &filter) g_next.store(previous);
    if (setup.image != nullptr &&
        replace_imports(setup.image, "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&set_filter)) == 0) {
        error = "the game does not import SetUnhandledExceptionFilter; a filter it sets replaces Anvil's";
        ok = false;
    }
    return ok;
}

ULONG thread_guarantee_for(DWORD context_bytes) {
    constexpr DWORD kDefaultHolds = 8 * 1024;  // the default leaves about 12 KB
    constexpr DWORD kHandlers = 8 * 1024;      // dispatch ~1 KB, Anvil's handlers ~2 KB, the rest margin
    constexpr DWORD kPage = 4096;
    if (context_bytes <= kDefaultHolds) return 0;
    return (context_bytes + kHandlers + kPage - 1) / kPage * kPage;
}

ULONG thread_guarantee() {
    // Looked up: Windows 10 before build 20348 has no InitializeContext2, and
    // an import of it would keep version.dll (and the game) from loading.
    using InitializeContext2Fn = BOOL(WINAPI*)(PVOID, DWORD, PCONTEXT*, PDWORD, ULONG64);
    const auto initialize = reinterpret_cast<InitializeContext2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "InitializeContext2"));
    if (initialize == nullptr) return 0;
    DWORD length = 0;
    initialize(nullptr, CONTEXT_ALL | CONTEXT_XSTATE, nullptr, &length, GetEnabledXStateFeatures());
    return thread_guarantee_for(length);
}

bool guard_thread(ULONG guarantee) {
    constexpr ULONG_PTR kSmallestGuarded = 256 * 1024;
    if (guarantee == 0) return false;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (high - low < kSmallestGuarded) return false;
    return SetThreadStackGuarantee(&guarantee) != FALSE;
}

void set_game_ready() { g_game_ready.store(true); }

bool reported() { return g_claimed != 0; }

bool crashed() {
    // A crash the game's own handler caught ends the process soon after.
    const auto last = static_cast<ULONGLONG>(InterlockedCompareExchange64(&g_last_fatal, 0, 0));
    return reported() || (last != 0 && GetTickCount64() - last < 30000);
}

void attach_plugins(const PluginSet* plugins) { g_plugins.store(plugins, std::memory_order_release); }

bool check_filter(std::string& by) {
    if (g_real_set == nullptr) return true;
    const Filter current = g_real_set(&filter);
    if (current == &filter) return true;
    if (current == nullptr) {
        by = "a module cleared it; Anvil's filter is set again";
        return false;
    }
    g_real_set(current);  // the later filter stays on top
    char text[160];
    TextBuffer out(text, sizeof(text));
    describe_address(reinterpret_cast<uintptr_t>(current), sources_now(), out);
    by = std::string(out.view());
    return false;
}

}  // namespace anvil::crash
