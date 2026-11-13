#pragma once

#include "crash_report.hpp"

#include <windows.h>

#include <filesystem>
#include <string>

namespace anvil::crash {

// Crash capture (design §12). Installed once per process at the entry point:
//
//  - A vectored handler records fatal-looking exceptions (first chance) in
//    memory: no IO, no allocation, at most 8. On a stack overflow it also
//    hands the exception to the helper thread (below), because the crashed
//    thread may have no stack left for the filter.
//  - An unhandled-exception filter ahead of EU4's: Anvil sets it before the
//    game's C runtime starts and replaces eu4.exe's import of
//    SetUnhandledExceptionFilter. The game's own calls (its C runtime, its
//    crash reporter) then only record their filter as Anvil's next one and get
//    back the one they replaced, so Anvil's filter stays on top: it has the
//    report written, then calls the game's filter and returns what that
//    returns. A filter set through another module's import is detected when
//    the game is ready (anvil-game.ready) and logged (check_filter).
//  - A helper thread, started here with its own stack, the minidump writer
//    loaded and a minidump file already open, writes every report while the
//    crashed thread waits, so a crash with little stack or a broken heap is
//    still written. After the report it adds a small minidump and names it
//    in the report.
//
// Reports go to <crash_dir>/<time>.json, .txt and .dmp; the last 10 are kept.
// While the process runs, the minidump file waits as pending-<pid>.dmp, which
// the system deletes when the process ends unless a crash kept it. Not
// caught: __fastfail and /GS failures (they bypass both handlers),
// TerminateProcess, and exceptions a frame handler of the game catches first.
struct Setup {
    std::filesystem::path crash_dir;  // anvil/crashes
    std::filesystem::path log_file;   // anvil.log: its end goes into the report
    HMODULE image = nullptr;          // whose import of SetUnhandledExceptionFilter is replaced (eu4.exe)
    CrashSources sources;             // what addresses are attributed against
    const char* anvil_version = "";
    LARGE_INTEGER start{};            // time 0 of "seconds since start"
    unsigned unfinished_starts = 0;   // from the start marker (safe_mode.hpp)
    bool safe_mode = false;
};

// False (with `error`) when a part could not be set up; the rest still works.
// A second call does nothing.
bool install(const Setup& setup, std::string& error);

// The plugins of this start, once the plugin set is made (it comes after
// install, before any plugin's code runs); reports name them from then on.
void attach_plugins(const PluginSet* plugins);

// A stack overflow on a thread reaches the handlers only when the CPU state
// Windows saves with the exception fits in the stack left after the overflow:
// about 12 KB with the default guarantee. That holds the 1.7 KB of AVX and
// the 3.2 KB of AVX-512, but not the 11.5 KB where the host enables AMX: there
// Windows ends the process before any handler runs. The game's main thread
// gets 64 KB at install; its other threads get thread_guarantee() from
// version.dll's DllMain when they start (guard_thread).
//
// The guarantee a thread needs for an exception context of `context_bytes`
// (with every extended state): 0 where the default holds it (8 KB or less),
// otherwise the context and 8 KB for the dispatch and Anvil's handlers,
// rounded up to whole pages (20 KB for AMX's 11.5 KB).
ULONG thread_guarantee_for(DWORD context_bytes);

// thread_guarantee_for this host's exception context. 0 where Windows has no
// InitializeContext2 (before build 20348, which has no AMX either).
ULONG thread_guarantee();

// Gives the calling thread `guarantee` (when not 0) if its stack reservation
// is at least 256 KB; a smaller stack would lose too much of its room.
// Whether it did.
bool guard_thread(ULONG guarantee);

// The game is ready: later reports say "after game_ready".
void set_game_ready();

// A report was written (or is being written) in this process. Such a start
// is not clean even when the game goes on: EU4's handler may let the process
// run after a crash on another thread.
bool reported();

// A report was written, or a fatal-looking exception came in the last 30
// seconds (one the game's own handler caught before exiting). The start
// marker uses it at exit: such an exit is not clean.
bool crashed();

// Whether Anvil's filter is still the process's top-level filter. When another
// module replaced it, the replacement stays (it was installed later and
// usually chains to the filter it replaced) and `by` names its address.
bool check_filter(std::string& by);

}  // namespace anvil::crash
