// Example: a script effect of the plugin's own (ADR 0008, anvil_script.h).
// A mod writes, in an event option or a decision:
//
//   capital_scope = { hello_script_log = "the capital" }
//
// and the game runs `execute` with the value and the scope, on its own
// thread, inside the command that runs the option. Without the plugin the
// game logs `Unknown effect type. Key: hello_script_log` and skips only that
// line. The plugin has no symbols.toml: anvil-script knows the game.
#include <anvil/anvil.hpp>
#include <anvil/anvil_script.h>

#include <cstdio>

namespace {

void execute(void*, const AnvilScriptCall* call) noexcept {
    anvil::Host{}.info("{} = {} in province {} of '{}'", call->key, call->value, call->scope.province,
                       call->scope.country);
}

// The tooltip line the game shows for the effect, e.g. in the option's tooltip.
uint32_t describe(void*, const AnvilScriptCall* call, char* utf8, uint32_t capacity) noexcept {
    const int n = std::snprintf(utf8, capacity, "Writes \"%s\" to anvil.log", call->value);
    return n > 0 ? static_cast<uint32_t>(n) : 0;
}

struct HelloScript : anvil::Plugin {
    void load(anvil::Host& host) override {
        const auto* script =
            host.service<AnvilScriptEffectsService>(ANVIL_SCRIPT_EFFECTS_SERVICE, ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION);
        if (script == nullptr) throw anvil::Error(ANVIL_ERROR_NOT_FOUND, "no anvil-script.effects service");
        static const AnvilScriptEffect log{sizeof(AnvilScriptEffect), 0, "hello_script_log", execute, describe, nullptr};
        const AnvilResult r = script->register_effect(host.self(), &log);
        if (r != ANVIL_OK) throw anvil::Error(r, "register_effect hello_script_log");
        host.info("hello_script_log is a script effect now");
    }
};

}  // namespace

ANVIL_PLUGIN(HelloScript)
