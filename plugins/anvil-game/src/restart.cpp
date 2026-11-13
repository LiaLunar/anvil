#include "restart.hpp"

#include <anvil/anvil_game.h>

#include <utility>

namespace anvil_game {

AnvilResult check_restart(const RestartState& last, std::uint32_t flags) {
    if ((flags & ~ANVIL_GAME_RESTART_CLOSE_CAMPAIGN) != 0) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!last.ready) return ANVIL_ERROR_WRONG_PHASE;
    if (last.session && (last.multiplayer || (flags & ANVIL_GAME_RESTART_CLOSE_CAMPAIGN) == 0)) {
        return ANVIL_ERROR_WRONG_PHASE;
    }
    return ANVIL_OK;
}

void RestartRequests::ask(std::uint32_t flags, std::string caller) {
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::Idle) {
        phase_ = Phase::Asked;
        frames_ = 0;
        caller_ = std::move(caller);
    }
    // One under way keeps its course; only a waiting one takes the new flags.
    if (phase_ == Phase::Asked) flags_ = flags;
}

bool RestartRequests::busy() const {
    std::lock_guard lock(mutex_);
    return phase_ != Phase::Idle;
}

void RestartRequests::cancel() {
    std::lock_guard lock(mutex_);
    finish(RestartAction::Dropped, "");
}

RestartStep RestartRequests::finish(RestartAction action, const char* why) {
    RestartStep step{action, std::move(caller_), why};
    phase_ = Phase::Idle;
    caller_.clear();
    flags_ = 0;
    frames_ = 0;
    return step;
}

RestartStep RestartRequests::step(const RestartState& now, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    switch (phase_) {
    case Phase::Idle:
        return {};
    case Phase::Asked:
        // The call checked the last frame; this is the frame it acts in.
        if (!now.ready) return finish(RestartAction::Dropped, "the game is not ready");
        if (now.session) {
            if (now.multiplayer) return finish(RestartAction::Dropped, "a multiplayer session runs");
            if ((flags_ & ANVIL_GAME_RESTART_CLOSE_CAMPAIGN) == 0) {
                return finish(RestartAction::Dropped, "a campaign runs, and closing it was not asked for");
            }
            phase_ = Phase::Resigned;
            session_ = now.number;
            frames_ = 0;
            return {RestartAction::Resign, caller_, "the game's Exit to Menu"};
        }
        if (now.front_end) return finish(RestartAction::ApplicationFlags, "no session");
        if (++frames_ > kRestartWaitFrames) return finish(RestartAction::Dropped, "neither a session nor the front end");
        return {};
    case Phase::Resigned:
        if (now.session) {
            if (now.number != session_) return finish(RestartAction::Dropped, "another session started");
            if (++frames_ > kRestartResignFrames) {
                return finish(RestartAction::ClearResign, "the game did not leave the campaign");
            }
            return {};
        }
        // Without a plugin that returns to the menu the game quits now and
        // no frame comes; with one, the restart waits for its main menu.
        phase_ = Phase::AwaitMenu;
        since_ms_ = now_ms;
        frames_ = 0;
        [[fallthrough]];
    case Phase::AwaitMenu:
        if (now.session) return finish(RestartAction::Dropped, "a session started before the main menu");
        if (now_ms - since_ms_ > kRestartMenuMs) return finish(RestartAction::Dropped, "no main menu after the campaign");
        if (!now.main_menu) {
            frames_ = 0;
            return {};
        }
        if (++frames_ < kRestartMenuFrames) return {};
        return finish(RestartAction::ApplicationFlags, "the main menu after the campaign");
    }
    return {};
}

}  // namespace anvil_game
