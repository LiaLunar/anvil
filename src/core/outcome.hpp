#pragma once

#include "i18n/message.hpp"
#include "resolver.hpp"

#include <string>

namespace anvil {

// What became of a plugin at one start (GLOSSARY: Outcome). A plugin whose
// phase has not run has none yet; a module has none of its own, it shares its
// plugin's.
struct Outcome {
    enum class Kind { loaded, failed, skipped, disabled };
    Kind kind = Kind::loaded;
    i18n::Message why;   // failed, skipped and disabled: the reason by code
    std::string reason;  // its English words, for the log and anvil.control
    SkipKind skip = SkipKind::other;  // skipped: the kind the benchmark record names
    bool by_benchmark_arm = false;    // disabled: by the arm of a benchmark start, not by the player
};

// The words the log, the crash report and the benchmark record use:
// "loaded", "failed", "skipped", "disabled", and "pending" for no outcome yet.
const char* outcome_name(Outcome::Kind kind);
const char* outcome_name(const Outcome* outcome);

}  // namespace anvil
