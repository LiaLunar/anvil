// Test plugin for tests/plugin_tests.cpp: every kind of change through the C++
// wrapper, and exports the test calls to use anvil::Host after load.
#include <anvil/anvil.hpp>

#include "symbols.gen.hpp"

#include <stdexcept>

namespace {

anvil::HookOf<sym::add_one> g_add;
int on_add(int x) noexcept { return g_add.next(x) + 100; }

anvil::PointerSwap<int(int)> g_tick;
int on_tick(int x) noexcept { return g_tick.previous()(x) * 10; }

struct Probe : anvil::Plugin {
    anvil::Change adjust, triple, thrower, patch;
    int adjusted = 0;

    void load(anvil::Host& host) override {
        if (!host.present(sym::twice)) throw anvil::Error(ANVIL_ERROR_NOT_DECLARED, "twice is not present");
        host.info("twice(21) = {}", host.call(sym::twice, 21));
        *host.data(sym::counter) = 7;
        host.info("counter {} through {}", host.call(sym::load_counter),
                  host.data(sym::counter_ref) == host.data(sym::counter) ? "the same address" : "another address");

        host.hook(g_add, sym::add_one, on_add);
        adjust = host.mid_hook(sym::adjust, [this](anvil::Cpu& cpu) {
            cpu.rcx += 1000;
            ++adjusted;
        });
        triple = host.replace_block(sym::triple, [](anvil::Cpu& cpu) { cpu.rax = cpu.rcx * 3; });
        thrower = host.mid_hook(sym::thrower, [](anvil::Cpu& cpu) {
            if (cpu.rcx == 13) throw std::runtime_error("unlucky thirteen");
            cpu.rcx += 1;
        });
        host.swap(g_tick, sym::tick_slot, on_tick);
        host.subscribe(ANVIL_EVENT_PLUGINS_LOADED, [host] { host.info("plugins loaded"); });

        // Refusals are anvil::Error with the host's result.
        try {
            patch = host.patch(sym::twice, {0x90, 0x90, 0x90, 0x90, 0x90});  // longer than the declared bytes
        } catch (const anvil::Error& e) {
            host.info("{} ({})", e.what(), static_cast<int>(e.result()));
        }
        try {
            host.group([] {});
        } catch (const anvil::Error& e) {
            host.info("{}", e.what());
        }
    }
};

Probe* g_probe = nullptr;

struct Registered : Probe {
    Registered() { g_probe = this; }
};

}  // namespace

ANVIL_PLUGIN(Registered)

// After load, from the test: anvil::Host{} and Change handles outside load().
extern "C" __declspec(dllexport) int probe_adjusted() { return g_probe->adjusted; }

extern "C" __declspec(dllexport) void probe_remove_adjust() { g_probe->adjust.reset(); }

extern "C" __declspec(dllexport) void probe_remove_add() { g_add.remove(); }

// The hook result of hooking add_one again with the same HookOf.
extern "C" __declspec(dllexport) int probe_hook_again() {
    try {
        anvil::Host{}.hook(g_add, sym::add_one, on_add);
        return ANVIL_OK;
    } catch (const anvil::Error& e) {
        return e.result();
    }
}

// 0: patched in a group; 1: the group threw and was aborted; 2: other error.
extern "C" __declspec(dllexport) int probe_patch_in_group(int fail) {
    try {
        anvil::Host{}.group([&] {
            g_probe->patch = anvil::Host{}.patch(sym::patch_site, {0xB8, 0x02});
            if (fail != 0) throw std::runtime_error("abort the group");
        });
        return 0;
    } catch (const std::runtime_error&) {
        return 1;
    } catch (...) {
        return 2;
    }
}
