// Member offsets of the owner classes of EU4 1.37.5, the same in the three
// builds (EU4 engine lab, docs/script-profiler-prototype.md):
//   CEvent: _DebugKey +32, _Trigger +64, _MajorTrigger +184, _Immediate +304
//     and _AfterEffect +1120 (CEventOption), _Options +1936 (vector of
//     CEventOption*), _MTTH +1960 (CMeanTimeToHappen);
//   CEventOption: _AIChance +16 (a CMeanTimeToHappen), _Effect +80, _Trigger +168;
//   CMeanTimeToHappen: _Modifiers +24, a vector of CMTTHModifier (240 bytes
//     each, a trigger block itself, with its _Potential block at +112);
//   CDecision: _Tag +16, _PotentialTrigger +48, _AllowedTrigger +168,
//     _ProvincesToHighlightTrigger +288, _Effect +408, _AICheck +464;
//   COnActionList: _Tag +24, _Effect +104;
//   CCasusBelliType: _Tag +24, _PrerequisitesSelf +56, _Prerequisites +176;
//   CTriggeredModifier and CProvinceTriggeredModifier: _Tag +344, _Potential
//     +376, _Trigger +496; the province one also _OnActivation +616 and
//     _OnDeactivation +672;
//   CMission: _Tag +16, _Trigger +112, _Effect +232, _AIWeight +320,
//     _ProvincesToHighlightTrigger +384.
// Each owner keeps its vtable at +0.
#include "owners.hpp"

#include <windows.h>

#include <cstring>

namespace script_profiler {

namespace {

struct Rule {
    OwnerKind kind;
    std::uintptr_t Vtables::*vtable;
    int offset;  // of the block in the owner
    bool effect;
    const char* role;
    int name;  // offset of the owner's name (an MSVC std::string)
};

constexpr int kEventKey = 32;
constexpr int kEventImmediate = 304;
constexpr int kEventAfter = 1120;
constexpr int kEventOptions = 1936;
constexpr int kEventMtth = 1960;
constexpr int kOptionAiChance = 16;
constexpr int kOptionEffect = 80;
constexpr int kOptionTrigger = 168;
constexpr int kMtthModifiers = 24;
constexpr int kModifierSize = 240;
constexpr int kModifierPotential = 112;
constexpr int kDecisionAiCheck = 464;
constexpr int kMissionAiWeight = 320;
constexpr std::size_t kMaxHeld = 256;  // options or modifiers of one owner; more is not a real one

const Rule kRules[] = {
    {OwnerKind::event, &Vtables::event, 64, false, "trigger", kEventKey},
    {OwnerKind::event, &Vtables::event, 184, false, "major_trigger", kEventKey},
    {OwnerKind::event, &Vtables::event, kEventImmediate + kOptionEffect, true, "immediate", kEventKey},
    {OwnerKind::event, &Vtables::event, kEventAfter + kOptionEffect, true, "after", kEventKey},
    {OwnerKind::decision, &Vtables::decision, 48, false, "potential", 16},
    {OwnerKind::decision, &Vtables::decision, 168, false, "allow", 16},
    {OwnerKind::decision, &Vtables::decision, 288, false, "provinces_to_highlight", 16},
    {OwnerKind::decision, &Vtables::decision, 408, true, "effect", 16},
    {OwnerKind::on_action, &Vtables::on_action, 104, true, "effect", 24},
    {OwnerKind::casus_belli, &Vtables::casus_belli, 56, false, "prerequisites_self", 24},
    {OwnerKind::casus_belli, &Vtables::casus_belli, 176, false, "prerequisites", 24},
    {OwnerKind::country_modifier, &Vtables::country_modifier, 376, false, "potential", 344},
    {OwnerKind::country_modifier, &Vtables::country_modifier, 496, false, "trigger", 344},
    {OwnerKind::province_modifier, &Vtables::province_modifier, 376, false, "potential", 344},
    {OwnerKind::province_modifier, &Vtables::province_modifier, 496, false, "trigger", 344},
    {OwnerKind::province_modifier, &Vtables::province_modifier, 616, true, "on_activation", 344},
    {OwnerKind::province_modifier, &Vtables::province_modifier, 672, true, "on_deactivation", 344},
    {OwnerKind::mission, &Vtables::mission, 112, false, "trigger", 16},
    {OwnerKind::mission, &Vtables::mission, 384, false, "provinces_to_highlight", 16},
    {OwnerKind::mission, &Vtables::mission, 232, true, "effect", 16},
};

const Owner kNone{};

// A vector's [first, last) of `size`-byte elements; false when it does not look like one.
bool read_vector(std::uintptr_t at, std::size_t size, std::uintptr_t& first, std::size_t& count) {
    std::uintptr_t v[3] = {};
    if (!read_memory(at, v, sizeof(v))) return false;
    if (v[1] < v[0] || v[2] < v[1] || (v[1] - v[0]) % size != 0) return false;
    first = v[0];
    count = (v[1] - v[0]) / size;
    return count <= kMaxHeld && (count == 0 || first != 0);
}

}  // namespace

const char* kind_key(OwnerKind kind) {
    switch (kind) {
    case OwnerKind::event: return "event";
    case OwnerKind::event_option: return "event_option";
    case OwnerKind::decision: return "decision";
    case OwnerKind::on_action: return "on_action";
    case OwnerKind::casus_belli: return "casus_belli";
    case OwnerKind::country_modifier: return "triggered_modifier";
    case OwnerKind::province_modifier: return "province_triggered_modifier";
    case OwnerKind::mission: return "mission";
    case OwnerKind::none: break;
    }
    return "none";
}

bool read_memory(std::uintptr_t at, void* out, std::size_t size) noexcept {
    if (at < 0x10000) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string read_game_string(std::uintptr_t at) {
    struct {
        char buffer[16];
        std::uint64_t size, capacity;
    } s{};
    if (!read_memory(at, &s, sizeof(s)) || s.size > 256 || s.capacity < s.size) return {};
    std::string out(static_cast<std::size_t>(s.size), '\0');
    if (s.capacity <= 15) {
        std::memcpy(out.data(), s.buffer, out.size());
    } else {
        std::uintptr_t p = 0;
        std::memcpy(&p, s.buffer, sizeof(p));
        if (!read_memory(p, out.data(), out.size())) return {};
    }
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    }
    return out;
}

void Resolver::add(const void* block, const Owner& owner, const char* role, int option) {
    Owner o = owner;
    o.role = role;
    o.option = option;
    held_.try_emplace(block, std::move(o));
}

void Resolver::add_modifiers(std::uintptr_t mtth, const Owner& owner, const char* role, int option) {
    std::uintptr_t first = 0;
    std::size_t count = 0;
    if (!read_vector(mtth + kMtthModifiers, kModifierSize, first, count)) return;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uintptr_t m = first + i * kModifierSize;
        add(reinterpret_cast<const void*>(m), owner, role, option);
        add(reinterpret_cast<const void*>(m + kModifierPotential), owner, role, option);
    }
}

