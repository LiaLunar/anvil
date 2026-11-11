// The script errors panel of anvil-log-throttle (docs/script-errors-panel.md),
// drawn by anvil-overlay.panel. Off by default (the `panel` setting): then it
// creates no panel and starts no thread, and the table is not kept.
#pragma once

#include <anvil/anvil.hpp>

#include "tally.hpp"

namespace log_throttle::panel {

// At the plugin's load: the overlay's panel service, if there is one, and the
// setting's value. On: the table starts now and the panel opens; once the
// player closes it, its switch is in the Status tab of the Anvil panel.
void load(const anvil::Host& host, Tally& tally, bool on);

// The setting changed (any thread). On: the table starts, if it has not, and
// the panel shows; off: it hides and is not refreshed.
void set_on(bool on);

}  // namespace log_throttle::panel
