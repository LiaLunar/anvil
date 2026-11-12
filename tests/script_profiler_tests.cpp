// anvil-script-profiler (plugins/anvil-script-profiler): its tables, owners,
// reports and texts on their own, and the plugin end to end. The DLL loads
// through the host table against a synthetic image with the declared bytes of
// symbols.toml at their Steam RVAs (CAndTrigger::Evaluate and CEffect::Execute
// run their declared bytes, undo them and jump to stand-ins) and the owner
// classes' vtables in .rdata. Stand-ins of anvil-game.game and
// anvil-overlay.panel carry the events and the clicks.
// Usage: anvil_script_profiler_tests <folder with one folder per plugin> -userdir=<folder for the CSV files>
#include "core/changes.hpp"
#include "core/host.hpp"
#include "core/manifest.hpp"
#include "core/plugin_symbols.hpp"
#include "core/services.hpp"
#include "core/settings.hpp"
#include "core/symbol_check.hpp"
#include "test_support.hpp"

#include "../plugins/anvil-script-profiler/src/owners.hpp"
#include "../plugins/anvil-script-profiler/src/recorder.hpp"
#include "../plugins/anvil-script-profiler/src/report.hpp"
#include "../plugins/anvil-script-profiler/src/texts.hpp"

#include <anvil/anvil_game.h>
#include <anvil/anvil_panel.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace anvil;
namespace sp = script_profiler;
namespace fs = std::filesystem;

namespace {

constexpr const char* kBuild = "eu4-1.37.5-steam-win64";

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}
bool any_starts(const std::vector<std::string>& list, const std::string& prefix) {
    return std::any_of(list.begin(), list.end(), [&](const std::string& s) { return s.rfind(prefix, 0) == 0; });
}

// ---- fake game objects ---------------------------------------------------------

// A zeroed object of `size` bytes, kept for the process like the game's.
std::uint8_t* object(std::size_t size) { return static_cast<std::uint8_t*>(calloc(1, size)); }
void put_pointer(std::uint8_t* at, std::uintptr_t value) { std::memcpy(at, &value, sizeof(value)); }
// An MSVC std::string with its text in place (up to 15 characters) or on the heap.
void put_string(std::uint8_t* at, const char* text) {
    const std::uint64_t size = std::strlen(text);
    if (size <= 15) {
        std::memcpy(at, text, size + 1);
        const std::uint64_t cap = 15;
        std::memcpy(at + 16, &size, 8);
        std::memcpy(at + 24, &cap, 8);
    } else {
        char* heap = _strdup(text);
        put_pointer(at, reinterpret_cast<std::uintptr_t>(heap));
        std::memcpy(at + 16, &size, 8);
        std::memcpy(at + 24, &size, 8);
    }
}
// A vector of `count` elements of `size` bytes at `first`.
void put_vector(std::uint8_t* at, const void* first, std::size_t count, std::size_t size) {
    const auto f = reinterpret_cast<std::uintptr_t>(first);
    put_pointer(at, f);
    put_pointer(at + 8, f + count * size);
    put_pointer(at + 16, f + count * size);
}

// An event "test_events.1" with two options, one mean_time_to_happen modifier,
// and a decision "my_decision", with the vtables the resolver is given.
struct World {
    std::uint8_t* event = object(2680);
    std::uint8_t* option[2] = {object(816), object(816)};
    std::uint8_t* options = object(16);
    std::uint8_t* mtth = object(240);
    std::uint8_t* decision = object(616);
    std::uint8_t* loose = object(120);  // a trigger block of no known owner

    World(std::uintptr_t event_vtable, std::uintptr_t decision_vtable) {
        put_pointer(event, event_vtable);
        put_string(event + 32, "test_events.1");
        put_pointer(options, reinterpret_cast<std::uintptr_t>(option[0]));
        put_pointer(options + 8, reinterpret_cast<std::uintptr_t>(option[1]));
        put_vector(event + 1936, options, 2, 8);
        put_vector(event + 1960 + 24, mtth, 1, 240);
        put_string(option[1] + 136, "test_events.1.b");
        put_pointer(decision, decision_vtable);
        put_string(decision + 16, "my_decision_with_a_long_name");
    }
    void* trigger() const { return event + 64; }
    void* immediate() const { return event + 304 + 80; }
    void* option_effect(int i) const { return option[i] + 80; }
    void* mtth_block() const { return mtth; }
    void* mtth_potential() const { return mtth + 112; }
    void* potential() const { return decision + 48; }
};

