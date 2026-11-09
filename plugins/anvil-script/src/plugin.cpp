// anvil-script: the standard plugin that lets a gameplay plugin add effects of
// its own to EU4's script language (docs/adr/0008-plugin-script-effects.md,
// docs/spec/script-effects-v1.md).
//
// The game has no table of effect names: CEffect::ReadMember is a switch over
// the lexer's tokens through which every container effect parses its members.
// A function hook on its start compares the member's key with the registered
// keys; for one of them it builds a CEffect of its own (the game's
// constructor, a copy of the game's vtable with five slots replaced, the
// parent's Append, the token), as the game builds its simple effects. Every
// other member goes on to the game.
//
// The parse runs on whichever threads the game parses on; the key table is
// complete and read-only by then (Registry::close on the first call).
#include <anvil/anvil.hpp>
#include <anvil/anvil_script.h>

#define NOMINMAX
#include <windows.h>

#include "anvil_control.h"
#include "registry.hpp"
#include "symbols.gen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace {

using namespace anvil_script;

constexpr std::size_t kEffectSize = 0x38;  // CEffect
constexpr std::size_t kSlots = 19;         // CEffect::vftable, 0x98 bytes
constexpr std::size_t kSlotDeletingDtor = 0;
constexpr std::size_t kSlotReadMember = 4;
constexpr std::size_t kSlotGetDesc = 13;   // CString GetDesc(CEventScope&, CString, int, bool)
constexpr std::size_t kSlotExecute = 15;   // void Execute(CEventScope&, int)
constexpr std::size_t kTokenText = 4;      // CToken: lexer id, then the text
constexpr std::size_t kMaxValue = 255;
constexpr std::size_t kMaxLine = 511;
// The token the parser passes for a name the lexer does not know, which is
// what a key of a plugin is (0xF, seen in the game: eu4-engine-lab#77; the
// lexer's FindTok answers 0xC, but that is not what reaches ReadMember). The
// game's keywords start at 0xB and its data words are dynamic ids above them.
constexpr int kUnknownToken = 0xF;

// Our CEffect: the game's object, then what we keep.
struct Effect {
    std::uint8_t base[kEffectSize];
    std::uint32_t entry;
    std::uint32_t length;
    char value[kMaxValue + 1];
};

struct Layout {
    std::size_t lhs = 0, rhs = 0, children = 0, token = 0, assign_slot = 0;
    std::size_t scope_province = 0, scope_tag = 0, scope_override = 0, scope_override_valid = 0;
} g_layout;

Registry g_registry;
anvil::HookOf<sym::effect_read_member> g_read_member;
const AnvilControlService* g_control = nullptr;
void* g_vtable[kSlots];
using GetDescFn = void* (*)(void* effect, void* out, void* scope, void* prefix, int indent, bool flag);
GetDescFn g_base_get_desc = nullptr;

std::atomic<std::uint64_t> g_calls{0};
std::array<std::atomic<std::uint64_t>, kMaxEffects> g_parsed{};
// The reasons already logged per key (one line per key and reason).
enum Reason : std::uint32_t { kGameWord = 1, kBlock = 2, kLongValue = 4, kNoMemory = 8 };
std::array<std::atomic<std::uint32_t>, kMaxEffects> g_logged{};

template <class T> T read_at(const void* base, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof value);
    return value;
}

void log_once(int index, Reason reason, const char* why) {
    if (g_logged[static_cast<std::size_t>(index)].fetch_or(reason) & reason) return;
    const Entry& e = g_registry.entry(index);
    anvil::Host{}.warn("{}: {}; the game reads it as its own (an unknown effect is skipped with a line in error.log)",
                       e.key, why);
}

bool caller_of(const AnvilPlugin* plugin, AnvilControlPlugin& out) {
    out = AnvilControlPlugin{};
    out.size = sizeof(out);
    return g_control != nullptr && plugin != nullptr && g_control->plugin_of(plugin, &out) != 0;
}

bool suspended(const AnvilPlugin* owner) {
    AnvilControlPlugin info;
    return caller_of(owner, info) && info.suspended != 0;
}

AnvilScriptCall call_of(Effect* e, void* scope) {
    AnvilScriptCall call{};
    call.size = sizeof(call);
    call.key = g_registry.entry(static_cast<int>(e->entry)).key;
    call.value = e->value;
    call.scope.size = sizeof(call.scope);
    // The scope's own fields, as the game's simple effects read them: the
    // effect's scope bits (ROOT, FROM, ...) are never set on ours.
    call.scope.province = read_at<int>(scope, g_layout.scope_province);
    if (call.scope.province < 0) call.scope.province = 0;
    const bool override_tag = read_at<std::uint8_t>(scope, g_layout.scope_override_valid) != 0;
    const auto* tag = static_cast<const std::uint8_t*>(scope) + (override_tag ? g_layout.scope_override : g_layout.scope_tag);
    if (tag[7] != 0) {  // CCountryTag::_bIsValid
        for (int i = 0; i < 3 && tag[i] >= 0x20 && tag[i] < 0x7F; ++i) call.scope.country[i] = static_cast<char>(tag[i]);
    }
    return call;
}

