// anvil-script (plugins/anvil-script): the key table on its own, and the
// plugin end to end. The DLL loads through the host table against a synthetic
// image with the declared bytes of symbols.toml at their Steam RVAs: the start
// of CEffect::ReadMember, the constructor and destructor, CArray::Append, the
// scope getters and CString's constructor run their declared bytes, undo them
// and jump to stand-ins; CEffect's vtable lies in .rdata. A stand-in of
// anvil.control answers who a plugin is.
// Usage: anvil_script_tests <folder with one folder per plugin>
#include "core/changes.hpp"
#include "core/host.hpp"
#include "core/manifest.hpp"
#include "core/plugin_symbols.hpp"
#include "core/services.hpp"
#include "core/symbol_check.hpp"
#include "test_support.hpp"

#include "../plugins/anvil-script/src/day_end.hpp"
#include "../plugins/anvil-script/src/registry.hpp"
#include "anvil_control.h"

#include <anvil/anvil_plugin.h>
#include <anvil/anvil_script.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

constexpr const char* kBuild = "eu4-1.37.5-steam-win64";

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// ---- the key table on its own ------------------------------------------------------

void noop_execute(void*, const AnvilScriptCall*) {}

void registry_tests() {
    run("script registry: a key is <owner>_<name>", [] {
        CHECK(anvil_script::valid_key("chisel", "chisel_set_terrain"));
        CHECK(anvil_script::valid_key("my-plugin", "my_plugin_x"));
        CHECK(anvil_script::valid_key("chisel", "chisel_2"));
        CHECK(!anvil_script::valid_key("chisel", "chisel_"));
        CHECK(!anvil_script::valid_key("chisel", "chisel"));
        CHECK(!anvil_script::valid_key("chisel", "chiselx_set"));
        CHECK(!anvil_script::valid_key("chisel", "add_treasury"));
        CHECK(!anvil_script::valid_key("chisel", "Chisel_set"));
        CHECK(!anvil_script::valid_key("my-plugin", "my-plugin_x"));
        CHECK(!anvil_script::valid_key("chisel", "chisel_set terrain"));
        CHECK(!anvil_script::valid_key("chisel", "chisel_" + std::string(57, 'a')));
        CHECK(anvil_script::valid_key("chisel", "chisel_" + std::string(56, 'a')));
    });

    run("script registry: the checks come in the spec's order", [] {
        anvil_script::Registry r;
        const auto* self = reinterpret_cast<const AnvilPlugin*>(0x1000);
        anvil_script::Caller gameplay{true, "chisel", true, true};
        AnvilScriptEffect e{sizeof(e), 0, "chisel_set_terrain", noop_execute, nullptr, nullptr};
        std::string why;
        CHECK(r.add({}, self, &e, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(r.add(gameplay, self, nullptr, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilScriptEffect small = e;
        small.size = offsetof(AnvilScriptEffect, execute);
        CHECK(r.add(gameplay, self, &small, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilScriptEffect bad_and_no_execute = e;
        bad_and_no_execute.execute = nullptr;
        CHECK(r.add(gameplay, self, &bad_and_no_execute, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilScriptEffect bad_key = e;
        bad_key.key = "add_treasury";
        anvil_script::Caller reader{true, "chisel", false, true};
        CHECK(r.add(reader, self, &bad_key, why) == ANVIL_ERROR_WRONG_KIND);
        anvil_script::Caller main{true, "chisel", true, false};
        CHECK(r.add(main, self, &bad_key, why) == ANVIL_ERROR_WRONG_PHASE);
        CHECK(r.add(gameplay, self, &bad_key, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        AnvilScriptEffect flagged = e;
        flagged.flags = 1;
        CHECK(r.add(gameplay, self, &flagged, why) == ANVIL_ERROR_UNSUPPORTED);
        CHECK(r.add(gameplay, self, &e, why) == ANVIL_OK && r.size() == 1);
        CHECK(r.add(gameplay, self, &e, why) == ANVIL_ERROR_ALREADY_EXISTS);
        CHECK(r.find("chisel_set_terrain") == 0 && r.find("CHISEL_Set_Terrain") == 0);
        CHECK(r.find("chisel_set_terrain2") == -1 && r.find("chisel_set_terrai") == -1 && r.find("add_treasury") == -1);
        r.close();
        AnvilScriptEffect late = e;
        late.key = "chisel_reset_terrain";
        CHECK(r.add(gameplay, self, &late, why) == ANVIL_ERROR_WRONG_PHASE && r.size() == 1);
    });

    run("script registry: describe and context only when the size covers them", [] {
        anvil_script::Registry r;
        const auto* self = reinterpret_cast<const AnvilPlugin*>(0x1000);
        int context = 0;
        AnvilScriptEffect e{offsetof(AnvilScriptEffect, describe), 0, "chisel_a", noop_execute,
                            reinterpret_cast<AnvilScriptDescribe>(&noop_execute), &context};
        std::string why;
        CHECK(r.add({true, "chisel", true, true}, self, &e, why) == ANVIL_OK);
        CHECK(r.entry(0).describe == nullptr && r.entry(0).context == nullptr && r.entry(0).owner == self);
    });

    run("script registry: at most 256 keys", [] {
        anvil_script::Registry r;
        const auto* self = reinterpret_cast<const AnvilPlugin*>(0x1000);
        std::string why;
        std::vector<std::string> keys;
        for (int i = 0; i <= 256; ++i) keys.push_back("chisel_k" + std::to_string(i));
        AnvilResult last = ANVIL_OK;
        for (const auto& k : keys) {
            AnvilScriptEffect e{sizeof(e), 0, k.c_str(), noop_execute, nullptr, nullptr};
            last = r.add({true, "chisel", true, true}, self, &e, why);
        }
        CHECK(last == ANVIL_ERROR_OUT_OF_MEMORY && r.size() == 256 && r.find("chisel_k255") == 255);
    });
}

// ---- the Day end on its own ---------------------------------------------------------

// The game's hours for a date: 365-day years from 1 January -5000.
std::int32_t hours_of(int year, int month, int day) {
    static const int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int day_of_year = day - 1;
    for (int m = 1; m < month; ++m) day_of_year += kMonthDays[m - 1];
    return static_cast<std::int32_t>((static_cast<std::int64_t>(year) * 365 + day_of_year) * 24 + 43'800'000);
}

void noop_day(void*, const AnvilScriptDay*) {}

void day_end_tests() {
    run("day end: the date and the flags of a month and a year", [] {
        AnvilScriptDay d = anvil_script::decode_day(hours_of(1445, 1, 1));
        CHECK(d.size == sizeof(AnvilScriptDay) && d.year == 1445 && d.month == 1 && d.day == 1 &&
              d.flags == (ANVIL_SCRIPT_DAY_NEW_MONTH | ANVIL_SCRIPT_DAY_NEW_YEAR));
        d = anvil_script::decode_day(hours_of(1719, 3, 17));
        CHECK(d.year == 1719 && d.month == 3 && d.day == 17 && d.flags == 0);
        d = anvil_script::decode_day(hours_of(1444, 2, 1));
        CHECK(d.month == 2 && d.day == 1 && d.flags == ANVIL_SCRIPT_DAY_NEW_MONTH);
        d = anvil_script::decode_day(hours_of(1444, 12, 31));
        CHECK(d.month == 12 && d.day == 31 && d.flags == 0);
        // The game's "unset" date (43808760) still decodes: 1 January of year 1.
        d = anvil_script::decode_day(43'808'760);
        CHECK(d.year == 1 && d.month == 1 && d.day == 1);
    });

    run("day end: the checks come in the spec's order, and the list closes", [] {
        anvil_script::DayEndList l;
        const auto* self = reinterpret_cast<const AnvilPlugin*>(0x1000);
        std::string why;
        CHECK(l.add(false, true, self, noop_day, nullptr, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(l.add(true, true, nullptr, noop_day, nullptr, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(l.add(true, false, self, nullptr, nullptr, why) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(l.add(true, false, self, noop_day, nullptr, why) == ANVIL_ERROR_WRONG_KIND);
        int context = 0;
        CHECK(l.add(true, true, self, noop_day, &context, why) == ANVIL_OK && l.size() == 1);
        CHECK(l.entry(0).owner == self && l.entry(0).context == &context);
        l.close();
        CHECK(l.add(true, true, self, noop_day, nullptr, why) == ANVIL_ERROR_WRONG_PHASE && l.size() == 1);
        CHECK(l.add(true, false, self, noop_day, nullptr, why) == ANVIL_ERROR_WRONG_KIND);
    });

    run("day end: at most 64 subscriptions", [] {
        anvil_script::DayEndList l;
        const auto* self = reinterpret_cast<const AnvilPlugin*>(0x1000);
        std::string why;
        AnvilResult last = ANVIL_OK;
        for (int i = 0; i <= 64; ++i) last = l.add(true, true, self, noop_day, nullptr, why);
        CHECK(last == ANVIL_ERROR_OUT_OF_MEMORY && l.size() == 64);
    });
}

// ---- the plugin end to end ------------------------------------------------------

// An MSVC std::string as eu4.exe lays it out.
struct GameString {
    union {
        char buffer[16];
        char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;

    std::string text() const { return std::string(capacity > 15 ? pointer : buffer, size); }
};
static_assert(sizeof(GameString) == 32);

// CToken: the lexer's id, then the text.
struct Token {
    int id = 0;
    char text[0x204] = {};
};
// CReader as far as the plugin reads it: _Toklhs at +0x30, _Tokrhs at +0x440.
struct Reader {
    std::uint8_t head[0x30] = {};
    Token lhs;
    std::uint8_t op[0x208] = {};
    Token rhs;
};
static_assert(offsetof(Reader, lhs) == 0x30 && offsetof(Reader, rhs) == 0x440);

// CEffect: vtable, the children vector at +0x10, the token at +0x28.
struct FakeEffect {
    const void* vtable = nullptr;
    std::uint8_t type[8] = {};
    void** begin = nullptr;
    void** end = nullptr;
    void** cap = nullptr;
    int token = -1;
    std::uint8_t rest[0xC] = {};
};
static_assert(sizeof(FakeEffect) == 0x38 && offsetof(FakeEffect, begin) == 0x10 && offsetof(FakeEffect, token) == 0x28);

// CEventScope as the plugin reads it: the province at +0x28, the country's
// tag at +0x2C and an override tag at +0x44 (CCountryTag, valid at +7).
struct FakeScope {
    std::uint8_t head[0x28] = {};
    int province = 0;
    char tag[8] = {};
    std::uint8_t mid[0x10] = {};
    char override_tag[8] = {};
    FakeScope(int p, const char* t, const char* o = nullptr) : province(p) {
        if (t) {
            std::memcpy(tag, t, 3);
            tag[7] = 1;
        }
        if (o) {
            std::memcpy(override_tag, o, 3);
            override_tag[7] = 1;
        }
    }
};
static_assert(offsetof(FakeScope, province) == 0x28 && offsetof(FakeScope, tag) == 0x2C &&
              offsetof(FakeScope, override_tag) == 0x44);

// What the stand-ins of the game got.
std::vector<std::string> g_read_keys;       // ReadMember reached the game
int g_ctors = 0, g_dtors = 0, g_grows = 0, g_base_descs = 0;
std::vector<std::string> g_strings;         // CString::CString(const char*)
std::uintptr_t g_vtable_address = 0;

void fake_read_member(void*, void* reader, int) {
    g_read_keys.push_back(static_cast<Reader*>(reader)->lhs.text);
}
void* fake_ctor(void* effect) {
    ++g_ctors;
    auto* e = static_cast<FakeEffect*>(effect);
    e->vtable = reinterpret_cast<const void*>(g_vtable_address);
    e->token = -1;
    return effect;
}
void fake_dtor(void*) { ++g_dtors; }
void fake_grow(void*, void**) { ++g_grows; }
void* fake_cstring(void* string, const char* text) {
    g_strings.push_back(text);
    auto* s = static_cast<GameString*>(string);
    const std::size_t n = std::strlen(text);
    s->size = n;
    if (n < 16) {
        std::memcpy(s->buffer, text, n + 1);
        s->capacity = 15;
    } else {
        s->pointer = static_cast<char*>(std::malloc(n + 1));
        std::memcpy(s->pointer, text, n + 1);
        s->capacity = n;
    }
    return string;
}
void* fake_base_get_desc(void*, void* out, void*, void* prefix, int, bool) {
    ++g_base_descs;
    static_cast<GameString*>(prefix)->size = 0xDEAD;  // freed by the callee
    auto* s = static_cast<GameString*>(out);
    std::memset(s, 0, sizeof(*s));
    s->capacity = 15;
    return out;
}
void fake_slot() {}

// CGameState as far as the Day end reads it: the date at +0x1DD0.
struct FakeGameState {
    std::uint8_t head[0x1DD0] = {};
    std::int32_t date = 0;
};
static_assert(offsetof(FakeGameState, date) == 0x1DD0);
int g_days_run = 0;
std::function<void()> g_inside_day;  // runs inside the game's day
void fake_daily_tick(void* game_state) {
    ++g_days_run;
    static_cast<FakeGameState*>(game_state)->date += 24;
    if (g_inside_day) g_inside_day();
}

// eu4.exe's size and sections (Steam), reserved; pages are committed only
// where the symbols of anvil-script lie.
struct FakeEu4 {
    static constexpr uint32_t kSize = 0x25E0000;
    static constexpr uint32_t kVtable = 0x1C9E548;
    uint8_t* base = nullptr;

    FakeEu4() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
        commit(0x0, PAGE_READWRITE);
        for (uint32_t page :
             {0xC9000u, 0x5DE000u, 0x5E0000u, 0x5E4000u, 0x5ED000u, 0x5EF000u, 0x623000u, 0x758000u, 0x1703000u})
            commit(page, PAGE_EXECUTE_READWRITE);
        commit(0x1C9E000, PAGE_READWRITE);
        write_headers();
        // ReadMember: mov [rsp+8],rbx; mov [rsp+10h],rsi; push rbp; push rdi; push r12; push r14; push r15,
        // then pop them and jump to the stand-in.
        put(0x5DEC50, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56,
                       0x41, 0x57, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5C, 0x5F, 0x5D});
        jump(0x5DEC50 + 26, &fake_read_member);
        // The constructor: its declared bytes, add rsp,40h; pop rdi; mov rbx,[rsp+20h].
        put(0x5DE770, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48,
                       0x8B, 0xD9, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x40, 0x5F, 0x48, 0x8B, 0x5C, 0x24, 0x20});
        // The vtable's site lies inside the constructor (+0x27), so a short jmp rel32 (to 0x5DE7A1) gets
        // over it to the long jump.
        put(0x5DE770 + 30, {0xE9, 0x0E, 0x00, 0x00, 0x00});
        jump(0x5DE7A1, &fake_ctor);
        // lea rax,[CEffect::vftable]; mov [rcx],rax
        put(0x5DE797, {0x48, 0x8D, 0x05, 0xAA, 0xFD, 0x6B, 0x01, 0x48, 0x89, 0x01});
        // The destructor: its declared bytes, add rsp,40h; pop r14.
        put(0x5DE8D0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41,
                       0x56, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0xF1, 0x48, 0x83, 0xC4, 0x40, 0x41, 0x5E});
        jump(0x5DE8D0 + 30, &fake_dtor);
        // CArray::Append runs as it is; the growth path goes to a stand-in.
        put(0xC9F10, {0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0x51, 0x08, 0x48, 0x3B, 0x51, 0x10, 0x74, 0x0C, 0x49, 0x8B, 0x00,
                      0x48, 0x89, 0x02, 0x48, 0x83, 0x41, 0x08, 0x08, 0xC3});
        jump(0xC9F10 + 0x19, &fake_grow);
        // Sites with values (not run).
        put(0x5E4B04, {0x48, 0x8D, 0x5E, 0x30, 0x48, 0x8B, 0xCB});
        put(0x5E0F43, {0x48, 0x8D, 0x4F, 0x10, 0x48, 0x8D, 0x54, 0x24, 0x20, 0xE8, 0xBF, 0x8F, 0xAE, 0xFF, 0x48, 0x8B,
                       0x4C, 0x24, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8D, 0x96, 0x40, 0x04, 0x00, 0x00, 0xFF, 0x50, 0x48});
        put(0x5EF140, {0x89, 0x51, 0x28, 0xC3});
        // Where the game's effects read the scope (not run): a province and a country effect.
        put(0x62367F, {0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0x83, 0x7A, 0x28, 0x00});
        put(0x5EFC55, {0x44, 0x0F, 0xB6, 0x42, 0x4B, 0xBA, 0x44, 0x00, 0x00, 0x00, 0x8B, 0xC2, 0x41, 0xB9, 0x2C, 0x00,
                       0x00, 0x00, 0x45, 0x84, 0xC0, 0x41, 0x0F, 0x44, 0xC1});
        // CString::CString(const char*): its declared bytes, add rsp,20h; pop rbx.
        put(0x1703550, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC0, 0x48, 0xC7, 0x41, 0x18, 0x0F, 0x00, 0x00, 0x00,
                        0x48, 0x89, 0x01, 0x48, 0x8B, 0xD9, 0x88, 0x01, 0x48, 0x83, 0xC4, 0x20, 0x5B});
        jump(0x1703550 + 29, &fake_cstring);
        // HandleDailyTick: its declared bytes, add rsp,1F8h and the eight pops, then the stand-in.
        put(0x758430, {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                       0x48, 0x8D, 0xA8, 0xC8, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF8, 0x01, 0x00, 0x00, 0x48,
                       0x81, 0xC4, 0xF8, 0x01, 0x00, 0x00, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F,
                       0x5E, 0x5B, 0x5D});
        jump(0x758430 + 48, &fake_daily_tick);
        // The date's site inside it (not run): lea r15,[rsi+1DD0h] ... add dword [r15],18h.
        put(0x7584C1, {0x4C, 0x8D, 0xBE, 0xD0, 0x1D, 0x00, 0x00, 0x49, 0x8B, 0xCF, 0xE8, 0x40, 0x79, 0x97, 0xFF,
                       0x4C, 0x8D, 0x25, 0xF1, 0x20, 0x4E, 0x01, 0x83, 0xE8, 0x1F, 0x78, 0x0F, 0x49, 0x8B, 0xCC,
                       0x90, 0x41, 0xFF, 0xC5, 0x48, 0x8D, 0x49, 0x04, 0x2B, 0x01, 0x79, 0xF5, 0x49, 0x8B, 0xCF,
                       0xE8, 0x7D, 0x78, 0x97, 0xFF, 0x89, 0x85, 0x40, 0x01, 0x00, 0x00, 0x41, 0x83, 0x07, 0x18});
        // Code for the slots the plugin keeps: the base GetDesc and an empty function.
        jump(0x5ED100, &fake_base_get_desc);
        jump(0x5ED200, &fake_slot);
        for (int i = 0; i < 19; ++i) put_pointer(kVtable + 8 * i, address(0x5ED200));
        put_pointer(kVtable + 8 * 4, address(0x5DEC50));
        put_pointer(kVtable + 8 * 13, address(0x5ED100));
        g_vtable_address = address(kVtable);
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

// The stand-in of anvil.control: who each plugin handle is.
struct FakePlugin {
    std::string id;
    bool affects_gameplay = true;
    bool early = true;
    bool suspended = false;
};
std::map<const AnvilPlugin*, FakePlugin> g_plugins;

int control_plugin_of(const AnvilPlugin* plugin, AnvilControlPlugin* out) {
    const auto it = g_plugins.find(plugin);
    if (it == g_plugins.end() || out == nullptr) return 0;
    AnvilControlPlugin full{};
    full.size = sizeof(full);
    full.id = it->second.id.c_str();
    full.affects_gameplay = it->second.affects_gameplay ? 1 : 0;
    full.early = it->second.early ? 1 : 0;
    full.suspended = it->second.suspended ? 1 : 0;
    std::memcpy(out, &full, std::min<std::size_t>(out->size, sizeof(full)));
    return 1;
}

// A parent effect with room for its children, so Append never grows.
struct Parent {
    FakeEffect effect;
    void* storage[16] = {};
    Parent() {
        effect.begin = effect.end = storage;
        effect.cap = storage + 16;
    }
    std::size_t children() const { return static_cast<std::size_t>(effect.end - effect.begin); }
    FakeEffect* child(std::size_t i) const { return static_cast<FakeEffect*>(effect.begin[i]); }
};

using ReadMemberFn = void(void* effect, void* reader, int token);
using ExecuteFn = void(void* effect, void* scope, int value);
using GetDescFn = void*(void* effect, void* out, void* scope, void* prefix, int indent, bool flag);
using DeletingDtorFn = void*(void* effect, unsigned flags);

struct Recorded {
    std::string key, value, country;
    int province = 0;
};
std::vector<Recorded> g_executed;

struct DayCalled {
    std::string who;
    int year, month, day;
    std::uint32_t flags;
};
std::string g_line = "Mountains ahead";

void consumer_execute(void* context, const AnvilScriptCall* call) {
    CHECK(context == &g_executed && call->size == sizeof(AnvilScriptCall));
    g_executed.push_back({call->key, call->value, call->scope.country, call->scope.province});
}
uint32_t consumer_describe(void*, const AnvilScriptCall* call, char* utf8, uint32_t capacity) {
    CHECK(std::string(call->value) == "mountains" && call->scope.province == 295);
    std::snprintf(utf8, capacity, "%s", g_line.c_str());
    return static_cast<uint32_t>(g_line.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_script_tests <plugins folder>\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash in the game's stand-ins keeps what came before
    const fs::path plugins_dir = fs::absolute(argv[1]);
    registry_tests();
    day_end_tests();

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
    AnvilControlService& control = *new AnvilControlService{};
    control.size = sizeof(control);
    control.plugin_of = control_plugin_of;
    CHECK(bus.register_service("anvil", ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION, &control) == ANVIL_OK);
    AnvilPlugin* plugin = nullptr;

    run("script plugin: symbols verify, it hooks ReadMember and HandleDailyTick and registers its services", [&] {
        const auto dir = plugins_dir / "anvil-script";
        const auto parsed = load_manifest(dir);
        CHECK(parsed.manifest && parsed.manifest->phase == Phase::Early && !parsed.manifest->affects_gameplay);
        if (!parsed.manifest) return;
        SymbolVerification verified;
        SymbolFile declared;
        const std::string reason = anvil::i18n::english(check_plugin_symbols(dir / parsed.manifest->symbols, kBuild, changes,
                                                                          image.address(0), &verified, &declared));
        if (!reason.empty()) std::printf("  %s\n", reason.c_str());
        CHECK(reason.empty() && verified.symbols.size() == 13);
        if (!reason.empty()) return;
        auto* p = new AnvilPlugin;
        p->context = &context;
        p->manifest = *parsed.manifest;
        p->symbols = PluginSymbols(declared, verified, image.address(0));
        p->owner = changes.add_owner(p->manifest.id);
        p->plugin_dir = dir.wstring();
        p->module = LoadLibraryExW((dir / p->manifest.entry).c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        CHECK(p->module != nullptr);
        if (p->module == nullptr) return;
        const auto fn = reinterpret_cast<PluginLoadFn>(GetProcAddress(p->module, "anvil_plugin_load"));
        const LoadOutcome outcome = run_plugin_load(*p, host, fn);
        CHECK(outcome.result == ANVIL_OK && outcome.staged == 2 && changes.count(p->owner) == 2);
        plugin = p;
    });
    if (plugin == nullptr) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }
    const auto* service = static_cast<const AnvilScriptEffectsService*>(
        bus.get_service(ANVIL_SCRIPT_EFFECTS_SERVICE, ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION));
    CHECK(service != nullptr && service->size == sizeof(AnvilScriptEffectsService));
    if (service == nullptr) return 1;

    auto* chisel = reinterpret_cast<AnvilPlugin*>(0x10);
    auto* reader_only = reinterpret_cast<AnvilPlugin*>(0x20);
    auto* late = reinterpret_cast<AnvilPlugin*>(0x30);
    g_plugins[chisel] = {"chisel", true, true, false};
    g_plugins[reader_only] = {"stats", false, true, false};
    g_plugins[late] = {"late", true, false, false};
    auto* read_member = reinterpret_cast<ReadMemberFn*>(image.address(0x5DEC50));

    run("script plugin: only an early gameplay plugin registers, with its own prefix", [&] {
        const AnvilScriptEffect set{sizeof(AnvilScriptEffect), 0, "chisel_set_terrain", consumer_execute, consumer_describe,
                                    &g_executed};
        const AnvilScriptEffect reset{sizeof(AnvilScriptEffect), 0, "chisel_reset_terrain", consumer_execute, nullptr,
                                      &g_executed};
        CHECK(service->register_effect(chisel, &set) == ANVIL_OK);
        CHECK(service->register_effect(chisel, &reset) == ANVIL_OK);
        CHECK(service->register_effect(chisel, &set) == ANVIL_ERROR_ALREADY_EXISTS);
        const AnvilScriptEffect stats{sizeof(AnvilScriptEffect), 0, "stats_x", consumer_execute, nullptr, nullptr};
        CHECK(service->register_effect(reader_only, &stats) == ANVIL_ERROR_WRONG_KIND);
        const AnvilScriptEffect main{sizeof(AnvilScriptEffect), 0, "late_x", consumer_execute, nullptr, nullptr};
        CHECK(service->register_effect(late, &main) == ANVIL_ERROR_WRONG_PHASE);
        CHECK(service->register_effect(reinterpret_cast<AnvilPlugin*>(0x40), &set) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(contains(log, "anvil-script: chisel registered chisel_set_terrain"));
        CHECK(contains(log, "anvil-script: stats: stats_x refused: the plugin has affects_gameplay = false; only gameplay "
                            "plugins add script effects"));
    });

    Reader& reader = *new Reader;
    const auto member = [&](Parent& parent, const char* key, const char* value, int token = 0xF) {
        std::snprintf(reader.lhs.text, sizeof reader.lhs.text, "%s", key);
        std::snprintf(reader.rhs.text, sizeof reader.rhs.text, "%s", value);
        reader.lhs.id = token;
        read_member(&parent.effect, &reader, token);
    };

    run("script plugin: the game's own keys go to the game", [&] {
        Parent parent;
        g_read_keys.clear();
        member(parent, "add_treasury", "50", 0x92);
        member(parent, "chisel_other", "x");
        CHECK((g_read_keys == std::vector<std::string>{"add_treasury", "chisel_other"}) && parent.children() == 0);
    });

    Parent& parent = *new Parent;
    run("script plugin: a registered key becomes an effect of its own in the parent", [&] {
        g_read_keys.clear();
        g_ctors = 0;
        member(parent, "Chisel_Set_Terrain", "mountains");
        CHECK(g_read_keys.empty() && g_ctors == 1 && parent.children() == 1);
        if (parent.children() != 1) return;
        FakeEffect* e = parent.child(0);
        CHECK(e->token == 0xF && e->vtable != reinterpret_cast<const void*>(g_vtable_address));
        auto* const* slots = static_cast<void* const*>(e->vtable);
        auto* const* game = reinterpret_cast<void* const*>(g_vtable_address);
        CHECK(slots[4] == game[4] && slots[1] == game[1] && slots[0] != game[0] && slots[15] != game[15]);
    });

    run("script plugin: registration ends once the game parses", [&] {
        const AnvilScriptEffect after{sizeof(AnvilScriptEffect), 0, "chisel_after", consumer_execute, nullptr, nullptr};
        CHECK(service->register_effect(chisel, &after) == ANVIL_ERROR_WRONG_PHASE);
    });

    run("script plugin: a block, a long value and a word of the game are left to the game, logged once", [&] {
        Parent other;
        g_read_keys.clear();
        member(other, "chisel_set_terrain", "{");
        member(other, "chisel_set_terrain", "{");
        member(other, "chisel_set_terrain", std::string(256, 'a').c_str());
        member(other, "chisel_reset_terrain", "yes", 0x1234);
        CHECK(g_read_keys.size() == 4 && other.children() == 0);
        const auto lines = std::count_if(log.begin(), log.end(), [](const std::string& l) {
            return l.rfind("anvil-script: chisel_set_terrain: a block value", 0) == 0;
        });
        CHECK(lines == 1);
        CHECK(contains(log, "anvil-script: chisel_reset_terrain: the key is a word of the game or of its data, so the "
                            "plugin's effect is not used; the game reads it as its own (an unknown effect is skipped "
                            "with a line in error.log)"));
        member(other, "chisel_set_terrain", std::string(255, 'a').c_str());
        CHECK(other.children() == 1);
    });

    if (parent.children() != 1) return 1;
    FakeEffect* effect = parent.child(0);
    auto* const* slots = static_cast<void* const*>(effect->vtable);

    run("script plugin: Execute calls the plugin with the value and the scope", [&] {
        FakeScope scope{295, "RUS"};
        g_executed.clear();
        reinterpret_cast<ExecuteFn*>(slots[15])(effect, &scope, 0);
        CHECK(g_executed.size() == 1);
        if (g_executed.size() != 1) return;
        CHECK(g_executed[0].key == "chisel_set_terrain" && g_executed[0].value == "mountains" &&
              g_executed[0].province == 295 && g_executed[0].country == "RUS");
        FakeScope country{0, "SWE"};
        reinterpret_cast<ExecuteFn*>(slots[15])(effect, &country, 0);
        CHECK(g_executed.size() == 2 && g_executed[1].province == 0 && g_executed[1].country == "SWE");
        FakeScope overridden{33, "RUS", "TUR"};
        reinterpret_cast<ExecuteFn*>(slots[15])(effect, &overridden, 0);
        CHECK(g_executed.size() == 3 && g_executed[2].province == 33 && g_executed[2].country == "TUR");
        FakeScope no_country{33, nullptr};
        reinterpret_cast<ExecuteFn*>(slots[15])(effect, &no_country, 0);
        CHECK(g_executed.size() == 4 && g_executed[3].country.empty());
    });

    run("script plugin: a suspended owner's effect does nothing", [&] {
        FakeScope scope{295, "RUS"};
        g_executed.clear();
        g_plugins[chisel].suspended = true;
        reinterpret_cast<ExecuteFn*>(slots[15])(effect, &scope, 0);
        CHECK(g_executed.empty());
        g_plugins[chisel].suspended = false;
    });

    run("script plugin: GetDesc frees the prefix through the base and builds the game's string", [&] {
        FakeScope scope{295, "RUS"};
        GameString prefix{};
        GameString out{};
        g_strings.clear();
        g_base_descs = 0;
        reinterpret_cast<GetDescFn*>(slots[13])(effect, &out, &scope, &prefix, 0, false);
        CHECK(g_base_descs == 1 && prefix.size == 0xDEAD && out.text() == "Mountains ahead\n" &&
              (g_strings == std::vector<std::string>{"Mountains ahead\n"}));
        // Without Mei's UTF-8: Windows-1252, literal as in anvil-map ('$' stays, the
        // colour sign becomes '?'), cut at a bad byte, one line; a line break ends it.
        g_line = "Teren: g\xC3\xB3ry \xC2\xA7Y$x$ \xE4\xB8\x80";
        g_strings.clear();
        reinterpret_cast<GetDescFn*>(slots[13])(effect, &out, &scope, &prefix, 0, false);
        CHECK((g_strings == std::vector<std::string>{"Teren: g\xF3ry ?Y$x$ ?\n"}));
        g_line = "two\nlines\r and a bad byte \xFF here";
        g_strings.clear();
        reinterpret_cast<GetDescFn*>(slots[13])(effect, &out, &scope, &prefix, 0, false);
        CHECK((g_strings == std::vector<std::string>{"twolines and a bad byte \n"}));
        g_line = "Mountains ahead";
    });

    run("script plugin: an effect without describe has the base's empty line", [&] {
        Parent p;
        member(p, "chisel_reset_terrain", "yes");
        if (p.children() != 1) {
            CHECK(false);
            return;
        }
        FakeScope scope{295, "RUS"};
        GameString prefix{}, out{};
        g_strings.clear();
        auto* const* s = static_cast<void* const*>(p.child(0)->vtable);
        reinterpret_cast<GetDescFn*>(s[13])(p.child(0), &out, &scope, &prefix, 0, false);
        CHECK(g_strings.empty() && out.size == 0);
    });

    run("script plugin: the game deletes it through slot 0", [&] {
        g_dtors = 0;
        reinterpret_cast<DeletingDtorFn*>(slots[0])(effect, 1);
        CHECK(g_dtors == 1);
    });

    const auto* day_end = static_cast<const AnvilScriptDayEndService*>(
        bus.get_service(ANVIL_SCRIPT_DAY_END_SERVICE, ANVIL_SCRIPT_DAY_END_SERVICE_VERSION));
    CHECK(day_end != nullptr && day_end->size == sizeof(AnvilScriptDayEndService));
    if (day_end == nullptr) return 1;
    auto* daily_tick = reinterpret_cast<void (*)(void*)>(image.address(0x758430));
    FakeGameState& game_state = *new FakeGameState;
    static std::vector<DayCalled> called;
    const auto record = [](void* context, const AnvilScriptDay* d) {
        CHECK(d->size == sizeof(AnvilScriptDay));
        called.push_back({static_cast<const char*>(context), d->year, d->month, d->day, d->flags});
    };
    static char chisel_name[] = "chisel", late_name[] = "late", chisel2_name[] = "chisel2";

    run("day end: gameplay plugins of any phase subscribe, in order", [&] {
        CHECK(day_end->subscribe(reader_only, record, chisel_name) == ANVIL_ERROR_WRONG_KIND);
        CHECK(day_end->subscribe(reinterpret_cast<AnvilPlugin*>(0x40), record, chisel_name) ==
              ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(day_end->subscribe(chisel, nullptr, chisel_name) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(day_end->subscribe(chisel, record, chisel_name) == ANVIL_OK);
        CHECK(day_end->subscribe(late, record, late_name) == ANVIL_OK);  // a main plugin
        CHECK(day_end->subscribe(chisel, record, chisel2_name) == ANVIL_OK);
        CHECK(contains(log, "anvil-script: chisel subscribed to the day end"));
        CHECK(contains(log, "anvil-script: stats: day end refused: the plugin has affects_gameplay = false; only "
                            "gameplay plugins run at the day end (anvil-game.day is for the others)"));
    });

    run("day end: after the game's day, once, with the date it moved to and the flags", [&] {
        game_state.date = hours_of(1444, 12, 31);
        called.clear();
        g_days_run = 0;
        AnvilResult inside = ANVIL_OK;
        g_inside_day = [&] {
            inside = day_end->subscribe(chisel, record, chisel_name);
            CHECK(called.empty());  // the subscribers come after the game's day
        };
        daily_tick(&game_state);
        g_inside_day = nullptr;
        CHECK(g_days_run == 1 && inside == ANVIL_ERROR_WRONG_PHASE);
        CHECK(called.size() == 3);
        if (called.size() != 3) return;
        CHECK(called[0].who == "chisel" && called[1].who == "late" && called[2].who == "chisel2");
        CHECK(called[0].year == 1445 && called[0].month == 1 && called[0].day == 1 &&
              called[0].flags == (ANVIL_SCRIPT_DAY_NEW_MONTH | ANVIL_SCRIPT_DAY_NEW_YEAR));
    });

    run("day end: an ordinary day, a month's first day, a suspended owner skipped", [&] {
        called.clear();
        daily_tick(&game_state);
        CHECK(called.size() == 3 && called[0].day == 2 && called[0].flags == 0);
        game_state.date = hours_of(1445, 1, 31);
        g_plugins[late].suspended = true;
        called.clear();
        daily_tick(&game_state);
        g_plugins[late].suspended = false;
        CHECK(called.size() == 2 && called[0].who == "chisel" && called[1].who == "chisel2");
        if (called.size() == 2) {
            CHECK(called[0].month == 2 && called[0].day == 1 && called[0].flags == ANVIL_SCRIPT_DAY_NEW_MONTH);
        }
    });

    run("day end: no subscription once a day has ended", [&] {
        CHECK(day_end->subscribe(chisel, record, chisel_name) == ANVIL_ERROR_WRONG_PHASE);
        CHECK(contains(log, "anvil-script: chisel: day end refused: the game has already ended a day; subscribe in "
                            "the plugin's load"));
    });

    if (g_failures) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
