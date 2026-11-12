// The owner of a root block (docs/script-profiler.md): the event, decision,
// on_action, casus belli, triggered modifier or mission whose member the block
// is, with the block's role there (trigger, effect, potential...).
//
// eu4.exe has no RTTI for its own classes, so an owner is told by its vtable
// (symbols.toml) at a known offset before the block. Blocks an owner holds
// elsewhere (an event's options and its mean_time_to_happen, a decision's
// ai_will_do, a mission's ai_weight) are found from the owner once one of its
// member blocks has been seen. Every read of game memory is guarded: a block
// that is not what the tables say has no owner.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace script_profiler {

enum class OwnerKind : std::uint8_t {
    none,
    event,
    event_option,
    decision,
    on_action,
    casus_belli,
    country_modifier,   // triggered_modifier (CTriggeredModifier)
    province_modifier,  // province_triggered_modifier (CProvinceTriggeredModifier)
    mission,
};

const char* kind_key(OwnerKind kind);  // "event", "event_option", ...: the CSV's words

// Where each owner class's vtable lies in the running eu4.exe; 0 when the
// build declares it absent (then no block is given that owner).
struct Vtables {
    std::uintptr_t event = 0;
    std::uintptr_t decision = 0;
    std::uintptr_t on_action = 0;
    std::uintptr_t casus_belli = 0;
    std::uintptr_t country_modifier = 0;
    std::uintptr_t province_modifier = 0;
    std::uintptr_t mission = 0;
};

struct Owner {
    OwnerKind kind = OwnerKind::none;
    const void* object = nullptr;  // the owner; for an option, the option
    std::string name;              // the event key, decision name...; for an option, its event's key
    int option = 0;                // an event option's number, from 1
    const char* role = "";         // the block's script key: "trigger", "effect", "potential"...
};

class Resolver {
public:
    explicit Resolver(Vtables vtables) : vtables_(vtables) {}

    // The owner of a root block, cached. Reading game memory finds new owners;
    // without it (when the game may have freed its data) only blocks of owners
    // already found are named.
    const Owner& owner_of(const void* block, bool effect, bool read_memory = true);
    // Forgets blocks that had no owner, so that the next owner_of tries again
    // (after more owners were found).
    void retry_unowned();
    void clear();

private:
    Owner find(const void* block, bool effect);
    void expand(const Owner& owner);
    void add(const void* block, const Owner& owner, const char* role, int option = 0);
    void add_modifiers(std::uintptr_t mtth, const Owner& owner, const char* role, int option = 0);

    Vtables vtables_;
    std::unordered_map<const void*, Owner> cache_;
    std::unordered_map<const void*, Owner> held_;   // blocks found through their owner
    std::unordered_set<const void*> expanded_;      // owners whose held blocks are in held_
};

// Guarded reads of game memory.
bool read_memory(std::uintptr_t at, void* out, std::size_t size) noexcept;
// An MSVC std::string as eu4.exe lays it out; "" when it does not look like one.
std::string read_game_string(std::uintptr_t at);

}  // namespace script_profiler
