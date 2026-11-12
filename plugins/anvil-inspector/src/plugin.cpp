// anvil-inspector: the standard plugin of the inspector for modders
// (docs/inspector.md; eu4-engine-lab#65, #155). A window of the overlay with
// the flags, variables and timed modifiers of the province whose view is open,
// of its owner and of the player's country, read through anvil-game.world. It
// has no game addresses and only reads.
#include <anvil/anvil.hpp>

#include "settings.gen.hpp"

#include "panel.hpp"

namespace {

struct Inspector : anvil::Plugin {
    void load(anvil::Host& host) override {
        anvil_inspector::panel::load(host, cfg::window.get());
        host.subscribe_setting(cfg::window, [](bool on) { anvil_inspector::panel::set_on(on); });
    }
};

}  // namespace

ANVIL_PLUGIN(Inspector)