// ---- the synthetic eu4.exe -----------------------------------------------------

// Stand-ins of the game's two functions: a trigger with a child evaluates the
// child (a nested block), an effect with a limit evaluates the limit (a root
// trigger inside the effect).
std::map<const void*, void*> g_children;
std::map<const void*, void*> g_limits;
using EvaluateFn = bool(void* trigger, void* scope);
using ExecuteFn = void(void* effect, void* scope, int flags);
EvaluateFn* g_evaluate = nullptr;
ExecuteFn* g_execute = nullptr;
int g_evaluated = 0, g_executed = 0;

bool fake_evaluate(void* trigger, void* scope) {
    ++g_evaluated;
    if (const auto it = g_children.find(trigger); it != g_children.end()) g_evaluate(it->second, scope);
    return true;
}
void fake_execute(void* effect, void* scope, int) {
    ++g_executed;
    if (const auto it = g_limits.find(effect); it != g_limits.end()) g_evaluate(it->second, scope);
}

struct FakeEu4 {
    static constexpr uint32_t kSize = 0x25E0000;
    struct Vtable {
        uint32_t rva, slot0;
    };
    static constexpr Vtable kVtables[] = {
        {0x1CAB3A8, 0x6EDB00}, {0x1C66920, 0x4879D0}, {0x1C663B0, 0x47A5C0}, {0x1C455F8, 0x1F6BB0},
        {0x1D19CA0, 0xD2F260}, {0x1D21D00, 0xDB28A0}, {0x1CC21C8, 0x8D4CE0},
    };
    uint8_t* base = nullptr;

