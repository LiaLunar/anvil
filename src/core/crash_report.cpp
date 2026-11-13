#include "crash_report.hpp"

#include "plugin_set.hpp"

#include <anvil/anvil_plugin.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace anvil {

// ---- text ------------------------------------------------------------------

void TextBuffer::put(std::string_view text) {
    const size_t room = capacity_ - size_;
    const size_t n = text.size() < room ? text.size() : room;
    std::memcpy(data_ + size_, text.data(), n);
    size_ += n;
    if (n < text.size()) truncated_ = true;
}

void TextBuffer::putf(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n < 0) return;
    put({line, static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1});
}

void TextBuffer::json(std::string_view text) {
    put("\"");
    for (const char c : text) {
        switch (c) {
        case '"': put("\\\""); break;
        case '\\': put("\\\\"); break;
        case '\n': put("\\n"); break;
        case '\r': put("\\r"); break;
        case '\t': put("\\t"); break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                putf("\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                put({&c, 1});
            }
        }
    }
    put("\"");
}

void TextBuffer::hex(uint64_t value) { putf("0x%llX", static_cast<unsigned long long>(value)); }

namespace {

// ---- memory and modules ----------------------------------------------------

// Reads memory of this process without faulting.
bool read_memory(uintptr_t address, void* out, size_t size) {
    SIZE_T read = 0;
    return address != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &read) &&
           read == size;
}

void copy_name(char (&out)[64], const char* name) {
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < sizeof(out); ++i) out[i] = name[i];
    out[i] = '\0';
}

