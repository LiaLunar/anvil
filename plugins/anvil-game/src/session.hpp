// Game sessions as anvil-game sees them, one view per frame of the main loop.
#pragma once

#include <anvil/anvil_game.h>

#include <cstdint>

namespace anvil_game {

// What one frame read from the game.
struct SessionView {
    bool readable = true;  // false: reading faulted; the view says nothing
    bool running = false;  // a game session runs: game session class, speed object, date set
    std::uintptr_t session = 0;
    std::int32_t date = 0;  // the root's date (hours since 1 January -5000) while running
    bool ironman = false;
    bool multiplayer = false;
    // How many times the game has put a session into the root so far. A save
    // loaded from the game menu puts the same session object back, inside one
    // frame of the main loop, so only this count tells the two sessions apart.
    std::uint64_t installs = 0;
};

// 1 January of year 1: the date of a root without a running game, or of a
// session whose save is still being read into it.
constexpr std::int32_t kUnsetDate = 43'800'000 + 365 * 24;

// The game's date: hours since 1 January of year -5000 in 365-day years (no
// leap days), as Tongs reads it for tongs_state. `session` is left 0.
AnvilGameDate decode_date(std::int32_t hours);

// The session events one view causes, in order: at most an ENDED and a STARTED.
struct SessionChanges {
    AnvilGameSession items[2]{};
    int count = 0;
};

// Turns views into session events. A session ends when the view stops
// running, shows another session object or another install of it, or changes
// ironman or multiplayer; the next running view starts a new one. A view that
// is not readable changes nothing. Main thread only.
class SessionTracker {
public:
    SessionChanges update(const SessionView& view);
    bool active() const { return active_; }
    // The running session (change = STARTED); meaningful while active().
    const AnvilGameSession& current() const { return current_; }

private:
    bool active_ = false;
    std::uintptr_t session_ = 0;
    std::uint64_t installs_ = 0;
    AnvilGameSession current_{};
    std::uint64_t started_ = 0;
};

}  // namespace anvil_game