    FakeEu4() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
        commit(0x0, PAGE_READWRITE);
        for (uint32_t page : {0xB98000u, 0x5ED000u}) commit(page, PAGE_EXECUTE_READWRITE);
        write_headers();
        // CAndTrigger::Evaluate: its declared bytes, then add rsp,20h; pop rdi; restore rsi and rbx.
        put(0xB98200, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
                       0x8B, 0x59, 0x08, 0x48, 0x83, 0xC4, 0x20, 0x5F, 0x48, 0x8B, 0x74, 0x24, 0x10, 0x48, 0x8B, 0x5C,
                       0x24, 0x08});
        jump(0xB98200 + 34, &fake_evaluate);
        // CEffect::Execute: its declared bytes, then add rsp,20h; pop r14; restore rdi, rsi and rbp.
        put(0x5ED800, {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41,
                       0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xC4, 0x20, 0x41, 0x5E, 0x48, 0x8B, 0x7C, 0x24, 0x20,
                       0x48, 0x8B, 0x74, 0x24, 0x18, 0x48, 0x8B, 0x6C, 0x24, 0x10});
        jump(0x5ED800 + 42, &fake_execute);
        g_evaluate = reinterpret_cast<EvaluateFn*>(address(0xB98200));
        g_execute = reinterpret_cast<ExecuteFn*>(address(0x5ED800));
        // The owner classes' vtables, slot 0 pointing to a ret.
        for (const Vtable& v : kVtables) {
            commit(v.rva & ~0xFFFu, PAGE_READWRITE);
            commit(v.slot0 & ~0xFFFu, PAGE_EXECUTE_READWRITE);
            put(v.slot0, {0xC3});
            put_pointer(v.rva, address(v.slot0));
        }
    }
    ~FakeEu4() { VirtualFree(base, 0, MEM_RELEASE); }

    void commit(uint32_t page, DWORD protect) { VirtualAlloc(base + page, 0x1000, MEM_COMMIT, protect); }
    uintptr_t address(uint32_t rva) const { return reinterpret_cast<uintptr_t>(base) + rva; }
    void put(uint32_t rva, std::initializer_list<uint8_t> bytes) {
        std::memcpy(base + rva, bytes.begin(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), base + rva, bytes.size());
    }
    void put_pointer(uint32_t rva, uintptr_t value) { std::memcpy(base + rva, &value, sizeof(value)); }
    // mov rax, target; jmp rax
    template <class F> void jump(uint32_t rva, F* target) {
        put(rva, {0x48, 0xB8});
        put_pointer(rva + 2, reinterpret_cast<uintptr_t>(target));
        put(rva + 10, {0xFF, 0xE0});
    }

    void write_headers() {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        std::memcpy(base, &dos, sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 3;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.ImageBase = reinterpret_cast<uintptr_t>(base);
        nt.OptionalHeader.SectionAlignment = 0x1000;
        nt.OptionalHeader.FileAlignment = 0x200;
        nt.OptionalHeader.SizeOfImage = kSize;
        nt.OptionalHeader.SizeOfHeaders = 0x400;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        std::memcpy(base + 0x80, &nt, sizeof(nt));
        struct Layout {
            const char* name;
            uint32_t rva, size, flags;
        };
        const Layout sections[] = {
            {".text", 0x1000, 0x1B64D78, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
            {".rdata", 0x1B66000, 0x44999A, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ},
            {".data", 0x1FB0000, 0x4B4FB4, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE},
        };
        uint32_t at = 0x80 + sizeof(IMAGE_NT_HEADERS64);
        for (const auto& l : sections) {
            IMAGE_SECTION_HEADER s{};
            std::memcpy(s.Name, l.name, std::strlen(l.name));
            s.VirtualAddress = l.rva;
            s.Misc.VirtualSize = l.size;
            s.Characteristics = l.flags;
            std::memcpy(base + at, &s, sizeof(s));
            at += sizeof(s);
        }
    }
};

// ---- stand-ins of anvil-game.game and anvil-overlay.panel -------------------------

std::vector<std::pair<AnvilGameWork, void*>> g_work;
DWORD g_main_thread = 0;
uint32_t fake_main_thread_id() { return g_main_thread; }
AnvilResult fake_run_on_main_thread(AnvilGameWork work, void* context) {
    g_work.push_back({work, context});
    return ANVIL_OK;
}
void run_work() {
    auto work = std::move(g_work);
    g_work.clear();
    for (const auto& [w, c] : work) w(c);
}

struct PanelRowCopy {
    uint32_t kind;
    std::string label, value;
    uint64_t id;
};
std::vector<PanelRowCopy> g_rows;
AnvilPanelClick g_click = nullptr;
void* g_click_context = nullptr;
std::string g_title;
int g_shown = 0;
AnvilResult fake_create(const char* title, AnvilPanel** out) {
    g_title = title;
    *out = reinterpret_cast<AnvilPanel*>(0x1234);
    return ANVIL_OK;
}
AnvilResult fake_set_rows(AnvilPanel*, const AnvilPanelRow* rows, uint32_t count, uint32_t) {
    g_rows.clear();
    for (uint32_t i = 0; i < count; ++i) {
        g_rows.push_back({rows[i].kind, rows[i].label ? rows[i].label : "", rows[i].value ? rows[i].value : "", rows[i].id});
    }
    return ANVIL_OK;
}
AnvilResult fake_on_click(AnvilPanel*, AnvilPanelClick click, void* context) {
    g_click = click;
    g_click_context = context;
    return ANVIL_OK;
}
AnvilResult fake_show(AnvilPanel*, int shown) {
    g_shown = shown;
    return ANVIL_OK;
}
int fake_shown(const AnvilPanel*) { return g_shown; }
const char* fake_language() { return "en"; }

const PanelRowCopy* row_labelled(const std::string& prefix) {
    for (const PanelRowCopy& r : g_rows) {
        if (r.label.rfind(prefix, 0) == 0) return &r;
    }
    return nullptr;
}
std::uint64_t button(const std::string& label) {
    for (const PanelRowCopy& r : g_rows) {
        if (r.kind == ANVIL_PANEL_BUTTON && r.label == label) return r.id;
    }
    return 0;
}

// The texts a source file hands to the table: t("…"), t.f("…"), N_("…"), texts()("…").
std::set<std::string> used_texts(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    std::set<std::string> out;
    for (const std::string call : {"t(\"", "t.f(\"", "N_(\"", "texts()(\""}) {
        for (std::size_t at = s.find(call); at != std::string::npos; at = s.find(call, at + 1)) {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_')) continue;
            // A text split over several string literals.
            std::string text;
            std::size_t from = at + call.size();
            for (;;) {
                const std::size_t end = s.find('"', from);
                text += s.substr(from, end - from);
                std::size_t next = end + 1;
                while (next < s.size() && std::isspace(static_cast<unsigned char>(s[next]))) ++next;
                if (next >= s.size() || s[next] != '"') break;
                from = next + 1;
            }
            out.insert(text);
        }
    }
    return out;
}

std::set<std::string> placeholders(const std::string& s) {
    std::set<std::string> out;
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] == '{' && std::isdigit(static_cast<unsigned char>(s[i + 1])) && s[i + 2] == '}') out.insert(s.substr(i, 3));
    }
    return out;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---- the parts on their own ---------------------------------------------------------

