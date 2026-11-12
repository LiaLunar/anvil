// The inspector's window, drawn by anvil-overlay.panel. Off by default (the
// `window` setting): then it creates no panel, starts no thread and asks the
// game for nothing.
#pragma once

#include <anvil/anvil.hpp>

namespace anvil_inspector::panel {

// At the plugin's load: the services, and the setting's value. On: the window
// opens; once the player closes it, its switch is in the Status tab of the
// Anvil panel.
void load(const anvil::Host& host, bool on);

// The setting changed (any thread). On: the window shows; off: it hides and
// nothing more is read.
void set_on(bool on);

}  // namespace anvil_inspector::panel