// --- the five slots of our vtable ---------------------------------------------

void* deleting_dtor(void* self, unsigned flags) noexcept {
    anvil::Host{}.call(sym::effect_dtor, self);
    if (flags & 1) ::operator delete(self);
    return self;
}

bool assign(void* self, const void* token) noexcept {
    auto* e = static_cast<Effect*>(self);
    const char* text = static_cast<const char*>(token) + kTokenText;
    const std::size_t n = strnlen(text, kMaxValue);
    std::memcpy(e->value, text, n);
    e->value[n] = 0;
    e->length = static_cast<std::uint32_t>(n);
    return true;
}

void execute(void* self, void* scope, int) noexcept {
    auto* e = static_cast<Effect*>(self);
    const Entry& entry = g_registry.entry(static_cast<int>(e->entry));
    if (suspended(entry.owner)) return;  // as if the plugin were missing
    const AnvilScriptCall call = call_of(e, scope);
    entry.execute(entry.context, &call);
}

struct MeiText {
    std::uint32_t size;
    int (*game_text_utf8)(void);
};

bool game_text_is_utf8() {
    static const MeiText* mei = anvil::Host{}.service<MeiText>("mei.text", 1);
    return mei != nullptr && mei->size >= sizeof(MeiText) && mei->game_text_utf8 != nullptr && mei->game_text_utf8() != 0;
}

// ADR 0007's rule: UTF-8 when Mei says the game keeps UTF-8, Windows-1252
// with '?' otherwise; the game's colour and variable marks ('§', '$') and line
// breaks are dropped.
std::string game_text(const char* utf8) {
    std::string kept;
    for (const char* p = utf8; *p; ++p) {
        const auto c = static_cast<unsigned char>(*p);
        if (c == '$' || c == '\n' || c == '\r') continue;
        if (c == 0xC2 && static_cast<unsigned char>(p[1]) == 0xA7) {  // U+00A7 in UTF-8
            ++p;
            continue;
        }
        kept.push_back(*p);
    }
    if (game_text_is_utf8()) return kept;
    const int wide = MultiByteToWideChar(CP_UTF8, 0, kept.data(), static_cast<int>(kept.size()), nullptr, 0);
    if (wide <= 0) return {};
    std::wstring w(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, kept.data(), static_cast<int>(kept.size()), w.data(), wide);
    const int bytes = WideCharToMultiByte(1252, 0, w.data(), wide, nullptr, 0, "?", nullptr);
    std::string out(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0) WideCharToMultiByte(1252, 0, w.data(), wide, out.data(), bytes, "?", nullptr);
    return out;
}

void* get_desc(void* self, void* out, void* scope, void* prefix, int indent, bool flag) noexcept {
    // The base frees the prefix the caller passed by value (MSVC: the callee
    // owns it) and writes an empty string, which holds no heap memory.
    g_base_get_desc(self, out, scope, prefix, indent, flag);
    auto* e = static_cast<Effect*>(self);
    const Entry& entry = g_registry.entry(static_cast<int>(e->entry));
    if (entry.describe == nullptr || suspended(entry.owner)) return out;
    const AnvilScriptCall call = call_of(e, scope);
    char line[kMaxLine + 1] = {};
    if (entry.describe(entry.context, &call, line, sizeof line) == 0) return out;
    line[kMaxLine] = 0;
    // The game's own lines end with a line break, and the parent adds the
    // indentation (eu4-engine-lab#77): without it the next line joins ours.
    const std::string text = game_text(line);
    if (!text.empty()) anvil::Host{}.call(sym::cstring_from_text, out, (text + "\n").c_str());
    return out;
}

// --- the hook ---------------------------------------------------------------------

void detour_read_member(void* self, void* reader, int token) noexcept {
    g_registry.close();
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const char* key = static_cast<const char*>(reader) + g_layout.lhs + kTokenText;
    const int index = g_registry.find(key);
    if (index < 0) return g_read_member.next(self, reader, token);

    const auto* rhs = static_cast<const std::uint8_t*>(reader) + g_layout.rhs;
    const char* value = reinterpret_cast<const char*>(rhs + kTokenText);
    if (token != kUnknownToken) {
        log_once(index, kGameWord, "the key is a word of the game or of its data, so the plugin's effect is not used");
        return g_read_member.next(self, reader, token);
    }
    if (value[0] == '{' && value[1] == 0) {
        log_once(index, kBlock, "a block value (key = { ... }) is not an effect of version 1");
        return g_read_member.next(self, reader, token);
    }
    if (strnlen(value, kMaxValue + 1) > kMaxValue) {
        log_once(index, kLongValue, "a value longer than 255 bytes");
        return g_read_member.next(self, reader, token);
    }
    void* memory = ::operator new(sizeof(Effect), std::nothrow);
    if (memory == nullptr) {
        log_once(index, kNoMemory, "out of memory");
        return g_read_member.next(self, reader, token);
    }
    std::memset(memory, 0, sizeof(Effect));
    anvil::Host host;
    host.call(sym::effect_ctor, memory);
    auto* e = static_cast<Effect*>(memory);
    void** vptr = g_vtable;
    std::memcpy(e->base, &vptr, sizeof vptr);
    e->entry = static_cast<std::uint32_t>(index);
    void* child = e;
    host.call(sym::array_append, static_cast<void*>(static_cast<std::uint8_t*>(self) + g_layout.children), &child);
    using AssignFn = bool (*)(void*, const void*);
    reinterpret_cast<AssignFn>(g_vtable[g_layout.assign_slot])(e, rhs);
    std::memcpy(e->base + g_layout.token, &token, sizeof token);
    g_parsed[static_cast<std::size_t>(index)].fetch_add(1, std::memory_order_relaxed);
}