void recorder_tests() {
    run("recorder: one row per root block, its calls, time and longest run; nested blocks counted", [] {
        sp::Thread* t = sp::this_thread();
        CHECK(t != nullptr && sp::this_thread() == t && t->thread_id == GetCurrentThreadId());
        if (t == nullptr) return;
        int a = 0, b = 0;
        t->table.record(&a, false, 100, 0x10);
        t->table.record(&a, false, 300, 0x20);
        t->table.record(&b, true, 50, 0x30);
        t->table.nested(false);
        t->table.nested(true);
        t->table.nested(true);
        CHECK(t->table.used() == 2 && t->table.nested_triggers() == 1 && t->table.nested_effects() == 2);
        const sp::Slot& s = t->table.nth(0);
        CHECK(s.key.load() == reinterpret_cast<std::uintptr_t>(&a) && s.calls.load() == 2 && s.ticks.load() == 400 &&
              s.max.load() == 300 && s.caller.load() == 0x10 && s.effect.load() == 0);
        // Another thread's table, summed by collect; main_ticks only from the main thread.
        std::thread([&] {
            sp::Thread* other = sp::this_thread();
            if (other != nullptr) other->table.record(&a, false, 1000, 0x40);
        }).join();
        const sp::Snapshot snap = sp::collect(GetCurrentThreadId());
        CHECK(snap.threads == 2 && snap.roots.size() == 2 && snap.nested_effects == 2);
        for (const sp::RootStat& r : snap.roots) {
            if (r.block == &a) CHECK(r.calls == 3 && r.ticks == 1400 && r.max == 1000 && r.main_ticks == 400 && !r.effect);
            if (r.block == &b) CHECK(r.calls == 1 && r.effect && r.caller == 0x30);
        }
        sp::clear_all();
        CHECK(t->table.used() == 0 && t->table.nested_effects() == 0 && sp::collect(0).roots.empty());
    });
    run("recorder: a full table only sums what does not fit", [] {
        sp::Thread* t = sp::this_thread();
        if (t == nullptr) return;
        static char blocks[sp::ThreadTable::kLimit + 10];
        for (std::uint32_t i = 0; i < sp::ThreadTable::kLimit + 10; ++i) t->table.record(&blocks[i], false, 2, 0);
        CHECK(t->table.used() == sp::ThreadTable::kLimit && t->table.dropped_calls() == 10 &&
              t->table.dropped_ticks() == 20);
        t->table.record(&blocks[0], false, 2, 0);  // one it has still counts
        CHECK(t->table.nth(0).calls.load() == 2);
        sp::clear_all();
    });
}

constexpr std::uintptr_t kEventVtable = 0x1400001000;
constexpr std::uintptr_t kDecisionVtable = 0x1400002000;

