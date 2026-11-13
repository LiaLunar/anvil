// The rules of restart() in anvil-game.game (include/anvil/anvil_game.h, ADR
// 0012), apart from the game: when a request is taken, what the main thread
// writes after a frame, and when a request is dropped. plugin.cpp does the
// writes; the tests drive this with made-up states.
#pragma once

#include <anvil/anvil_plugin.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace anvil_game {

// What anvil-game knows of the game, as of one frame of the main loop.
struct RestartState {
    bool ready = false;
    bool session = false;      // a game session runs
    bool multiplayer = false;  // and it is a multiplayer one
    std::uint64_t number = 0;  // its number (AnvilGameSession.number)
    bool front_end = false;    // the root holds the front end (any of its screens)
    bool main_menu = false;    // and it shows the main menu
};

// The check at the call, with the state of the last frame.
AnvilResult check_restart(const RestartState& last, std::uint32_t flags);

enum class RestartAction {
    None,
    ApplicationFlags,  // write app+0x83/+0x84: the game quits after this frame and starts again
    Resign,            // set the in-game idler's _bResignGame: the game's own Exit to Menu
    ClearResign,       // the game did not act on it: take it back
    Dropped,           // the request is gone without a write
};

struct RestartStep {
    RestartAction action = RestartAction::None;
    std::string caller;
    const char* why = "";  // for the log
};

// A frame count and a time limit, so a request never waits for ever.
constexpr std::uint32_t kRestartResignFrames = 300;  // frames for the game to leave the campaign
constexpr std::uint32_t kRestartMenuFrames = 30;     // frames of the main menu before the flags
constexpr std::uint64_t kRestartMenuMs = 120'000;    // the main menu must come within this after a campaign
constexpr std::uint32_t kRestartWaitFrames = 600;    // a request with neither a session nor a front end

class RestartRequests {
public:
    // Any thread: queues the request, or changes the flags of one that waits.
    void ask(std::uint32_t flags, std::string caller);
    // The main thread, after each outer frame, with that frame's state.
    RestartStep step(const RestartState& now, std::uint64_t now_ms);
    // A request is queued or under way.
    bool busy() const;
    // Forgets the request, whatever its phase.
    void cancel();

private:
    enum class Phase { Idle, Asked, Resigned, AwaitMenu };
    RestartStep finish(RestartAction action, const char* why);

    mutable std::mutex mutex_;
    Phase phase_ = Phase::Idle;
    std::uint32_t flags_ = 0;
    std::string caller_;
    std::uint64_t session_ = 0;
    std::uint32_t frames_ = 0;
    std::uint64_t since_ms_ = 0;
};

}  // namespace anvil_game