// The module of an address without the loader lock: the inverted function
// table (RtlPcToFileHeader), the size from the PE header, the name from what
// Anvil knows or the module's export directory.
void find_module(uintptr_t address, const CrashSources& s, ModuleInfo& out) {
    out = ModuleInfo{};
    if (s.image_size != 0 && address >= s.image_base && address - s.image_base < s.image_size) {
        out.base = s.image_base;
        out.size = s.image_size;
        copy_name(out.name, s.image_name);
        return;
    }
    void* base = nullptr;
    if (RtlPcToFileHeader(reinterpret_cast<void*>(address), &base) == nullptr || base == nullptr) return;
    out.base = reinterpret_cast<uintptr_t>(base);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (read_memory(out.base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        read_memory(out.base + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE) {
        out.size = nt.OptionalHeader.SizeOfImage;
    }
    if (const PluginRecord* p = s.plugins != nullptr ? s.plugins->plugin_with_dll(out.base) : nullptr) {
        copy_name(out.name, p->manifest().entry.c_str());
        return;
    }
    if (out.base == s.anvil_base) {
        copy_name(out.name, "version.dll (anvil)");
        return;
    }
    const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY exports{};
    if (out.size != 0 && dir.VirtualAddress != 0 && read_memory(out.base + dir.VirtualAddress, &exports, sizeof(exports)) &&
        exports.Name != 0 && read_memory(out.base + exports.Name, out.name, sizeof(out.name) - 1)) {
        out.name[sizeof(out.name) - 1] = '\0';
        return;
    }
    copy_name(out.name, "module");
}

bool in_image(const CrashSources& s, uintptr_t address) {
    return s.image_size != 0 && address >= s.image_base && address - s.image_base < s.image_size;
}

}  // namespace

void locate(uintptr_t address, const CrashSources& s, Located& out) {
    out = Located{};
    out.address = address;
    find_module(address, s, out.module);
    if (s.plugins != nullptr) out.plugin = s.plugins->plugin_with_dll(out.module.base);
    if (s.changes != nullptr) {
        // Another thread may hold the lock for a moment; a crashed one for good.
        bool asked = false;
        for (int attempt = 0; attempt < 5 && !asked; ++attempt) {
            asked = s.changes->attribute(address, out.change);
            if (!asked) Sleep(2);
        }
        out.registry_busy = !asked;
    }
    if (!in_image(s, address) || s.plugins == nullptr) return;
    const NearestSymbol nearest = s.plugins->nearest_symbol(address);
    out.symbol = nearest.symbol;
    out.symbol_plugin = nearest.plugin;
    out.also_declared = nearest.also_declared;
    if (out.symbol != nullptr) {
        out.symbol_offset = address - out.symbol->address;
        out.inside_symbol = out.symbol_offset < out.symbol->declaration.bytes.size();
    }
}

void describe_address(uintptr_t address, const CrashSources& sources, TextBuffer& out) {
    ModuleInfo module;
    find_module(address, sources, module);
    if (module.base != 0) {
        out.putf("%s+0x%llX", module.name, static_cast<unsigned long long>(address - module.base));
    } else {
        out.hex(address);
    }
}

const char* frame_source_name(FrameSource source) {
    switch (source) {
    case FrameSource::Context: return "context";
    case FrameSource::Unwind: return "unwind";
    case FrameSource::ContextStub: return "context stub";
    case FrameSource::Trampoline: return "trampoline";
    case FrameSource::Scan: return "scan";
    }
    return "?";
}

namespace {

// The bytes before `v` end in a call instruction: E8 rel32, FF /2 in its
// common forms (register, [reg], [reg+disp8], [rip/reg+disp32], with REX).
bool after_call(uintptr_t v) {
    uint8_t b[8] = {};
    if (v < 8 || !read_memory(v - 8, b, sizeof(b))) return false;
    const auto call_modrm = [](uint8_t modrm) { return (modrm & 0x38) == 0x10; };
    if (b[3] == 0xE8) return true;                                                          // call rel32
    if (b[2] == 0xFF && call_modrm(b[3]) && ((b[3] >> 6) == 2 || (b[3] & 0xC7) == 0x05)) return true;  // disp32
    if (b[6] == 0xFF && call_modrm(b[7]) && ((b[7] >> 6) == 3 || ((b[7] >> 6) == 0 && (b[7] & 7) != 4 && (b[7] & 7) != 5))) {
        return true;  // call reg / call [reg]
    }
    if (b[5] == 0xFF && call_modrm(b[6]) && (b[6] >> 6) == 1 && (b[6] & 7) != 4) return true;  // [reg+disp8]
    if (b[4] == 0xFF && call_modrm(b[5]) && (b[5] >> 6) == 1 && (b[5] & 7) == 4) return true;  // [sib+disp8]
    if (b[1] == 0xFF && call_modrm(b[2]) && (b[2] >> 6) == 2 && (b[2] & 7) == 4) return true;  // [sib+disp32]
    return false;
}

// A value on the stack that may be a return address.
bool return_candidate(uintptr_t v, const CrashSources& s) {
    if (v < 0x10000 || (v >> 47) != 0) return false;
    if (in_image(s, v)) return after_call(v);
    void* base = nullptr;
    if (RtlPcToFileHeader(reinterpret_cast<void*>(v), &base) != nullptr && base != nullptr) return after_call(v);
    AddressAttribution a;
    return s.changes != nullptr && s.changes->attribute(v, a) && a.role != AddressRole::None && after_call(v);
}

void load_context(CONTEXT& ctx, const AnvilCpuContext& cpu) {
    ctx.Rax = cpu.rax, ctx.Rbx = cpu.rbx, ctx.Rcx = cpu.rcx, ctx.Rdx = cpu.rdx;
    ctx.Rsi = cpu.rsi, ctx.Rdi = cpu.rdi, ctx.Rbp = cpu.rbp, ctx.Rsp = cpu.rsp;
    ctx.R8 = cpu.r8, ctx.R9 = cpu.r9, ctx.R10 = cpu.r10, ctx.R11 = cpu.r11;
    ctx.R12 = cpu.r12, ctx.R13 = cpu.r13, ctx.R14 = cpu.r14, ctx.R15 = cpu.r15;
    ctx.EFlags = static_cast<DWORD>(cpu.rflags);
}

}  // namespace

size_t walk_stack(const CONTEXT& start, uintptr_t stack_low, uintptr_t stack_high, const CrashSources& s,
                  Frame* out, size_t max) {
    const auto on_stack = [&](uintptr_t rsp) {
        return stack_high == 0 || (rsp >= stack_low && rsp < stack_high);
    };
    CONTEXT ctx = start;
    FrameSource source = FrameSource::Context;
    size_t n = 0;
    uintptr_t scan_from = 0;
    // A recursion fills a whole stack: up to 1 MB of 16-byte frames.
    for (size_t step = 0; step < 0x20000; ++step) {
        Frame* frame = nullptr;
        if (n > 0 && out[n - 1].where.address == ctx.Rip && out[n - 1].source == source) {
            frame = &out[n - 1];
            ++frame->repeats;
        } else {
            if (n == max) return n;
            frame = &out[n++];
            frame->source = source;
            frame->stack = ctx.Rsp;
            frame->repeats = 0;
            locate(ctx.Rip, s, frame->where);
        }
        const Located& at = frame->where;
        const uintptr_t rsp = ctx.Rsp;
        if (!on_stack(rsp)) return n;  // a broken rsp: nothing to unwind or scan
        if (at.change.context_return) {
            // Returned to the shared context stub: its spare slot names the
            // site, rbx is its saved context, and the game was at the site's
            // address with those registers.
            uintptr_t state = 0;
            AddressAttribution site;
            AnvilCpuContext cpu{};
            if (s.changes == nullptr ||
                !read_memory(rsp + ChangeRegistry::context_stub_site_slot(), &state, sizeof(state)) ||
                !s.changes->attribute_context(state, site) || !read_memory(ctx.Rbx, &cpu, sizeof(cpu)) ||
                !on_stack(cpu.rsp)) {
                scan_from = rsp;
                break;
            }
            frame->where.change = site;
            load_context(ctx, cpu);
            ctx.Rip = at.change.site;
            source = FrameSource::ContextStub;
            continue;
        }
        uintptr_t pc = ctx.Rip;
        source = FrameSource::Unwind;
        if (at.change.role == AddressRole::Trampoline && at.change.original != 0) {
            pc = at.change.original;
            source = FrameSource::Trampoline;
        } else if (at.change.role == AddressRole::Stub || at.change.role == AddressRole::Trampoline ||
                   at.change.role == AddressRole::StubMemory) {
            scan_from = rsp;  // no unwind data in Anvil's stubs
            break;
        }
        DWORD64 image = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image, nullptr);
        if (function != nullptr) {
            ctx.Rip = pc;
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, function, &ctx, &handler_data, &establisher, nullptr);
        } else if (step == 0 || at.module.base != 0) {
            // A leaf function, or a call through a bad pointer: the return
            // address is on top of the stack.
            uintptr_t back = 0;
            if (!on_stack(rsp) || !read_memory(rsp, &back, sizeof(back))) break;
            ctx.Rip = back;
            ctx.Rsp = rsp + 8;
        } else {
            scan_from = rsp;
            break;
        }
        if (ctx.Rip == 0) return n;  // the thread's first frame
        if (ctx.Rsp <= rsp || !on_stack(ctx.Rsp)) {
            scan_from = rsp;
            break;
        }
    }
    if (scan_from == 0 || n >= max || stack_high == 0) return n;
    // Scan: return addresses between the last good rsp and the stack's base.
    const uintptr_t end = stack_high < scan_from + 0x20000 ? stack_high : scan_from + 0x20000;
    for (uintptr_t slot = (scan_from + 7) & ~static_cast<uintptr_t>(7); slot + 8 <= end && n < max; slot += 8) {
        uintptr_t v = 0;
        if (!read_memory(slot, &v, sizeof(v)) || !return_candidate(v, s)) continue;
        Frame& frame = out[n++];
        frame.source = FrameSource::Scan;
        frame.stack = slot;
        frame.repeats = 0;
        locate(v, s, frame.where);
    }
    return n;
}

