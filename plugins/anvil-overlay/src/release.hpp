// The overlay's release check (design §15, ADR 0005): whether to ask GitHub,
// the question, and the notice of a newer Anvil. Pure functions, tested
// without a game or the network; ui.cpp runs the check on a worker thread.
#pragma once

#include "net/release_check.hpp"
#include "notices.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace overlay {

// The options of the `release_check` setting, in settings.toml's order.
enum class ReleaseConsent { Ask, On, Off };

enum class ReleasePlan {
    Check,     // ask GitHub once, in the background
    Question,  // ask the player first (the "release-question" notice)
    None,
};

// On and Off are the player's word. Ask follows the player's answer in Anvil
// Manager (anvil::net::manager_release_answer), and asks in the game when
// there is none. A benchmark start never checks.
ReleasePlan release_plan(ReleaseConsent consent, std::optional<bool> manager_answer, bool benchmark);

// "Should Anvil ask GitHub...?" with Yes and No; it stays until answered or closed.
Notice release_question();

// The notice of `latest` when it is newer than `running` (the core's Anvil
// version) and the player did not close the notice of that tag before
// (`dismissed`): its version, how to update, a link to its page.
std::optional<Notice> release_notice(const anvil::net::Release& latest, std::string_view running,
                                     std::string_view dismissed);

// The first line of the file that remembers the closed notice, trimmed; ""
// when there is none.
std::string dismissed_tag(std::string_view file_text);

}  // namespace overlay
