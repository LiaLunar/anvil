#pragma once

#include "changes.hpp"
#include "plugin_symbols.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

struct AnvilPlugin;

namespace anvil {

class PluginRecord;
class PluginSet;

// The crash report (design §12): where an exception happened, which plugins
// are involved and how. Everything here runs on the crash helper thread while
// the crashed thread waits, so it neither allocates nor waits for a lock: text
// goes into fixed buffers, the registry is asked with try-locks, and memory of
// the crashed thread is read with ReadProcessMemory.

// Text in a fixed buffer; what does not fit is cut, and truncated() says so.
class TextBuffer {
public:
    TextBuffer(char* data, size_t capacity) : data_(data), capacity_(capacity) {}
    void put(std::string_view text);
    void putf(const char* format, ...);
    // A JSON string literal, quoted and escaped (control bytes as \u00XX).
    void json(std::string_view text);
    void hex(uint64_t value);  // 0x1A2B
    std::string_view view() const { return {data_, size_}; }
    bool truncated() const { return truncated_; }
    void clear() { size_ = 0, truncated_ = false; }

private:
    char* data_;
    size_t capacity_;
    size_t size_ = 0;
    bool truncated_ = false;
};

// What the report attributes addresses against. Any pointer may be null.
struct CrashSources {
    const ChangeRegistry* changes = nullptr;
    // The plugins of this start, whose handles own the changes of `changes`.
    const PluginSet* plugins = nullptr;
    uintptr_t image_base = 0;  // eu4.exe, or a test's image
    uint32_t image_size = 0;
    const char* image_name = "eu4.exe";
    const char* build_id = nullptr;  // the known game build, null if unknown
    uintptr_t anvil_base = 0;        // version.dll, named "anvil" in frames
};

struct ModuleInfo {
    uintptr_t base = 0;  // 0: no module (heap, stub memory, garbage)
    uint32_t size = 0;
    char name[64] = {};
};

// Everything known about one address.
struct Located {
    uintptr_t address = 0;
    ModuleInfo module;
    const PluginRecord* plugin = nullptr;  // the plugin whose DLL holds the address
    AddressAttribution change;    // what the change registry knows
    bool registry_busy = false;   // the registry could not be asked
    // The nearest declared symbol at or below an address in the game image.
    const PluginSymbol* symbol = nullptr;
    const PluginRecord* symbol_plugin = nullptr;  // first plugin declaring it
    int also_declared = 0;        // further plugins declaring the same address
    uintptr_t symbol_offset = 0;
    bool inside_symbol = false;   // within the symbol's declared bytes
};

void locate(uintptr_t address, const CrashSources& sources, Located& out);
// "eu4.exe+0x1A2B", "hello.dll+0x10", or the bare address outside modules.
void describe_address(uintptr_t address, const CrashSources& sources, TextBuffer& out);

// How a frame was found.
enum class FrameSource {
    Context,      // the exception's context
    Unwind,       // unwind data (RtlVirtualUnwind), or a leaf function's return address
    ContextStub,  // through a context stub: its saved AnvilCpuContext
    Trampoline,   // a trampoline unwound as the game instructions it copies
    Scan,         // a return address found on the stack; may be stale
};

const char* frame_source_name(FrameSource source);

struct Frame {
    Located where;
    FrameSource source = FrameSource::Unwind;
    uintptr_t stack = 0;   // rsp of the frame, or the stack slot of a scanned address
    uint32_t repeats = 0;  // further frames at the same address right after this one (recursion)
};

// Walks the stack from `start`: unwind data where the code has it, a context
// stub's saved context, a trampoline as the instructions it copies; anything
// else without unwind data (other Anvil stubs, code of no module) switches to
// a scan for return addresses between rsp and stack_high. Consecutive frames
// at one address count as one with `repeats`, so a stack overflow still shows
// where the recursion started.
size_t walk_stack(const CONTEXT& start, uintptr_t stack_low, uintptr_t stack_high, const CrashSources& sources,
                  Frame* out, size_t max);

// The four levels of design §12; never "X caused it".
enum class CrashLevel { NoPlugin, OnStack, InChange, InCode };

// The handles involved, most involved first: for a crash in a plugin's DLL
// its own handle, for one in a change the handle that requested it (a
// module's, ADR 0011). The report names their plugins, and the modules among
// them.
struct CrashVerdict {
    static constexpr size_t kMax = 8;
    CrashLevel level = CrashLevel::NoPlugin;
    const AnvilPlugin* handles[kMax] = {};
    size_t count = 0;
};

CrashVerdict assess(const Frame* frames, size_t count, const CrashSources& sources);
const char* crash_level_key(CrashLevel level);  // in_plugin_code, in_plugin_change, plugin_on_stack, no_plugin

// A fatal-looking exception the vectored handler saw (first chance).
struct SeenException {
    DWORD code = 0;
    DWORD thread = 0;
    uintptr_t address = 0;
    uintptr_t info[2] = {};  // access violation: read/write/execute, address
    DWORD parameters = 0;
    ULONGLONG tick = 0;      // GetTickCount64
};

bool is_fatal_looking(DWORD code);
const char* exception_name(DWORD code);

struct ReportInput {
    const EXCEPTION_RECORD* record = nullptr;
    const CONTEXT* context = nullptr;
    DWORD thread = 0;
    bool main_thread = false;
    uintptr_t stack_low = 0, stack_high = 0;  // the crashed thread's stack
    const CrashSources* sources = nullptr;
    const char* anvil_version = "";
    SYSTEMTIME time{};
    double seconds = 0;  // since Anvil started
    bool before_game_ready = true;
    unsigned unfinished_starts = 0;  // safe mode counter at this start
    bool safe_mode = false;
    const SeenException* seen = nullptr;
    size_t seen_count = 0;
    std::string_view log_tail;
    const char* minidump = nullptr;  // file name next to the report, or null
};

// Writes the report as JSON and as text; returns the verdict.
CrashVerdict build_report(const ReportInput& input, TextBuffer& json, TextBuffer& text);
// "in plugin tongs's code", "plugin a on the stack (also b)", "no plugin involved"
void describe_verdict(const CrashVerdict& verdict, const CrashSources& sources, TextBuffer& out);

}  // namespace anvil