void Resolver::expand(const Owner& owner) {
    if (!expanded_.insert(owner.object).second) return;
    const auto o = reinterpret_cast<std::uintptr_t>(owner.object);
    switch (owner.kind) {
    case OwnerKind::event: {
        add_modifiers(o + kEventMtth, owner, "mean_time_to_happen");
        std::uintptr_t first = 0;
        std::size_t count = 0;
        if (!read_vector(o + kEventOptions, sizeof(void*), first, count)) break;
        for (std::size_t i = 0; i < count; ++i) {
            std::uintptr_t option = 0;
            if (!read_memory(first + i * sizeof(void*), &option, sizeof(option)) || option == 0) continue;
            Owner opt = owner;
            opt.kind = OwnerKind::event_option;
            opt.object = reinterpret_cast<const void*>(option);
            const int number = static_cast<int>(i) + 1;
            add(reinterpret_cast<const void*>(option + kOptionTrigger), opt, "trigger", number);
            add(reinterpret_cast<const void*>(option + kOptionEffect), opt, "effect", number);
            add_modifiers(option + kOptionAiChance, opt, "ai_chance", number);
        }
        break;
    }
    case OwnerKind::decision: add_modifiers(o + kDecisionAiCheck, owner, "ai_will_do"); break;
    case OwnerKind::mission: add_modifiers(o + kMissionAiWeight, owner, "ai_weight"); break;
    default: break;
    }
}

Owner Resolver::find(const void* block, bool effect) {
    const auto b = reinterpret_cast<std::uintptr_t>(block);
    for (const Rule& r : kRules) {
        if (r.effect != effect) continue;
        const std::uintptr_t vtable = vtables_.*r.vtable;
        if (vtable == 0 || b < static_cast<std::uintptr_t>(r.offset)) continue;
        const std::uintptr_t owner = b - static_cast<std::uintptr_t>(r.offset);
        std::uintptr_t seen = 0;
        if (!read_memory(owner, &seen, sizeof(seen)) || seen != vtable) continue;
        Owner o;
        o.kind = r.kind;
        o.object = reinterpret_cast<const void*>(owner);
        o.name = read_game_string(owner + static_cast<std::uintptr_t>(r.name));
        o.role = r.role;
        return o;
    }
    return {};
}

const Owner& Resolver::owner_of(const void* block, bool effect, bool read_memory_now) {
    if (const auto it = cache_.find(block); it != cache_.end()) return it->second;
    if (const auto it = held_.find(block); it != held_.end()) return cache_.emplace(block, it->second).first->second;
    if (!read_memory_now) return kNone;
    Owner o = find(block, effect);
    if (o.kind != OwnerKind::none) {
        Owner owner = o;
        owner.role = "";
        expand(owner);
    }
    return cache_.emplace(block, std::move(o)).first->second;
}

void Resolver::retry_unowned() {
    for (auto it = cache_.begin(); it != cache_.end();) {
        it = it->second.kind == OwnerKind::none ? cache_.erase(it) : std::next(it);
    }
}

void Resolver::clear() {
    cache_.clear();
    held_.clear();
    expanded_.clear();
}

}  // namespace script_profiler