void owner_tests() {
    sp::Vtables v;
    v.event = kEventVtable;
    v.decision = kDecisionVtable;
    const World w(kEventVtable, kDecisionVtable);
    run("owners: an event's trigger and immediate, a decision's potential, nothing for a stranger", [&] {
        sp::Resolver r(v);
        const sp::Owner& t = r.owner_of(w.trigger(), false);
        CHECK(t.kind == sp::OwnerKind::event && t.name == "test_events.1" && std::string(t.role) == "trigger" &&
              t.object == w.event);
        const sp::Owner& i = r.owner_of(w.immediate(), true);
        CHECK(i.kind == sp::OwnerKind::event && std::string(i.role) == "immediate");
        CHECK(r.owner_of(w.trigger(), true).kind == sp::OwnerKind::event);  // cached by address
        const sp::Owner& d = r.owner_of(w.potential(), false);
        CHECK(d.kind == sp::OwnerKind::decision && d.name == "my_decision_with_a_long_name" &&
              std::string(d.role) == "potential");
        CHECK(r.owner_of(w.loose, false).kind == sp::OwnerKind::none);
        CHECK(r.owner_of(reinterpret_cast<void*>(0x20), false).kind == sp::OwnerKind::none);  // unreadable
        sp::Resolver absent(sp::Vtables{});
        CHECK(absent.owner_of(w.trigger(), false).kind == sp::OwnerKind::none);
    });
    run("owners: an option and the mean_time_to_happen are found once their event is", [&] {
        sp::Resolver r(v);
        CHECK(r.owner_of(w.option_effect(1), true).kind == sp::OwnerKind::none);
        CHECK(r.owner_of(w.trigger(), false).kind == sp::OwnerKind::event);
        CHECK(r.owner_of(w.option_effect(1), true).kind == sp::OwnerKind::none);  // still the cached answer
        r.retry_unowned();
        const sp::Owner& o = r.owner_of(w.option_effect(1), true);
        CHECK(o.kind == sp::OwnerKind::event_option && o.option == 2 && o.name == "test_events.1" &&
              std::string(o.role) == "effect" && o.object == w.option[1]);
        CHECK(std::string(r.owner_of(w.mtth_block(), false).role) == "mean_time_to_happen" &&
              std::string(r.owner_of(w.mtth_potential(), false).role) == "mean_time_to_happen");
        // Without reading memory only what is known is named.
        sp::Resolver cold(v);
        CHECK(cold.owner_of(w.trigger(), false, false).kind == sp::OwnerKind::none);
    });
    run("report: blocks merge by owner and role; blocks without an owner by caller in the CSV only", [&] {
        sp::Resolver r(v);
        r.owner_of(w.trigger(), false);
        sp::Snapshot snap;
        snap.threads = 1;
        snap.roots = {
            {w.trigger(), false, 10, 3000, 900, 3000, 0x111},
            {w.mtth_block(), false, 4, 1000, 300, 0, 0x222},
            {w.mtth_potential(), false, 4, 500, 200, 0, 0x222},
            {w.loose, false, 2, 6000, 5000, 6000, 0xABC},
            {w.loose + 8, false, 1, 100, 100, 0, 0xDEF},
            {w.immediate(), true, 1, 3000000, 3000000, 3000000, 0x333},
        };
        sp::Span span;
        span.ending = sp::Ending::complete;
        span.days = 30;
        span.first_date = "1719.3.18";
        span.last_date = "1719.4.16";
        span.ticks_per_second = 1.0e9;
        span.anvil_version = "1.22.0";
        span.game_build = kBuild;
        const sp::Report split = sp::make_report(snap, r, span, true);
        CHECK(split.triggers.size() == 4 && split.effects.size() == 1 && split.root_calls == 22);
        const sp::Report merged = sp::make_report(snap, r, span, false);
        CHECK(merged.triggers.size() == 3);
        if (merged.triggers.size() != 3) return;
        CHECK(merged.triggers[0].kind == sp::OwnerKind::none && merged.triggers[0].calls == 3 &&
              merged.triggers[0].ticks == 6100 && merged.triggers[0].max == 5000);
        CHECK(merged.triggers[1].kind == sp::OwnerKind::event && merged.triggers[1].role == "trigger");
        CHECK(merged.triggers[2].role == "mean_time_to_happen" && merged.triggers[2].calls == 8 &&
              merged.triggers[2].ticks == 1500 && merged.triggers[2].max == 300);
        const std::string text = sp::csv(split);
        CHECK(text.rfind("# format=1 plugin=anvil-script-profiler version=1.22.0 build=eu4-1.37.5-steam-win64 "
                         "status=complete ",
                         0) == 0);
        CHECK(text.find("\nlist,rank,owner,name,option,role,calls,total_ms,mean_us,max_us,main_thread_ms,caller_rva\n") !=
              std::string::npos);
        CHECK(text.find("\ntrigger,1,none,,,trigger,2,0.0060,3.000,5.000,0.0060,0xABC\n") != std::string::npos);
        CHECK(text.find("\ntrigger,2,event,test_events.1,,trigger,10,0.0030,0.300,0.900,0.0030,\n") != std::string::npos);
        CHECK(text.find("\neffect,1,event,test_events.1,,immediate,1,3.0000,3000.000,3000.000,3.0000,\n") !=
              std::string::npos);
        CHECK(sp::csv_name(2026, 11, 12, 8, 5, 9) == "script_profile_2026-11-12_080509.csv");

        const sp::Texts en{sp::Lang::en};
        sp::PanelState st;
        st.in_session = true;
        st.top = 2;
        st.csv_path = "logs\\x.csv";
        std::vector<sp::PanelRow> rows = sp::panel_rows(st, &merged, en);
        const auto has = [&](const std::string& label, const std::string& value = {}) {
            return std::any_of(rows.begin(), rows.end(), [&](const sp::PanelRow& p) {
                return p.label == label && (value.empty() || p.value == value);
            });
        };
        CHECK(has("Start") && has("Measure 30 days") && !has("Stop"));
        CHECK(has("Last measurement", "30 days (1719.3.18 to 1719.4.16), 0.0 s") && has("Saved to logs\\x.csv"));
        CHECK(has("Triggers", "11 \xC2\xB5s in 21 calls") && has("Effects", "3.00 ms in 1 calls"));
        CHECK(has("No owner found \xC2\xB7 trigger", "6.10 \xC2\xB5s, 3 calls, mean 2.03 \xC2\xB5s, max 5.00 \xC2\xB5s"));
        CHECK(has("Event test_events.1 \xC2\xB7 trigger") && !has("Event test_events.1 \xC2\xB7 mean_time_to_happen"));
        CHECK(has("Event test_events.1 \xC2\xB7 immediate"));
        st.measuring = true;
        st.multiplayer = true;
        st.days_so_far = 4;
        st.days_to_measure = 30;
        rows = sp::panel_rows(st, nullptr, en);
        CHECK(has("Stop") && !has("Start") && has("State", "Measuring: 4 of 30 days, 0.0 s"));
        CHECK(has("In multiplayer the measurement slows your game down, and every player waits for the slowest."));
        st = sp::PanelState{};
        rows = sp::panel_rows(st, nullptr, en);
        CHECK(has("Load a game to measure.") && has("State", "Not measuring: nothing in the game is hooked.") &&
              !has("Start"));
        CHECK(sp::format_ms(0.0123) == "12 \xC2\xB5s" && sp::format_ms(0.00123) == "1.23 \xC2\xB5s" &&
              sp::format_ms(1534.0) == "1.53 s" &&
              sp::format_count(1234567) == "1 234 567");
    });
}

