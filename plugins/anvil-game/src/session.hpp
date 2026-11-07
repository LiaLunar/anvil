// Game sessions as anvil-game sees them, one view per frame of the main loop.
#pragma once

#include <anvil/anvil_game.h>

#include <cstdint>
#include <string>
#include <vector>

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
    std::uint32_t oos = 0;  // ANVIL_GAME_OOS_*
};

// 1 January of year 1: the date of a root without a running game, or of a
// session whose save is still being read into it.
constexpr std::int32_t kUnsetDate = 43'800'000 + 365 * 24;

// The game's date: hours since 1 January of year -5000 in 365-day years (no
// leap days), as Tongs reads it for tongs_state. `session` is left 0.
AnvilGameDate decode_date(std::int32_t hours);

// What one frame's date gives: the events to publish, and their payload.
struct DateEvents {
    bool day = false;
    bool month = false;
    bool year = false;
    AnvilGameDateChange change{};
};

// Compares the date frame by frame. The first date of a session is the
// baseline, a session change starts a new one, and a date that goes back only
// moves the baseline. Main thread only.
class DateTracker {
public:
    // `running`: a session runs; `session`: its number; `hours`: the root's date.
    DateEvents update(bool running, std::uint64_t session, std::int32_t hours);

private:
    bool known_ = false;
    std::uint64_t session_ = 0;
    std::int32_t hours_ = 0;
};

// Compares the out-of-sync flags frame by frame. Each session starts from 0;
// without a session the flags are 0 and nothing changes. Main thread only.
class OosTracker {
public:
    struct Change {
        bool changed = false;
        AnvilGameOosChange event{};
    };
    Change update(bool running, std::uint64_t session, std::uint32_t flags);
    std::uint32_t flags() const { return flags_; }

private:
    std::uint64_t session_ = 0;
    std::uint32_t flags_ = 0;
};

// A running war as the tracker keeps it, for the event when it ends.
struct WarSeen {
    std::uint64_t id = 0;
    AnvilGameTag attacker{};
    AnvilGameTag defender{};
    std::string name;
};

// Compares the running wars day by day. The first list of a session is the
// baseline; a war is reported once when it appears and once when it goes.
class WarTracker {
public:
    struct Changes {
        std::vector<WarSeen> started;
        std::vector<WarSeen> ended;
    };
    // `seen` is the running wars; a session that ended forgets the baseline.
    Changes update(std::uint64_t session, std::vector<WarSeen> seen);
    // Whether this session has its baseline.
    bool known(std::uint64_t session) const { return known_ && session == session_; }
    // Whether the tracker already knows this war, so its details need no reading.
    const WarSeen* find(std::uint64_t session, std::uint64_t id) const;

private:
    bool known_ = false;
    std::uint64_t session_ = 0;
    std::vector<WarSeen> wars_;
};

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