namespace {

void add_handle(CrashVerdict& v, const AnvilPlugin* handle) {
    if (handle == nullptr) return;
    for (size_t i = 0; i < v.count; ++i) {
        if (v.handles[i] == handle) return;
    }
    if (v.count < CrashVerdict::kMax) v.handles[v.count++] = handle;
}

// The plugin whose DLL holds the address: its own handle.
void add_plugin(CrashVerdict& v, const PluginRecord* plugin) {
    if (plugin != nullptr) add_handle(v, plugin->handle());
}

// The handles that requested the changes at the address; the core's own
// changes have none.
void add_owners(CrashVerdict& v, const AddressAttribution& a, const CrashSources& s) {
    if (s.plugins == nullptr) return;
    for (size_t i = 0; i < a.count; ++i) add_handle(v, s.plugins->handle_of(a.changes[i].owner));
}

}  // namespace

CrashVerdict assess(const Frame* frames, size_t count, const CrashSources& sources) {
    CrashVerdict v;
    if (count == 0) return v;
    const Located& top = frames[0].where;
    if (top.plugin != nullptr) {
        v.level = CrashLevel::InCode;
        add_plugin(v, top.plugin);
        return v;
    }
    add_owners(v, top.change, sources);
    if (v.count > 0) {
        v.level = CrashLevel::InChange;
        return v;
    }
    for (size_t i = 1; i < count; ++i) {
        add_plugin(v, frames[i].where.plugin);
        add_owners(v, frames[i].where.change, sources);
    }
    if (v.count > 0) v.level = CrashLevel::OnStack;
    return v;
}