void text_tests(const fs::path& source) {
    run("texts: every text of the panel has every language with the same placeholders", [&] {
        std::size_t count = 0;
        const sp::TextEntry* entries = sp::text_entries(count);
        std::set<std::string> table;
        for (std::size_t i = 0; i < count; ++i) {
            CHECK(table.insert(entries[i].english).second);
            for (const char* tr : entries[i].translated) {
                CHECK(tr != nullptr && *tr != '\0' && placeholders(tr) == placeholders(entries[i].english));
            }
        }
        std::set<std::string> used;
        for (const char* file : {"plugin.cpp", "report.cpp"}) {
            for (const std::string& t : used_texts(source / "plugins" / "anvil-script-profiler" / "src" / file)) {
                used.insert(t);
            }
        }
        for (const std::string& t : used) {
            if (!table.contains(t)) std::printf("  not in the table: %s\n", t.c_str());
            CHECK(table.contains(t));
        }
        for (const std::string& t : table) {
            if (!used.contains(t)) std::printf("  not used: %s\n", t.c_str());
            CHECK(used.contains(t));
        }
        const sp::Texts pl{sp::Lang::pl};
        CHECK(std::string(pl("Stop")) == "Stop" && pl.f("Measure {0} days", {"30"}) == "Zmierz 30 dni");
    });
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_script_profiler_tests <plugins folder> -userdir=<folder>\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path plugins_dir = fs::absolute(argv[1]);
    fs::path user;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("-userdir=", 0) == 0) user = fs::path(a.substr(9));
    }
    if (user.empty()) {
        std::printf("-userdir=<folder> is needed: the plugin writes its CSV files there\n");
        return 2;
    }
    std::error_code ignored;
    fs::remove_all(user / "logs", ignored);
    g_main_thread = GetCurrentThreadId();
    recorder_tests();
    owner_tests();
    text_tests(fs::path(ANVIL_SOURCE_DIR));

    // As in the game, the image and the host outlive the plugin.
    FakeEu4& image = *new FakeEu4;
    ChangeRegistry& changes = *new ChangeRegistry(image.address(0), FakeEu4::kSize);
    ServiceBus& bus = *new ServiceBus;
    std::vector<std::string>& log = *new std::vector<std::string>;
    HostContext& context = *new HostContext;
    context.changes = &changes;
    context.bus = &bus;
    context.image = GetModuleHandleW(nullptr);
    context.log = [&log](const AnvilPlugin& p, AnvilLogLevel, std::string_view message) {
        log.push_back(p.manifest.id + ": " + std::string(message));
    };
    AnvilGameInfo& info = *new AnvilGameInfo{};
    info.size = sizeof(info);
    info.build_id = kBuild;
    info.image_base = image.address(0);
    info.image_size = FakeEu4::kSize;
    const AnvilHost& host = *new AnvilHost(make_host(&info));
    AnvilGameService& game = *new AnvilGameService{};
    game.size = sizeof(game);
    game.main_thread_id = fake_main_thread_id;
    game.run_on_main_thread = fake_run_on_main_thread;
    CHECK(bus.register_service("anvil-game", ANVIL_GAME_SERVICE, ANVIL_GAME_SERVICE_VERSION, &game) == ANVIL_OK);
    AnvilPanelService& panels = *new AnvilPanelService{};
    panels.size = sizeof(panels);
    panels.create = fake_create;
    panels.set_rows = fake_set_rows;
    panels.on_click = fake_on_click;
    panels.show = fake_show;
    panels.shown = fake_shown;
    panels.language = fake_language;
    CHECK(bus.register_service("anvil-overlay", ANVIL_PANEL_SERVICE, ANVIL_PANEL_SERVICE_VERSION, &panels) == ANVIL_OK);
    const fs::path data = fs::temp_directory_path() / ("anvil-script-profiler-tests-" + std::to_string(GetCurrentProcessId()));
    AnvilPlugin* plugin = nullptr;

    run("profiler plugin: symbols verify, it loads idle with no change and a hidden panel", [&] {
        const auto dir = plugins_dir / "anvil-script-profiler";
        const auto parsed = load_manifest(dir);
        CHECK(parsed.manifest && parsed.manifest->phase == Phase::Main && !parsed.manifest->affects_gameplay);
        if (!parsed.manifest) return;
        SymbolVerification verified;
        SymbolFile declared;
        const std::string reason = anvil::i18n::english(check_plugin_symbols(dir / parsed.manifest->symbols, kBuild, changes,
                                                                          image.address(0), &verified, &declared));
        if (!reason.empty()) std::printf("  %s\n", reason.c_str());
        CHECK(reason.empty() && verified.symbols.size() == 9);
        if (!reason.empty()) return;
        auto* p = new AnvilPlugin;
        p->context = &context;
        p->manifest = *parsed.manifest;
        p->symbols = PluginSymbols(declared, verified, image.address(0));
        p->owner = changes.add_owner(p->manifest.id);
        p->plugin_dir = dir.wstring();
        p->data_dir = data.wstring();
        auto schema = load_settings(dir / p->manifest.settings);
        CHECK(schema.file.has_value());
        if (!schema.file) return;
        p->settings.init(std::move(*schema.file), data / "settings.toml", p->manifest.id,
                         [](const std::string&) -> std::optional<std::string> { return std::nullopt; });
        p->module = LoadLibraryExW((dir / p->manifest.entry).c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        CHECK(p->module != nullptr);
        if (p->module == nullptr) return;
        const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(p->module, "anvil_plugin_load"));
        const LoadOutcome outcome = run_plugin_load(*p, host, fn);
        CHECK(outcome.result == ANVIL_OK && outcome.staged == 0 && changes.count(p->owner) == 0);
        CHECK(g_title == "Script profiler" && g_shown == 0 && g_click != nullptr);
        CHECK(row_labelled("Load a game to measure.") != nullptr && button("Start") == 0);
        CHECK(contains(log, "anvil-script-profiler: idle: no hooks until a profiling span starts in the panel (7 owner "
                            "classes known)"));
        plugin = p;
    });
    if (plugin == nullptr) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }

    AnvilPlugin& game_plugin = *new AnvilPlugin;
    game_plugin.context = &context;
    game_plugin.manifest = manifest(plugin_toml("anvil-game"));
    const auto session = [&](std::uint32_t change, std::uint32_t multiplayer) {
        AnvilGameSession s{};
        s.size = sizeof(s);
        s.change = change;
        s.multiplayer = multiplayer;
        s.number = 1;
        CHECK(host.publish(&game_plugin, ANVIL_GAME_EVENT_SESSION, &s) == ANVIL_OK);
    };
    int day_of_year = 75;
    const auto day = [&] {
        AnvilGameDateChange d{};
        d.size = sizeof(d);
        d.days = 1;
        d.date.size = sizeof(d.date);
        d.date.year = 1719;
        ++day_of_year;
        d.date.month = static_cast<std::uint32_t>(1 + day_of_year / 31);
        d.date.day = static_cast<std::uint32_t>(1 + day_of_year % 31);
        CHECK(host.publish(&game_plugin, ANVIL_GAME_EVENT_DAY, &d) == ANVIL_OK);
    };
    const auto click = [&](const std::string& label) {
        const std::uint64_t id = button(label);
        CHECK(id != 0);
        if (id != 0) g_click(g_click_context, id);
    };
    const World& w = *new World(image.address(0x1CAB3A8), image.address(0x1C66920));
    // The event's trigger evaluates a nested AND; its immediate a limit, a root trigger of its own.
    g_children[w.trigger()] = w.loose + 16;
    g_limits[w.immediate()] = w.loose;
    const auto play = [&] {
        int scope = 0;
        for (int i = 0; i < 5; ++i) g_evaluate(w.trigger(), &scope);
        g_evaluate(w.potential(), &scope);
        g_execute(w.immediate(), &scope, 0);
        std::thread([&] {
            int s = 0;
            g_evaluate(w.potential(), &s);
        }).join();
    };
    const auto csv_files = [&] {
        std::vector<fs::path> out;
        for (const auto& e : fs::directory_iterator(user / "logs", ignored)) {
            if (e.path().extension() == ".csv") out.push_back(e.path());
        }
        std::sort(out.begin(), out.end());
        return out;
    };

    run("profiler plugin: Start hooks the gate and the effect blocks, Stop removes them and writes the CSV", [&] {
        session(ANVIL_GAME_SESSION_STARTED, 0);
        CHECK(button("Start") != 0 && button("Measure 30 days") != 0);
        play();  // before the span: nothing is hooked, nothing recorded
        CHECK(changes.count(plugin->owner) == 0);
        click("Start");
        CHECK(changes.count(plugin->owner) == 2 && button("Stop") != 0);
        CHECK(contains(log, "anvil-script-profiler: profiling span started: hooks on CAndTrigger::Evaluate and "
                            "CEffect::Execute"));
        g_evaluated = g_executed = 0;
        play();
        CHECK(g_evaluated == 13 && g_executed == 1);  // 5 x (trigger + nested), 2 potentials, the limit
        day();
        day();
        click("Stop");
        CHECK(changes.count(plugin->owner) == 0 && button("Start") != 0);
        CHECK(any_starts(log, "anvil-script-profiler: profiling span ended (complete): 2 days"));
        const auto files = csv_files();
        CHECK(files.size() == 1);
        if (files.size() != 1) return;
        const std::string text = read_text(files[0]);
        CHECK(text.find(" status=complete ") != std::string::npos && text.find(" days=2 ") != std::string::npos &&
              text.find(" first_day=1719.3.15 last_day=1719.3.16 ") != std::string::npos &&
              text.find(" threads=2 ") != std::string::npos && text.find(" nested_triggers=5 ") != std::string::npos);
        CHECK(text.find(",event,test_events.1,,trigger,5,") != std::string::npos);
        CHECK(text.find(",decision,my_decision_with_a_long_name,,potential,2,") != std::string::npos);
        CHECK(text.find("effect,1,event,test_events.1,,immediate,1,") != std::string::npos);
        CHECK(text.find(",none,,,trigger,1,") != std::string::npos);  // the limit inside the effect
        CHECK(row_labelled("Event test_events.1 \xC2\xB7 trigger") != nullptr &&
              row_labelled("Decision my_decision_with_a_long_name \xC2\xB7 potential") != nullptr);
        const PanelRowCopy* saved = row_labelled("Saved to logs");
        CHECK(saved != nullptr);
        g_evaluated = 0;
        play();
        CHECK(g_evaluated == 13);  // the game's code runs as before
    });

    run("profiler plugin: Measure N days stops by itself; the setting changes N", [&] {
        CHECK(plugin->settings.set_text(0, "3").applied);
        run_work();
        click("Measure 3 days");
        CHECK(changes.count(plugin->owner) == 2 && row_labelled("State") != nullptr &&
              row_labelled("State")->value.rfind("Measuring: 0 of 3 days", 0) == 0);
        play();
        day();
        day();
        CHECK(changes.count(plugin->owner) == 2);
        day();
        CHECK(changes.count(plugin->owner) == 0 && button("Measure 3 days") != 0);
        CHECK(csv_files().size() == 2);  // a second file, even within the same second
    });

    run("profiler plugin: a load or leaving the session interrupts the span; the CSV says so", [&] {
        Sleep(1100);  // the newest file sorts last
        click("Start");
        play();
        day();
        session(ANVIL_GAME_SESSION_ENDED, 0);
        CHECK(changes.count(plugin->owner) == 0 && row_labelled("Load a game to measure.") != nullptr);
        CHECK(row_labelled("Interrupted: the game was loaded or left.") != nullptr);
        CHECK(any_starts(log, "anvil-script-profiler: profiling span ended (interrupted: the session ended): 1 days"));
        const auto files = csv_files();
        CHECK(!files.empty() && read_text(files.back()).find(" status=interrupted_session ") != std::string::npos);
        session(ANVIL_GAME_SESSION_STARTED, 1);
        CHECK(row_labelled("In multiplayer the measurement slows your game down") != nullptr && button("Start") != 0);
    });

    if (g_failures) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
