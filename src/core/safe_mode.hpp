#pragma once

#include "crash_history.hpp"

#include "i18n/message.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// Safe mode (design §10, §12): the start after two consecutive starts that
// ended before the game was ready (anvil-game.ready) loads no plugins, except
// the standard ones the overlay needs to explain it.
//
// The record is one number in anvil/data/anvil/unfinished-starts.txt: how many
// starts in a row ended before the game was ready or wrote a crash report
// before it. Each start counts itself in at the beginning; readiness without
// a crash report removes the file; a clean exit before it, without a crash
// (crash::crashed), puts the previous number back. So a start that crashed,
// was killed or hung before the game was ready stays counted, whatever caught
// the crash, and so does one that crashed on another thread while the game
// went on to the menu. The safe start removes the file: the next one loads
// the plugins again.
inline constexpr unsigned kSafeModeAfter = 2;

// The standard plugins that still load in safe mode: anvil-game and
// anvil-overlay, so the overlay can explain it (design §10, #58).
bool loads_in_safe_mode(std::string_view id);

// Why a standard plugin that loads in safe mode is left out of this safe
// start: one of `crashes` (the reports of the starts that led to it) was in
// the plugin's own code or in its change, so loading it again could end this
// start the same way. Empty when it loads.
i18n::Message safe_mode_exclusion(std::string_view id, const std::vector<CrashSummary>& crashes);

// The reports of the starts that led to a safe start: of the newest reports
// in `crash_dir`, those written before the game was ready, at most
// `unfinished` of them. A start that hung or was killed wrote none, so an
// older report may stand in for it.
std::vector<CrashSummary> safe_mode_crashes(const std::filesystem::path& crash_dir, unsigned unfinished);

struct StartRecord {
    unsigned unfinished = 0;  // starts before this one that ended before game_ready
    bool safe_mode = false;
};

class StartMarker {
public:
    // Reads the record and counts this start in (or, in safe mode, clears it).
    StartRecord begin(const std::filesystem::path& file);
    // The game is ready: this start is not counted, unless a crash report
    // was written before (crash::reported); then it stays counted.
    void game_ready(bool crash_reported);
    // The process exits without a crash before game_ready: the record goes
    // back to what it was before this start.
    void clean_exit();
    bool active() const { return active_; }

private:
    std::wstring file_;
    unsigned previous_ = 0;
    bool active_ = false;  // counted in, not yet ready or exited
};

}  // namespace anvil