const char* crash_level_key(CrashLevel level) {
    switch (level) {
    case CrashLevel::InCode: return "in_plugin_code";
    case CrashLevel::InChange: return "in_plugin_change";
    case CrashLevel::OnStack: return "plugin_on_stack";
    case CrashLevel::NoPlugin: return "no_plugin";
    }
    return "?";
}

bool is_fatal_looking(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_INVALID_DISPOSITION:
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN, when raised rather than __fastfail
    case 0xC0000420:  // STATUS_ASSERTION_FAILURE
        return true;
    default: return false;
    }
}

const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION: return "invalid disposition";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point division by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "floating-point invalid operation";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun";
    case 0xC0000420: return "assertion failure";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

namespace {

const char* plugin_id(const PluginRecord* plugin) { return plugin != nullptr ? plugin->id().c_str() : "?"; }

// The handle that owns a change: a plugin's own or a module's; null for the
// core's own owner.
const AnvilPlugin* owner_handle(const CrashSources& s, OwnerId owner) {
    return s.plugins != nullptr ? s.plugins->handle_of(owner) : nullptr;
}

// "tongs" for a plugin's own handle, "smithy.drill" for a module's, "?" for no plugin's.
const char* owner_name(const CrashSources& s, OwnerId owner) {
    const AnvilPlugin* h = owner_handle(s, owner);
    return h != nullptr ? h->name.c_str() : "?";
}

// The verdict's plugins once each, most involved first.
template <class Fn> void each_plugin(const CrashVerdict& v, Fn&& fn) {
    for (size_t i = 0; i < v.count; ++i) {
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) seen = v.handles[j]->record == v.handles[i]->record;
        if (!seen) fn(*v.handles[i]->record);
    }
}

// "eu4.exe+0x1A2B" or "0x1A2B".
void put_where(TextBuffer& out, const Located& at) {
    if (at.module.base != 0) {
        out.putf("%s+0x%llX", at.module.name, static_cast<unsigned long long>(at.address - at.module.base));
    } else {
        out.hex(at.address);
    }
}

void put_symbol(TextBuffer& out, const Located& at, const CrashSources& s) {
    (void)s;
    out.putf("%s+0x%llX (declared by %s", at.symbol->declaration.name.c_str(),
             static_cast<unsigned long long>(at.symbol_offset), plugin_id(at.symbol_plugin));
    if (at.also_declared > 0) out.putf(" and %d more", at.also_declared);
    out.put(at.inside_symbol ? ", inside its declared bytes)" : ")");
}

// "in tongs's function hook rva 0x40 [speed_object]"
void put_changes(TextBuffer& out, const Located& at, const CrashSources& s) {
    if (at.change.role == AddressRole::None) return;
    out.putf("%s", address_role_name(at.change.role));
    for (size_t i = 0; i < at.change.count; ++i) {
        const AddressChange& c = at.change.changes[i];
        out.putf("%s %s's %s", i == 0 ? " of" : ",", owner_name(s, c.owner), c.label);
        if (c.symbol[0] != '\0') out.putf(" [%s]", c.symbol);
    }
    if (at.change.total > at.change.count) out.putf(" and %zu more", at.change.total - at.change.count);
}

