// What the inspector shows, copied out of the game in one read: the province
// whose view is open, its owner and the player's country, each with its flags,
// variables and timed modifiers, and a country's ruler, heir and consort.
// Nothing here keeps a pointer into the game, so a save load leaves nothing
// stale behind; the next read starts over.
#pragma once

#include <anvil/anvil_game.h>

#include <cstdint>
#include <string>
#include <vector>

namespace anvil_inspector {

struct Flag {
    std::string name;
    AnvilGameDate date{};
};

struct Variable {
    std::string name;
    std::int32_t value = 0;  // x 1000
};

struct Timed {
    std::string key;
    std::uint32_t flags = 0;  // ANVIL_GAME_TIMED_*
    AnvilGameDate start{};
    AnvilGameDate end{};
};

// A list and how its read went: ANVIL_ERROR_UNSUPPORTED when the running
// anvil-game has no such call, ANVIL_ERROR_NOT_DECLARED when this game build
// lacks its symbols.
template <class T>
struct List {
    AnvilResult result = ANVIL_ERROR_UNSUPPORTED;
    std::vector<T> items;
};

struct Monarch {
    AnvilResult result = ANVIL_ERROR_UNSUPPORTED;  // ANVIL_ERROR_NOT_FOUND: none
    std::string name;
    std::string dynasty;
    std::int32_t adm = 0, dip = 0, mil = 0;
    List<Flag> flags;
};

enum class Role { province, owner, player, owner_and_player };

struct Scope {
    Role role = Role::province;
    AnvilResult result = ANVIL_OK;  // of the scope itself (an unknown tag, a province that went away)
    std::int32_t province = 0;      // for Role::province
    std::string tag;                // for a country
    std::string name;               // a province's name
    List<Flag> flags;
    List<Variable> variables;
    List<Timed> modifiers;
    Monarch monarchs[3];  // ruler, heir, consort, for a country
};

struct Snapshot {
    bool session = false;  // false: no game is running, and the rest is empty
    AnvilGameDate date{};
    AnvilResult view = ANVIL_ERROR_UNSUPPORTED;  // of province_view: ANVIL_ERROR_NOT_FOUND when no view is open
    std::vector<Scope> scopes;
};

// What decides whether a read is due: a new session, date, view or player.
struct Key {
    std::uint64_t session = 0;
    std::int32_t year = 0;
    std::uint32_t month = 0, day = 0;
    std::int32_t province = 0;
    std::string player;

    bool operator==(const Key&) const = default;
};

// Main thread only, after an outer frame (run_on_main_thread work), like every
// read of anvil-game.world. `world` may be NULL.
Key key_of(const AnvilGameService& game, const AnvilGameWorldService* world);
Snapshot read_snapshot(const AnvilGameService& game, const AnvilGameWorldService* world);

}  // namespace anvil_inspector
