// The rules of anvil-game.commands (include/anvil/anvil_game.h, anvil#256),
// apart from the game: who may call, which units, intents, limits, and what
// happens to a command the session refuses. The game's side is a backend:
// plugin.cpp has the real one, the tests a fake.
#pragma once

#include <anvil/anvil_game.h>

#include <cstdint>
#include <vector>

namespace anvil_game {

// One of the player's units, as the backend found it.
struct CommandUnit {
    std::uintptr_t object = 0;  // the game's unit
    std::uint64_t id = 0;
    std::uint32_t kind = 0;      // ANVIL_GAME_UNIT_*
    std::int32_t location = 0;   // a province ID, 0 for none
    bool drilling = false;
    std::uint32_t subunits = 0;  // regiments or ships
};

class CommandBackend {
public:
    virtual ~CommandBackend() = default;

    // The caller's manifest and whether Anvil suspended it now; false when
    // `self` is no plugin's handle.
    virtual bool caller(const AnvilPlugin* self, bool& affects_gameplay, bool& suspended) = 0;
    // Whether this game build has the symbols of a command type.
    virtual bool declared(std::uint32_t type) = 0;
    // On the main thread, after an outer frame, in a session with a game session object.
    virtual bool open() = 0;
    // This machine's session takes part in a hot-join.
    virtual bool hot_join() = 0;
    // The frame the call is in, for the limit per frame.
    virtual std::uint64_t frame() = 0;
    // The player's own unit for `ref`: ANVIL_ERROR_NOT_FOUND when it is gone or
    // another country's, ANVIL_ERROR_INTERNAL when memory is not what it should be.
    virtual AnvilResult find(const AnvilGameUnitRef& ref, CommandUnit& out) = 0;

    // A new command, built by the game's allocator and constructor; nullptr
    // when the allocation failed.
    virtual void* make_drill(const CommandUnit& unit) = 0;
    virtual void* make_split(const CommandUnit& unit, std::int32_t count) = 0;
    virtual void* make_merge(const std::vector<CommandUnit>& units) = 0;
    // Hands a command to the session: true when it took it (and owns it now).
    virtual bool submit(void* command) = 0;
    // Frees a command the session refused, through its deleting destructor.
    virtual void destroy(void* command) = 0;
};

// The regiments or ships the split button moves: half, rounded up.
std::int32_t split_count(std::uint32_t subunits);

// Main thread only, as the service it serves.
class CommandPoster {
public:
    AnvilResult post(CommandBackend& backend, const AnvilPlugin* self, const AnvilGameCommand* command,
                     AnvilGameCommandResult* out);

private:
    std::uint64_t frame_ = 0;
    std::uint32_t used_ = 0;  // commands posted in frame_
};

}  // namespace anvil_game