void json_located(TextBuffer& j, const Located& at, const CrashSources& s) {
    j.put("\"address\": \"");
    j.hex(at.address);
    j.put("\"");
    if (at.module.base != 0) {
        j.put(", \"module\": ");
        j.json(at.module.name);
        j.put(", \"offset\": \"");
        j.hex(at.address - at.module.base);
        j.put("\"");
    }
    if (at.plugin != nullptr) {
        j.put(", \"plugin\": ");
        j.json(plugin_id(at.plugin));
    }
    if (at.registry_busy) j.put(", \"registry\": \"busy\"");
    if (at.change.role != AddressRole::None) {
        j.put(", \"role\": ");
        j.json(address_role_name(at.change.role));
        j.put(", \"changes\": [");
        for (size_t i = 0; i < at.change.count; ++i) {
            const AddressChange& c = at.change.changes[i];
            j.put(i == 0 ? "{" : ", {");
            // The plugin's id, as before modules; a module's change also names the module.
            const AnvilPlugin* owner = owner_handle(s, c.owner);
            j.putf("\"id\": %llu, \"plugin\": ", static_cast<unsigned long long>(c.id));
            j.json(owner != nullptr ? owner->record->id().c_str() : "?");
            if (owner != nullptr && owner->module != nullptr) {
                j.put(", \"module\": ");
                j.json(owner->name);
            }
            j.put(", \"kind\": ");
            j.json(change_kind_name(c.kind));
            j.put(", \"label\": ");
            j.json(c.label);
            if (c.symbol[0] != '\0') {
                j.put(", \"symbol\": ");
                j.json(c.symbol);
            }
            j.put("}");
        }
        j.put("]");
        if (at.change.total > at.change.count) j.putf(", \"more_changes\": %zu", at.change.total - at.change.count);
    }
    if (at.symbol != nullptr) {
        j.put(", \"symbol\": {\"name\": ");
        j.json(at.symbol->declaration.name);
        j.put(", \"offset\": \"");
        j.hex(at.symbol_offset);
        j.put("\", \"declared_by\": ");
        j.json(plugin_id(at.symbol_plugin));
        if (at.also_declared > 0) j.putf(", \"also_declared_by\": %d", at.also_declared);
        j.putf(", \"inside\": %s}", at.inside_symbol ? "true" : "false");
    }
}

const char* state_name(const PluginRecord& p) { return outcome_name(p.outcome()); }

// "1.2.3-beta", without allocating.
void put_version(TextBuffer& out, const Version& v) {
    out.putf("%u.%u.%u", v.major, v.minor, v.patch);
    if (!v.prerelease.empty()) out.putf("-%s", v.prerelease.c_str());
}

// A register value with the module it points into, if any.
void put_register_note(TextBuffer& out, uintptr_t value, const CrashSources& s) {
    ModuleInfo module;
    if (value < 0x10000) return;
    find_module(value, s, module);
    if (module.base == 0) return;
    out.putf(" (%s+0x%llX)", module.name, static_cast<unsigned long long>(value - module.base));
}

struct Register {
    const char* name;
    DWORD64 CONTEXT::*field;
};
constexpr Register kRegisters[] = {
    {"rax", &CONTEXT::Rax}, {"rbx", &CONTEXT::Rbx}, {"rcx", &CONTEXT::Rcx}, {"rdx", &CONTEXT::Rdx},
    {"rsi", &CONTEXT::Rsi}, {"rdi", &CONTEXT::Rdi}, {"rbp", &CONTEXT::Rbp}, {"rsp", &CONTEXT::Rsp},
    {"r8", &CONTEXT::R8},   {"r9", &CONTEXT::R9},   {"r10", &CONTEXT::R10}, {"r11", &CONTEXT::R11},
    {"r12", &CONTEXT::R12}, {"r13", &CONTEXT::R13}, {"r14", &CONTEXT::R14}, {"r15", &CONTEXT::R15},
    {"rip", &CONTEXT::Rip},
};

constexpr size_t kMaxFrames = 64;
Frame g_frames[kMaxFrames];  // one report at a time: the crash helper's

}  // namespace