// --- the service ------------------------------------------------------------------

AnvilResult svc_register_effect(AnvilPlugin* self, const AnvilScriptEffect* effect) {
    AnvilControlPlugin info;
    Caller caller;
    if (caller_of(self, info)) {
        caller.known = true;
        caller.id = info.id != nullptr ? info.id : "";
        caller.affects_gameplay = info.affects_gameplay != 0;
        caller.early = info.early != 0;
    }
    std::string why;
    const AnvilResult r = g_registry.add(caller, self, effect, why);
    const char* key = effect != nullptr && effect->key != nullptr ? effect->key : "(none)";
    const std::string owner = caller.known ? caller.id : std::string("an unknown plugin");
    if (r == ANVIL_OK) anvil::Host{}.info("{} registered {}", owner, key);
    else anvil::Host{}.warn("{}: {} refused: {}", owner, key, why);
    return r;
}

const AnvilScriptEffectsService kService{sizeof(AnvilScriptEffectsService), svc_register_effect};

void report_ready() {
    anvil::Host host;
    const std::size_t n = g_registry.size();
    if (n == 0) {
        host.info("game ready: no registered effect; CEffect::ReadMember ran {} times", g_calls.load());
        return;
    }
    std::string counts;
    for (std::size_t i = 0; i < n; ++i) {
        if (!counts.empty()) counts += ", ";
        counts += std::string(g_registry.entry(static_cast<int>(i)).key) + " " + std::to_string(g_parsed[i].load());
    }
    host.info("game ready: parsed {} (CEffect::ReadMember ran {} times)", counts, g_calls.load());
}

struct AnvilScript : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_control = host.service<AnvilControlService>(ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION);
        if (g_control == nullptr ||
            g_control->size < offsetof(AnvilControlService, plugin_of) + sizeof(g_control->plugin_of)) {
            throw anvil::Error(ANVIL_ERROR_UNSUPPORTED, "anvil.control has no plugin_of; this Anvil is older than the plugin");
        }
        g_layout.lhs = host.value(sym::effect_lhs_site, "lhs");
        g_layout.rhs = host.value(sym::effect_assign_site, "rhs");
        g_layout.children = host.value(sym::effect_assign_site, "children");
        g_layout.token = host.value(sym::effect_set_token, "token");
        g_layout.assign_slot = host.value(sym::effect_assign_site, "assign") / sizeof(void*);
        g_layout.scope_province = host.value(sym::scope_province_site, "province");
        g_layout.scope_tag = host.value(sym::scope_country_site, "tag");
        g_layout.scope_override = host.value(sym::scope_country_site, "override");
        g_layout.scope_override_valid = host.value(sym::scope_country_site, "override_valid");

        // The game's vtable: its ReadMember slot must be the hooked function,
        // and the slots we keep must be code of eu4.exe.
        void* const* game = host.data(sym::vtable);
        const std::uintptr_t base = host.game().image_base;
        const std::uintptr_t end = base + host.game().image_size;
        bool inside = true;
        for (std::size_t i = 0; i < kSlots; ++i) {
            const auto slot = reinterpret_cast<std::uintptr_t>(game[i]);
            inside = inside && slot >= base && slot < end;
        }
        if (!inside || game[kSlotReadMember] != host.address(sym::effect_read_member) ||
            g_layout.assign_slot >= kSlots || g_layout.token + sizeof(int) > kEffectSize) {
            throw anvil::Error(ANVIL_ERROR_BYTES_MISMATCH, "CEffect's vtable is not the one this plugin knows");
        }
        std::memcpy(g_vtable, game, sizeof g_vtable);
        g_base_get_desc = reinterpret_cast<GetDescFn>(game[kSlotGetDesc]);
        g_vtable[kSlotDeletingDtor] = reinterpret_cast<void*>(&deleting_dtor);
        g_vtable[g_layout.assign_slot] = reinterpret_cast<void*>(&assign);
        g_vtable[kSlotGetDesc] = reinterpret_cast<void*>(&get_desc);
        g_vtable[kSlotExecute] = reinterpret_cast<void*>(&execute);

        host.hook(g_read_member, sym::effect_read_member, &detour_read_member);
        host.register_service(ANVIL_SCRIPT_EFFECTS_SERVICE, ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION, kService);
        host.subscribe("anvil-game.ready", [] { report_ready(); });
    }
};

}  // namespace

ANVIL_PLUGIN(AnvilScript)
