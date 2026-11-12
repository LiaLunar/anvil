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

// The local player's event window for a pending event, as the backend found it.
struct EventWindow {
    std::uintptr_t object = 0;  // the game's CEventWindow
    bool keep_alive = false;    // no option chosen yet
    bool has_button = false;    // the asked option has a button in the window
    bool enabled = false;       // and it is enabled now
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

    // Version 2. The event window whose pending event has `pending`, and the
    // button of `option` in it: ANVIL_ERROR_NOT_FOUND without such a window.
    virtual AnvilResult find_event_window(std::int32_t pending, std::int32_t option, EventWindow& out) = 0;
    // The conditions of the game's post function hold for the session now.
    virtual bool post_allowed() = 0;
    // CEventWindow::OptionSelected: what the button does.
    virtual void choose_option(const EventWindow& window, std::int32_t option) = 0;

    // Version 3. The game speed now (1 the slowest .. 5) and whether the speed
    // bar's buttons act now (no in-game menu shown, the player has admin
    // rights); false when the speed cannot be read.
    virtual bool game_speed(std::int32_t& speed, bool& buttons_act) = 0;
    // One step of the speed bar: CIncreaseGameSpeedCommand or CDecrease...
    virtual void* make_speed_step(bool up) = 0;
};

// The regiments or ships the split button moves: half, rounded up.
std::int32_t split_count(std::uint32_t subunits);

// Main thread only, as the service it serves.
class CommandPoster {
public:
    AnvilResult post(CommandBackend& backend, const AnvilPlugin* self, const AnvilGameCommand* command,
                     AnvilGameCommandResult* out);

private:
    AnvilResult choose(CommandBackend& backend, const AnvilGameCommand& command, AnvilGameCommandResult* out);
    AnvilResult set_speed(CommandBackend& backend, const AnvilGameCommand& command, AnvilGameCommandResult* out);

    std::uint64_t frame_ = 0;
    std::uint32_t used_ = 0;  // commands posted in frame_
};

}  // namespace anvil_game