void describe_verdict(const CrashVerdict& v, const CrashSources& s, TextBuffer& out) {
    if (v.count == 0) {
        out.put("no plugin involved");
        return;
    }
    (void)s;
    const char* first = v.handles[0]->record->id().c_str();
    switch (v.level) {
    case CrashLevel::InCode: out.putf("in plugin %s's code", first); break;
    case CrashLevel::InChange: out.putf("in plugin %s's change", first); break;
    case CrashLevel::OnStack: out.putf("plugin %s on the stack", first); break;
    case CrashLevel::NoPlugin: out.put("no plugin involved"); return;
    }
    size_t n = 0;
    each_plugin(v, [&](const PluginRecord& p) {
        if (n++ > 0) out.putf("%s%s", n == 2 ? " (also " : ", ", p.id().c_str());
    });
    if (n > 1) out.put(")");
}

CrashVerdict build_report(const ReportInput& in, TextBuffer& j, TextBuffer& t) {
    static const CrashSources none;
    const CrashSources& s = in.sources != nullptr ? *in.sources : none;
    const EXCEPTION_RECORD& record = *in.record;
    size_t frames = 0;
    if (in.context != nullptr) {
        frames = walk_stack(*in.context, in.stack_low, in.stack_high, s, g_frames, kMaxFrames);
    } else {
        locate(reinterpret_cast<uintptr_t>(record.ExceptionAddress), s, g_frames[0].where);
        g_frames[0].source = FrameSource::Context;
        frames = 1;
    }
    const CrashVerdict verdict = assess(g_frames, frames, s);
    const Located& top = g_frames[0].where;
    char verdict_text[256];
    TextBuffer vt(verdict_text, sizeof(verdict_text));
    describe_verdict(verdict, s, vt);

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02u", in.time.wYear, in.time.wMonth, in.time.wDay,
                  in.time.wHour, in.time.wMinute, in.time.wSecond);
    const DWORD code = record.ExceptionCode;
    const bool av = (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && record.NumberParameters >= 2;
    const char* access = !av ? nullptr
                         : record.ExceptionInformation[0] == 0 ? "read"
                         : record.ExceptionInformation[0] == 8 ? "execute"
                                                               : "write";

    // ---- text
    t.put("Anvil crash report\r\n==================\r\n\r\n");
    t.putf("Anvil %s (plugin API %d), game build %s\r\n", in.anvil_version, ANVIL_API_VERSION,
           s.build_id != nullptr ? s.build_id : "unknown");
    t.putf("Time: %s local, %.1f s after Anvil started%s\r\n", stamp, in.seconds,
           in.before_game_ready ? ", before the game was ready" : "");
    if (in.safe_mode) t.put("This start was in safe mode: no plugins were loaded.\r\n");
    t.putf("Exception: 0x%08lX %s", code, exception_name(code));
    if (access != nullptr) {
        t.putf(" (%s at ", access);
        t.hex(record.ExceptionInformation[1]);
        t.put(")");
    }
    t.putf("\r\nThread: %lu%s\r\nAddress: ", in.thread, in.main_thread ? " (main thread)" : "");
    put_where(t, top);
    t.put("\r\n\r\nVerdict: ");
    t.put(vt.view());
    t.put(".\r\n");
    t.put("  The verdict says where the crash happened and which plugins were involved, not who caused it.\r\n");
    if (top.change.role != AddressRole::None) {
        t.put("  The address is in the ");
        put_changes(t, top, s);
        t.put(".\r\n");
    }
    if (top.symbol != nullptr) {
        t.put("  Nearest declared symbol: ");
        put_symbol(t, top, s);
        t.put(".\r\n");
    }
    if (top.registry_busy) t.put("  The change registry was busy (held by the crashed thread?); changes are not listed.\r\n");
    for (size_t i = 0; i < verdict.count; ++i) {
        const AnvilPlugin& h = *verdict.handles[i];
        if (h.module != nullptr) t.putf("  Through module %s of plugin %s.\r\n", h.name.c_str(), h.record->id().c_str());
    }
    each_plugin(verdict, [&](const PluginRecord& p) {
        if (!p.manifest().issues.empty()) {
            t.putf("  Report it to %s: %s\r\n", p.id().c_str(), p.manifest().issues.c_str());
        } else {
            t.putf("  %s has no issues link in its plugin.toml.\r\n", p.id().c_str());
        }
    });

    t.put("\r\nStack (innermost first; \"scan\" frames are return addresses found on the stack and may be stale):\r\n");
    for (size_t i = 0; i < frames; ++i) {
        const Frame& f = g_frames[i];
        t.putf("  #%-2zu ", i);
        put_where(t, f.where);
        if (f.where.plugin != nullptr) t.putf("  [plugin %s]", plugin_id(f.where.plugin));
        if (f.where.symbol != nullptr) {
            t.put("  ");
            put_symbol(t, f.where, s);
        }
        if (f.where.change.role != AddressRole::None) {
            t.put("  [");
            put_changes(t, f.where, s);
            t.put("]");
        }
        t.putf("  (%s)", frame_source_name(f.source));
        if (f.repeats > 0) t.putf("  and %u more frame(s) here", f.repeats);
        t.put("\r\n");
    }

    if (in.context != nullptr) {
        t.put("\r\nRegisters:\r\n");
        for (const Register& r : kRegisters) {
            const DWORD64 value = in.context->*r.field;
            t.putf("  %-3s 0x%016llX", r.name, static_cast<unsigned long long>(value));
            put_register_note(t, value, s);
            t.put("\r\n");
        }
        t.putf("  rflags 0x%08lX\r\n", in.context->EFlags);
    }

    // The plugins Anvil loads at this start (not the skipped ones).
    const size_t loadable = s.plugins != nullptr ? s.plugins->resolved_count() : 0;
    t.putf("\r\nPlugins (%zu):\r\n", loadable);
    for (size_t i = 0; i < loadable; ++i) {
        const PluginRecord& p = s.plugins->at(i);
        const Manifest& m = p.manifest();
        t.putf("  %s ", m.id.c_str());
        put_version(t, m.version);
        t.putf(", %s, %s phase%s", state_name(p), m.phase == Phase::Early ? "early" : "main",
               m.affects_gameplay ? ", affects gameplay" : "");
        if (!m.issues.empty()) t.putf(", issues: %s", m.issues.c_str());
        t.put("\r\n");
    }

    if (in.seen_count > 0) {
        t.put("\r\nFatal-looking exceptions seen first (vectored handler; some may have been handled):\r\n");
        for (size_t i = 0; i < in.seen_count; ++i) {
            const SeenException& e = in.seen[i];
            Located at;
            find_module(e.address, s, at.module);
            at.address = e.address;
            t.putf("  0x%08lX %s at ", e.code, exception_name(e.code));
            put_where(t, at);
            t.putf(", thread %lu\r\n", e.thread);
        }
    }
    if (in.minidump != nullptr) t.putf("\r\nMinidump: %s\r\n", in.minidump);
    if (!in.log_tail.empty()) {
        t.put("\r\nEnd of anvil.log:\r\n");
        t.put(in.log_tail);
        if (in.log_tail.back() != '\n') t.put("\r\n");
    }

    // ---- JSON
    j.put("{\n  \"format\": 1,\n  \"anvil\": ");
    j.json(in.anvil_version);
    j.putf(",\n  \"plugin_api\": %d,\n  \"game_build\": ", ANVIL_API_VERSION);
    if (s.build_id != nullptr) {
        j.json(s.build_id);
    } else {
        j.put("null");
    }
    j.put(",\n  \"time\": ");
    j.json(stamp);
    j.putf(",\n  \"seconds_since_start\": %.1f,\n  \"before_game_ready\": %s,\n  \"safe_mode\": %s,\n"
           "  \"unfinished_starts\": %u,\n",
           in.seconds, in.before_game_ready ? "true" : "false", in.safe_mode ? "true" : "false", in.unfinished_starts);
    j.putf("  \"exception\": {\"code\": \"0x%08lX\", \"name\": ", code);
    j.json(exception_name(code));
    if (access != nullptr) {
        j.put(", \"access\": ");
        j.json(access);
        j.put(", \"access_address\": \"");
        j.hex(record.ExceptionInformation[1]);
        j.put("\"");
    }
    j.putf("},\n  \"thread\": %lu,\n  \"main_thread\": %s,\n  \"level\": \"%s\",\n  \"summary\": ", in.thread,
           in.main_thread ? "true" : "false", crash_level_key(verdict.level));
    j.json(vt.view());
    // Plugin ids, as before modules: safe mode and the overlay read them.
    j.put(",\n  \"plugins_involved\": [");
    bool first_plugin = true;
    each_plugin(verdict, [&](const PluginRecord& p) {
        if (!first_plugin) j.put(", ");
        first_plugin = false;
        j.json(p.id());
    });
    j.put("],\n  \"modules_involved\": [");
    bool first_module = true;
    for (size_t i = 0; i < verdict.count; ++i) {
        if (verdict.handles[i]->module == nullptr) continue;
        if (!first_module) j.put(", ");
        first_module = false;
        j.json(verdict.handles[i]->name);
    }
    j.put("],\n  \"issues\": [");
    bool first_issue = true;
    each_plugin(verdict, [&](const PluginRecord& p) {
        if (p.manifest().issues.empty()) return;
        j.put(first_issue ? "{\"plugin\": " : ", {\"plugin\": ");
        first_issue = false;
        j.json(p.id());
        j.put(", \"url\": ");
        j.json(p.manifest().issues);
        j.put("}");
    });
    j.put("],\n  \"stack\": [\n");
    for (size_t i = 0; i < frames; ++i) {
        j.put("    {");
        json_located(j, g_frames[i].where, s);
        j.put(", \"source\": ");
        j.json(frame_source_name(g_frames[i].source));
        if (g_frames[i].repeats > 0) j.putf(", \"repeats\": %u", g_frames[i].repeats);
        j.put(i + 1 < frames ? "},\n" : "}\n");
    }
    j.put("  ],\n  \"registers\": {");
    if (in.context != nullptr) {
        bool first = true;
        for (const Register& r : kRegisters) {
            const DWORD64 value = in.context->*r.field;
            j.putf("%s\"%s\": \"0x%llX\"", first ? "" : ", ", r.name, static_cast<unsigned long long>(value));
            first = false;
        }
        j.putf(", \"rflags\": \"0x%lX\"", in.context->EFlags);
    }
    j.put("},\n  \"plugins\": [");
    for (size_t i = 0; i < loadable; ++i) {
        const PluginRecord& p = s.plugins->at(i);
        const Manifest& m = p.manifest();
        j.put(i == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ");
        j.json(m.id);
        char version[96];
        TextBuffer v(version, sizeof(version));
        put_version(v, m.version);
        j.put(", \"version\": ");
        j.json(v.view());
        j.put(", \"state\": ");
        j.json(state_name(p));
        j.putf(", \"phase\": \"%s\", \"affects_gameplay\": %s", m.phase == Phase::Early ? "early" : "main",
               m.affects_gameplay ? "true" : "false");
        if (!m.issues.empty()) {
            j.put(", \"issues\": ");
            j.json(m.issues);
        }
        j.put("}");
    }
    j.put(loadable == 0 ? "],\n  \"earlier_exceptions\": [" : "\n  ],\n  \"earlier_exceptions\": [");
    for (size_t i = 0; i < in.seen_count; ++i) {
        const SeenException& e = in.seen[i];
        Located at;
        find_module(e.address, s, at.module);
        at.address = e.address;
        j.putf("%s{\"code\": \"0x%08lX\", \"thread\": %lu, \"address\": \"0x%llX\"", i == 0 ? "" : ", ", e.code,
               e.thread, static_cast<unsigned long long>(e.address));
        if (at.module.base != 0) {
            j.put(", \"module\": ");
            j.json(at.module.name);
            j.putf(", \"offset\": \"0x%llX\"", static_cast<unsigned long long>(e.address - at.module.base));
        }
        j.put("}");
    }
    j.put("],\n  \"minidump\": ");
    if (in.minidump != nullptr) {
        j.json(in.minidump);
    } else {
        j.put("null");
    }
    j.put(",\n  \"log_tail\": [");
    bool first_line = true;
    std::string_view rest = in.log_tail;
    while (!rest.empty()) {
        const size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        j.put(first_line ? "\n    " : ",\n    ");
        first_line = false;
        j.json(line);
    }
    j.put(first_line ? "]\n}\n" : "\n  ]\n}\n");
    return verdict;
}

}  // namespace anvil
