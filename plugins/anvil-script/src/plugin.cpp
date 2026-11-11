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
//
// The Day end (docs/adr/0009-day-end.md): a function hook on
// CGameState::HandleDailyTick calls the game's day, then the subscribers in
// the order they subscribed, on the main thread, before the day's command
// compares checksums.
//
// has_dlc for plugin names (anvil#316, docs/spec/has-dlc-v1.md): a function
// hook on CHasDLCTrigger::Assign takes a name "anvil:<id>" for itself and gives
// that trigger a copy of the game's vtable whose Evaluate answers from a table
// fixed when a session starts or ends. Every other name goes on to the game.
#include <anvil/anvil.hpp>
#include <anvil/anvil_game.h>
#include <anvil/anvil_script.h>

#define NOMINMAX
#include <windows.h>

#include "anvil_control.h"
#include "day_end.hpp"
#include "dlc_gate.hpp"
#include "registry.hpp"
#include "text.hpp"
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
DayEndList g_day_end;
anvil::HookOf<sym::daily_tick> g_daily_tick;
std::size_t g_date_offset = 0;
std::atomic<std::uint64_t> g_days{0};
std::atomic<DWORD> g_day_thread{0};
std::atomic<bool> g_day_thread_logged{false};
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

// ADR 0007's rule, as anvil-map applies it (docs/spec/map-modes-v1.md section 7):
// UTF-8 when Mei says the game keeps UTF-8, Windows-1252 with '?' otherwise,
// cut at the first byte that is not UTF-8; text is literal, so '$' stays and
// the game's colour sign U+00A7, which has no escape, becomes '?'. A describe
// line is one line: line breaks are dropped (the service adds the one the
// game's lines end with).
std::string game_text(const char* utf8) {
    std::string text = anvil_map::game_text(utf8, game_text_is_utf8());
    text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
    return text;
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

// --- the Day end -----------------------------------------------------------------

void detour_daily_tick(void* game_state) noexcept {
    // Closed before the game's day, so a subscription from inside it is refused too.
    g_day_end.close();
    g_daily_tick.next(game_state);
    g_days.fetch_add(1, std::memory_order_relaxed);
    const std::size_t n = g_day_end.size();
    if (n == 0) return;
    const DWORD thread = GetCurrentThreadId();
    DWORD first = 0;
    if (!g_day_thread.compare_exchange_strong(first, thread) && first != thread &&
        !g_day_thread_logged.exchange(true)) {
        anvil::Host{}.warn("day end on thread {}, the first was on thread {}; the game's day moved threads", thread,
                           first);
    }
    const AnvilScriptDay day = decode_day(read_at<std::int32_t>(game_state, g_date_offset));
    for (std::size_t i = 0; i < n; ++i) {
        const DayEndEntry& e = g_day_end.entry(i);
        if (suspended(e.owner)) continue;
        e.callback(e.context, &day);
    }
}

AnvilResult svc_subscribe_day_end(AnvilPlugin* self, AnvilScriptDayEnd callback, void* context) {
    AnvilControlPlugin info;
    const bool known = caller_of(self, info);
    std::string why;
    const AnvilResult r = g_day_end.add(known, known && info.affects_gameplay != 0, self, callback, context, why);
    const std::string owner = known && info.id != nullptr ? std::string(info.id) : std::string("an unknown plugin");
    if (r == ANVIL_OK) anvil::Host{}.info("{} subscribed to the day end", owner);
    else anvil::Host{}.warn("{}: day end refused: {}", owner, why);
    return r;
}

const AnvilScriptDayEndService kDayEndService{sizeof(AnvilScriptDayEndService), svc_subscribe_day_end};

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

// --- has_dlc for plugin names ------------------------------------------------------

constexpr std::size_t kTriggerSlots = 16;    // CHasDLCTrigger::vftable, CTrigger's 16 virtual slots
constexpr std::size_t kSlotEvaluate = 6;     // bool Evaluate(CEventScope&)
constexpr std::size_t kSlotAssign = 7;       // bool Assign(const CToken&)

GateTable g_gates;
anvil::HookOf<sym::has_dlc_assign> g_has_dlc_assign;
void* g_gate_vtable[kTriggerSlots];
std::size_t g_gate_index_offset = 0;  // the padding after the DLC bits: our index
std::atomic<bool> g_gate_full_logged{false};

bool gate_evaluate(void* self, void*) noexcept {
    return g_gates.answer(read_at<std::int32_t>(self, g_gate_index_offset));
}

bool detour_has_dlc_assign(void* self, const void* token) noexcept {
    const char* text = static_cast<const char*>(token) + kTokenText;
    const std::string_view id = gate_id(std::string_view(text, strnlen(text, kMaxGateId + kDlcPrefix.size() + 1)));
    if (id.empty()) return g_has_dlc_assign.next(self, token);
    const int index = g_gates.intern(id);
    if (index < 0) {
        if (!g_gate_full_logged.exchange(true)) {
            anvil::Host{}.warn("has_dlc: more than {} plugin names; {} is left to the game, which answers false",
                               kMaxGates, text);
        }
        return g_has_dlc_assign.next(self, token);
    }
    // The game's constructor cleared the DLC bits, so its own tooltip, which
    // the copy keeps, names no DLC. The index goes into the padding behind them.
    std::memcpy(static_cast<std::uint8_t*>(self) + g_gate_index_offset, &index, sizeof index);
    void** vptr = g_gate_vtable;
    std::memcpy(self, &vptr, sizeof vptr);
    return true;
}

// A session started or ended: the answers for it. Anvil suspends plugins
// before any plugin hears of the session, so the entries say who acts now.
void on_session(const void* payload) {
    AnvilGameSession s{};
    if (payload != nullptr) std::memcpy(&s, payload, std::min<std::size_t>(sizeof s, *static_cast<const std::uint32_t*>(payload)));
    const bool allowed = s.change == ANVIL_GAME_SESSION_STARTED && s.multiplayer == 0;
    std::vector<std::string> active;
    AnvilControlStatus status{};
    status.size = sizeof status;
    if (g_control != nullptr) g_control->status(&status);
    for (std::uint32_t i = 0; allowed && g_control != nullptr && i < status.plugins; ++i) {
        AnvilControlPlugin p{};
        p.size = sizeof p;
        if (g_control->plugin(i, &p) == 0 || p.id == nullptr || p.state == nullptr) continue;
        if (std::strcmp(p.state, "loaded") == 0 && p.suspended == 0) active.emplace_back(p.id);
    }
    g_gates.update(allowed, active);
    if (g_gates.size() == 0) return;
    std::string answers;
    for (std::size_t i = 0; i < g_gates.size(); ++i) {
        if (!answers.empty()) answers += ", ";
        answers += g_gates.id(static_cast<int>(i)) + (g_gates.answer(static_cast<int>(i)) ? " yes" : " no");
    }
    anvil::Host{}.info("has_dlc for this {}: {}", allowed ? "session" : (s.change == ANVIL_GAME_SESSION_STARTED ? "multiplayer session" : "menu"), answers);
}

bool g_effects = false, g_day_end_hooked = false, g_gate = false;

std::string day_end_subscribers() {
    if (!g_day_end_hooked) return "day end: not on this build";
    std::string ids;
    for (std::size_t i = 0; i < g_day_end.size(); ++i) {
        AnvilControlPlugin info;
        if (!ids.empty()) ids += ", ";
        ids += caller_of(g_day_end.entry(i).owner, info) && info.id != nullptr ? info.id : "?";
    }
    return ids.empty() ? "day end: no subscriber" : "day end: " + ids;
}

std::string gate_names() {
    if (!g_gate) return "has_dlc for plugins: not on this build";
    if (g_gates.size() == 0) return "has_dlc for plugins: no name parsed";
    std::string names;
    for (std::size_t i = 0; i < g_gates.size(); ++i) {
        if (!names.empty()) names += ", ";
        names += g_gates.id(static_cast<int>(i)) + " " + std::to_string(g_gates.parsed(static_cast<int>(i)));
    }
    return "has_dlc for plugins: " + names;
}

void report_ready() {
    anvil::Host host;
    const std::size_t n = g_registry.size();
    const std::string day_end = day_end_subscribers() + "; " + gate_names();
    if (!g_effects) {
        host.info("game ready: no effects on this build; {}", day_end);
        return;
    }
    if (n == 0) {
        host.info("game ready: no registered effect; CEffect::ReadMember ran {} times; {}", g_calls.load(), day_end);
        return;
    }
    std::string counts;
    for (std::size_t i = 0; i < n; ++i) {
        if (!counts.empty()) counts += ", ";
        counts += std::string(g_registry.entry(static_cast<int>(i)).key) + " " + std::to_string(g_parsed[i].load());
    }
    host.info("game ready: parsed {} (CEffect::ReadMember ran {} times); {}", counts, g_calls.load(), day_end);
}

bool all(std::initializer_list<bool> values) {
    return std::all_of(values.begin(), values.end(), [](bool v) { return v; });
}

struct AnvilScript : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_control = host.service<AnvilControlService>(ANVIL_CONTROL_SERVICE, ANVIL_CONTROL_VERSION);
        if (g_control == nullptr ||
            g_control->size < offsetof(AnvilControlService, plugin_of) + sizeof(g_control->plugin_of)) {
            throw anvil::Error(ANVIL_ERROR_UNSUPPORTED, "anvil.control has no plugin_of; this Anvil is older than the plugin");
        }
        // Each service needs only its own symbols (spec day-end-v1 section 7):
        // a build without one set still gets the other.
        g_effects = all({host.present(sym::effect_read_member), host.present(sym::effect_lhs_site),
                         host.present(sym::effect_assign_site), host.present(sym::effect_set_token),
                         host.present(sym::effect_ctor), host.present(sym::effect_vtable_site),
                         host.present(sym::effect_dtor), host.present(sym::array_append),
                         host.present(sym::scope_province_site), host.present(sym::scope_country_site),
                         host.present(sym::cstring_from_text)});
        g_day_end_hooked = host.present(sym::daily_tick) && host.present(sym::daily_tick_date_site);
        g_gate = host.present(sym::has_dlc_assign) && host.present(sym::has_dlc_make);
        if (!g_effects && !g_day_end_hooked && !g_gate) {
            throw anvil::Error(ANVIL_ERROR_NOT_DECLARED,
                               "neither the effects', the day end's nor has_dlc's symbols are on this build");
        }
        if (g_gate) g_gate = load_gate(host);
        else host.warn("has_dlc's symbols are absent on this build: no plugin names in has_dlc");
        if (g_effects) load_effects(host);
        else host.warn("the effects' symbols are absent on this build: no anvil-script.effects");
        if (g_day_end_hooked) {
            g_date_offset = host.value(sym::daily_tick_date_site, "date");
            host.hook(g_daily_tick, sym::daily_tick, &detour_daily_tick);
            host.register_service(ANVIL_SCRIPT_DAY_END_SERVICE, ANVIL_SCRIPT_DAY_END_SERVICE_VERSION, kDayEndService);
        } else {
            host.warn("the day end's symbols are absent on this build: no anvil-script.day_end");
        }
        host.subscribe("anvil-game.ready", [] { report_ready(); });
    }

    // The trigger's layout and the game's vtable must be the ones this plugin
    // knows; anything else leaves has_dlc to the game.
    static bool load_gate(anvil::Host& host) {
        const std::size_t size = host.value(sym::has_dlc_make, "trigger_size");
        const std::size_t bits = host.value(sym::has_dlc_make, "dlc_bits");
        void* const* game = host.data(sym::dlc_vtable);
        const std::uintptr_t base = host.game().image_base;
        const std::uintptr_t end = base + host.game().image_size;
        bool inside = game != nullptr;
        for (std::size_t i = 0; inside && i < kTriggerSlots; ++i) {
            const auto slot = reinterpret_cast<std::uintptr_t>(game[i]);
            inside = slot >= base && slot < end;
        }
        if (!inside || game[kSlotAssign] != host.address(sym::has_dlc_assign) || bits + 8 > size) {
            host.warn("CHasDLCTrigger is not the one this plugin knows: no plugin names in has_dlc");
            return false;
        }
        g_gate_index_offset = bits + 4;
        std::memcpy(g_gate_vtable, game, sizeof g_gate_vtable);
        g_gate_vtable[kSlotEvaluate] = reinterpret_cast<void*>(&gate_evaluate);
        host.hook(g_has_dlc_assign, sym::has_dlc_assign, &detour_has_dlc_assign);
        host.subscribe(ANVIL_GAME_EVENT_SESSION, [](std::string_view, const void* payload) { on_session(payload); });
        return true;
    }

    static void load_effects(anvil::Host& host) {
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
    }
};

}  // namespace

ANVIL_PLUGIN(AnvilScript)
