// The gameplay policy of design §13: which plugins are suspended in which
// session, the opt-ins of anvil.toml, the implied dependency on anvil-game,
// and suspension driven by a simulated anvil-game session event.
#include "core/changes.hpp"
#include "core/discovery.hpp"
#include "core/gameplay_policy.hpp"
#include "core/host.hpp"
#include "core/resolver.hpp"
#include "core/services.hpp"
#include "plugin_set_fixture.hpp"

#include <anvil/anvil_game.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace anvil;

Manifest gameplay(const std::string& id, const std::string& extra = "", const std::string& phase = "main") {
    std::string toml = plugin_toml(id, extra, "1.0.0", phase);
    toml.replace(toml.find("affects_gameplay = false"), 24, "affects_gameplay = true");
    return manifest(toml);
}

bool skipped_with(const ResolveResult& r, const std::string& id, const std::string& part) {
    return std::any_of(r.skipped.begin(), r.skipped.end(), [&](const Skipped& s) {
        return s.id == id && s.reason.find(part) != std::string::npos;
    });
}

std::vector<std::string> ids(const ResolveResult& r) {
    std::vector<std::string> out;
    for (const auto& m : r.order) out.push_back(m.id);
    return out;
}

// A scratch "image" with one byte patch per plugin, so suspension is visible.
struct World {
    uint8_t* base = nullptr;
    ChangeRegistry* changes = nullptr;
    ServiceBus bus;
    std::vector<std::string> log;     // "<id>: <message>"
    std::vector<std::string> events;  // "<id>:<event>" as each plugin heard it

    World() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        std::memset(base, 0xCC, 0x1000);
        changes = new ChangeRegistry(reinterpret_cast<uintptr_t>(base), 0x1000);
    }
    ~World() {
        delete changes;
        VirtualFree(base, 0, MEM_RELEASE);
    }

    GameplayPolicy::LogSink sink() {
        return [this](AnvilLogLevel, const std::string& id, const std::string& message) { log.push_back(id + ": " + message); };
    }

    // An owner with a 2-byte patch at `offset`, listening to its own events.
    OwnerId plugin(const std::string& id, size_t offset) {
        const OwnerId owner = changes->add_owner(id);
        const uint8_t patch[2] = {0x90, 0x90};
        const ChangeTarget target{reinterpret_cast<uintptr_t>(base) + offset, true, nullptr, nullptr, 0};
        CHECK(changes->patch_bytes(owner, target, patch, 2) == ChangeStatus::Ok);
        auto* who = new std::pair<World*, std::string>(this, id);  // kept for the test run
        const AnvilEventCallback record = [](const char* event, const void*, void* context) {
            auto* w = static_cast<std::pair<World*, std::string>*>(context);
            w->first->events.push_back(w->second + ":" + event);
        };
        bus.subscribe(id, ANVIL_EVENT_SUSPENDED, record, who);
        bus.subscribe(id, ANVIL_EVENT_RESUMED, record, who);
        return owner;
    }
    bool patched(size_t offset) const { return base[offset] == 0x90; }

    void session(uint32_t change, bool ironman, bool multiplayer) {
        AnvilGameSession s{};
        s.size = sizeof(s);
        s.change = change;
        s.ironman = ironman ? 1 : 0;
        s.multiplayer = multiplayer ? 1 : 0;
        s.number = 1;
        bus.publish(ANVIL_GAME_EVENT_SESSION, &s);
    }
    bool logged(const std::string& part) const {
        return std::any_of(log.begin(), log.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; });
    }
    void dump() const {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
    }
};

}  // namespace

void gameplay_tests() {
    run("gameplay: who is suspended in which session", [] {
        const Manifest plain = manifest(plugin_toml("plain"));
        const Manifest gp = gameplay("gp");
        const Manifest both = gameplay("both", "allow_ironman = \"opt-in\"\nallow_multiplayer = \"opt-in\"\n");
        const GameplayOptIns none;
        const GameplayOptIns ironman_only{{"both"}, {}};
        const GameplayOptIns all{{"both", "gp"}, {"both"}};
        const SessionMode sp, im{true, false}, mp{false, true}, im_mp{true, true};
        // Plugins that do not affect gameplay, and single-player sessions, never suspend anything.
        for (SessionMode m : {sp, im, mp, im_mp}) CHECK(suspension_reason(plain, m, none).empty());
        CHECK(suspension_reason(gp, sp, none).empty() && suspension_reason(both, sp, none).empty());
        // Without an opt-in in plugin.toml the player cannot allow it.
        CHECK(suspension_reason(gp, im, all) == "affects gameplay and its plugin.toml does not offer allow_ironman = "
                                                "\"opt-in\" (ironman)");
        CHECK(suspension_reason(gp, mp, all).find("allow_multiplayer = \"opt-in\"") != std::string::npos);
        // Offered, but the player must list it, per mode.
        CHECK(suspension_reason(both, im, none) ==
              "affects gameplay and is not allowed in ironman (add it to allow_ironman in anvil.toml)");
        CHECK(suspension_reason(both, im, ironman_only).empty());
        CHECK(suspension_reason(both, mp, ironman_only) ==
              "affects gameplay and is not allowed in multiplayer (add it to allow_multiplayer in anvil.toml)");
        // Ironman multiplayer needs both.
        CHECK(!suspension_reason(both, im_mp, ironman_only).empty());
        CHECK(suspension_reason(both, im_mp, all).empty());
    });

    run("gameplay: opt-ins that do nothing are reported", [] {
        const GameplayOptIns opt{{"plain", "gp", "both"}, {"both"}};
        CHECK(opt_in_problems(manifest(plugin_toml("plain")), opt).size() == 1);
        const auto gp = opt_in_problems(gameplay("gp"), opt);
        CHECK(gp.size() == 1 && gp[0] == "allow_ironman in anvil.toml lists it, but its plugin.toml does not offer "
                                         "allow_ironman = \"opt-in\": it stays suspended in ironman");
        CHECK(opt_in_problems(gameplay("both", "allow_ironman = \"opt-in\"\nallow_multiplayer = \"opt-in\"\n"), opt).empty());
        CHECK(opt_in_problems(gameplay("other"), opt).empty());
    });

    run("gameplay: anvil.toml allow_ironman and allow_multiplayer", [] {
        const auto file = std::filesystem::temp_directory_path() / "anvil_gameplay_test.toml";
        std::ofstream(file) << "allow_ironman = [\"a\", \"b\"]\nallow_multiplayer = [\"b\"]\n";
        const AnvilSettings s = read_anvil_settings(file);
        CHECK(s.error.empty() && (s.allow_ironman == std::vector<std::string>{"a", "b"}) &&
              (s.allow_multiplayer == std::vector<std::string>{"b"}));
        std::filesystem::remove(file);
        const AnvilSettings defaults = read_anvil_settings(file);
        CHECK(defaults.allow_ironman.empty() && defaults.allow_multiplayer.empty());
    });

    run("gameplay: a plugin that affects gameplay needs anvil-game", [] {
        ResolveInput in;
        in.host_api = kManifestApi;
        in.manifests = {gameplay("gp"), manifest(plugin_toml("plain"))};
        ResolveResult r = resolve(in);
        CHECK(ids(r) == std::vector<std::string>{"plain"});
        CHECK(skipped_with(r, "gp", "affects gameplay, so it needs plugin 'anvil-game', which is not loaded"));

        // With anvil-game (early, as shipped) it loads after it, also an early gameplay plugin.
        in.manifests = {gameplay("gp"), gameplay("early-gp", "", "early"),
                        manifest(plugin_toml(ANVIL_GAME_ID, "", "1.0.0", "early"))};
        r = resolve(in);
        CHECK(r.skipped.empty());
        CHECK((ids(r) == std::vector<std::string>{"anvil-game", "early-gp", "gp"}));
        const auto& deps = r.order[2].dependencies;
        CHECK(deps.size() == 1 && deps[0].id == ANVIL_GAME_ID && deps[0].implied && !deps[0].optional);

        // The player disabling anvil-game skips the gameplay plugins too.
        in.disabled = {ANVIL_GAME_ID};
        r = resolve(in);
        CHECK(r.order.empty() && skipped_with(r, "gp", "needs plugin 'anvil-game'"));

        // A declared dependency on anvil-game is kept as written, not doubled.
        in.disabled.clear();
        in.manifests = {gameplay("gp", "[[dependencies]]\nid = \"anvil-game\"\nversion = \"^2\"\n"),
                        manifest(plugin_toml(ANVIL_GAME_ID))};
        r = resolve(in);
        CHECK(skipped_with(r, "gp", "requires 'anvil-game' ^2, found 1.0.0"));
    });

    run("gameplay: a simulated session suspends and resumes gameplay plugins", [] {
        World w;
        const Manifest gp_m = gameplay("gp");
        const Manifest optin_m = gameplay("optin", "allow_ironman = \"opt-in\"\n");
        const Manifest plain_m = manifest(plugin_toml("plain"));
        const OwnerId gp = w.plugin("gp", 0x100), optin = w.plugin("optin", 0x200), plain = w.plugin("plain", 0x300);
        GameplayPolicy policy(*w.changes, w.bus, GameplayOptIns{{"optin"}, {}}, w.sink());
        policy.subscribe();
        policy.add(gp_m, gp);
        policy.add(optin_m, optin);
        policy.add(plain_m, plain);

        // Single player: nothing changes.
        w.session(ANVIL_GAME_SESSION_STARTED, false, false);
        CHECK(w.patched(0x100) && w.patched(0x200) && w.patched(0x300) && w.events.empty());
        CHECK(w.logged("anvil: game session started (neither ironman nor multiplayer): 0 gameplay plugin(s) or module(s) suspended"));
        w.session(ANVIL_GAME_SESSION_ENDED, false, false);

        // Ironman: gp out, optin allowed by the player, plain untouched.
        w.session(ANVIL_GAME_SESSION_STARTED, true, false);
        CHECK(!w.patched(0x100) && w.patched(0x200) && w.patched(0x300));
        CHECK(policy.suspended(gp) && !policy.suspended(optin) && w.changes->suspended(gp));
        CHECK(w.events == std::vector<std::string>{"gp:anvil.suspended"});
        CHECK(w.logged("gp: suspended: affects gameplay and its plugin.toml does not offer allow_ironman = \"opt-in\" "
                       "(ironman); 1 change(s) out of the game"));
        CHECK(w.logged("optin: stays active: the player allows it in ironman"));
        CHECK(w.logged("anvil: game session started (ironman): 1 gameplay plugin(s) or module(s) suspended, 1 allowed by the player"));
        w.session(ANVIL_GAME_SESSION_ENDED, true, false);
        CHECK(w.patched(0x100) && !w.changes->suspended(gp) && !policy.session());
        CHECK((w.events == std::vector<std::string>{"gp:anvil.suspended", "gp:anvil.resumed"}));
        CHECK(w.logged("gp: resumed: the session ended; 1 change(s) back"));

        // Ironman multiplayer: optin is allowed in ironman only, so it goes too.
        w.events.clear();
        w.session(ANVIL_GAME_SESSION_STARTED, true, true);
        CHECK(!w.patched(0x100) && !w.patched(0x200) && w.patched(0x300));
        CHECK((w.events == std::vector<std::string>{"gp:anvil.suspended", "optin:anvil.suspended"}));
        CHECK(w.logged("optin: suspended: affects gameplay and its plugin.toml does not offer allow_multiplayer"));
        // A second start without an end (another save): optin stays out, no event twice.
        w.session(ANVIL_GAME_SESSION_STARTED, true, true);
        CHECK(w.events.size() == 2);
        w.session(ANVIL_GAME_SESSION_ENDED, true, true);
        CHECK(w.patched(0x100) && w.patched(0x200) && w.events.size() == 4);
        // An end without a session does nothing.
        w.session(ANVIL_GAME_SESSION_ENDED, true, true);
        CHECK(w.events.size() == 4);
        if (g_failures != 0) w.dump();
    });

    run("gameplay: a plugin added during a session starts suspended; a failed one is left alone", [] {
        World w;
        GameplayPolicy policy(*w.changes, w.bus, {}, w.sink());
        policy.subscribe();
        const OwnerId failed = w.changes->add_owner("failed");
        policy.add(gameplay("failed"), failed);
        policy.forget(failed);
        w.session(ANVIL_GAME_SESSION_STARTED, false, true);
        CHECK(!policy.suspended(failed) && !w.changes->suspended(failed));

        const OwnerId late = w.changes->add_owner("late");
        policy.add(gameplay("late"), late);
        CHECK(policy.suspended(late) && w.changes->suspended(late));
        // Its changes are enabled suspended (the load would request them now).
        const uint8_t patch[2] = {0x90, 0x90};
        const ChangeTarget target{reinterpret_cast<uintptr_t>(w.base) + 0x400, true, nullptr, nullptr, 0};
        CHECK(w.changes->patch_bytes(late, target, patch, 2) == ChangeStatus::Ok);
        CHECK(!w.patched(0x400) && w.changes->suspended_count(late) == 1);
        w.session(ANVIL_GAME_SESSION_ENDED, false, true);
        CHECK(w.patched(0x400) && !policy.suspended(late));
    });

    run("gameplay: a session event without a valid payload is ignored", [] {
        World w;
        GameplayPolicy policy(*w.changes, w.bus, {}, w.sink());
        policy.subscribe();
        w.bus.publish(ANVIL_GAME_EVENT_SESSION, nullptr);
        AnvilGameSession small{};
        small.size = 4;
        small.change = ANVIL_GAME_SESSION_STARTED;
        w.bus.publish(ANVIL_GAME_EVENT_SESSION, &small);
        AnvilGameSession odd{};
        odd.size = sizeof(odd);
        odd.change = 7;
        w.bus.publish(ANVIL_GAME_EVENT_SESSION, &odd);
        CHECK(!policy.session());
        CHECK(w.logged("anvil: anvil-game.session without a valid payload: ignored"));
        CHECK(w.logged("anvil: anvil-game.session with unknown change 7: ignored"));
    });

    run("gameplay: modules are suspended by their own flags (ADR 0011)", [] {
        World w;  // its scratch image; the start's registry patches it
        TestStart start(reinterpret_cast<uintptr_t>(w.base), 0x1000, nullptr, GameplayOptIns{{"smithy.drill_all"}, {}});
        anvil::ResolvedPlugin& smithy = start.add(plugin_toml("smithy", R"([[modules]]
id = "ui_fix"
name = "UI fix"
affects_gameplay = false

[[modules]]
id = "drill_all"
name = "Drill"
affects_gameplay = true
allow_ironman = "opt-in"
allow_multiplayer = "opt-in"

[[modules]]
id = "chase"
name = "Chase"
affects_gameplay = true

[[modules]]
id = "explore"
name = "Explore"
affects_gameplay = true
default = false
)"), L"C:\\plugins\\smithy");
        AnvilSettings player;
        player.disabled_modules = {"smithy.chase", "other.ui_fix"};
        for (const Module& m : smithy.manifest.modules) {
            smithy.modules_on.push_back(module_enabled(player, module_key(smithy.manifest, m), m.enabled_by_default));
        }
        start.make();
        AnvilPlugin& plugin = *start.handle("smithy");
        CHECK(start.plugin("smithy").modules().size() == 4);
        AnvilPlugin* const ui_handle = start.module("smithy", "smithy.ui_fix");
        AnvilPlugin* const drill_handle = start.module("smithy", "smithy.drill_all");
        CHECK(ui_handle != nullptr && drill_handle != nullptr);
        if (ui_handle == nullptr || drill_handle == nullptr) return;
        CHECK(start.changes.owner_name(drill_handle->owner) == "smithy.drill_all");
        // Off at this start: no handle, no owner.
        CHECK(start.module("smithy", "smithy.chase") == nullptr && start.module("smithy", "smithy.explore") == nullptr);
        CHECK(start.logged("anvil: smithy.chase: module off at this start (anvil.toml)"));
        player.enabled_modules = {"smithy.explore", "smithy.chase"};
        CHECK(module_enabled(player, "smithy.explore", false) && !module_enabled(player, "smithy.chase", true));

        const AnvilHost host = make_host(nullptr);
        CHECK(ANVIL_HOST_HAS(&host, module));
        AnvilPlugin *ui = nullptr, *drill = nullptr, *chase = &plugin, *none = &plugin;
        CHECK(host.module(&plugin, "ui_fix", &ui) == ANVIL_OK && ui == ui_handle);
        CHECK(host.module(&plugin, "drill_all", &drill) == ANVIL_OK && drill == drill_handle);
        CHECK(host.module(&plugin, "chase", &chase) == ANVIL_ERROR_DISABLED && chase == nullptr);
        CHECK(host.module(&plugin, "nope", &none) == ANVIL_ERROR_NOT_DECLARED && none == nullptr);
        CHECK(host.module(drill, "ui_fix", &none) == ANVIL_ERROR_NOT_DECLARED);
        CHECK(host.module(&plugin, "ui_fix", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
        // A module shares the plugin's folders and names, and logs under its key.
        CHECK(std::wstring(host.plugin_dir(drill)) == L"C:\\plugins\\smithy");
        host.log(drill, ANVIL_LOG_INFO, "hello");
        CHECK(!start.log.empty() && start.log.back() == "smithy.drill_all: hello");
        static const uint32_t service[2] = {sizeof(service), 0};  // a table starts with its size
        CHECK(host.register_service(drill, "smithy.api", 1, service) == ANVIL_OK);  // the plugin's name, not the module's
        CHECK(host.publish(drill, "smithy.ready", nullptr) == ANVIL_OK);
        CHECK(host.publish(drill, "smithy.drill_all.ready", nullptr) == ANVIL_OK);  // still the plugin's prefix
        CHECK(host.publish(drill, "other.ready", nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);

        // One change per handle; each belongs to its owner.
        ChangeRegistry& changes = start.changes;
        const uint8_t patch[2] = {0x90, 0x90};
        const auto at = [&](size_t offset) {
            return ChangeTarget{reinterpret_cast<uintptr_t>(w.base) + offset, true, nullptr, nullptr, 0};
        };
        CHECK(changes.patch_bytes(plugin.owner, at(0x100), patch, 2) == ChangeStatus::Ok);
        CHECK(changes.patch_bytes(ui->owner, at(0x200), patch, 2) == ChangeStatus::Ok);
        CHECK(changes.patch_bytes(drill->owner, at(0x300), patch, 2) == ChangeStatus::Ok);
        std::vector<std::string> heard;
        const AnvilEventCallback record = [](const char* event, const void*, void* context) {
            static_cast<std::vector<std::string>*>(context)->push_back(event);
        };
        CHECK(host.subscribe(drill, ANVIL_EVENT_SUSPENDED, record, &heard) == ANVIL_OK);
        CHECK(host.subscribe(drill, ANVIL_EVENT_RESUMED, record, &heard) == ANVIL_OK);
        std::vector<std::string> plugin_heard;
        CHECK(host.subscribe(&plugin, ANVIL_EVENT_SUSPENDED, record, &plugin_heard) == ANVIL_OK);

        // The set gave the policy the plugin and its modules that are on.
        const auto session = [&](uint32_t change, bool ironman, bool multiplayer) {
            AnvilGameSession s{};
            s.size = sizeof(s);
            s.change = change;
            s.ironman = ironman ? 1 : 0;
            s.multiplayer = multiplayer ? 1 : 0;
            s.number = 1;
            start.bus.publish(ANVIL_GAME_EVENT_SESSION, &s);
        };
        // Ironman: drill_all is allowed by the player, the interface module never suspends.
        session(ANVIL_GAME_SESSION_STARTED, true, false);
        CHECK(w.patched(0x100) && w.patched(0x200) && w.patched(0x300) && heard.empty());
        CHECK(start.logged_containing("smithy.drill_all: stays active: the player allows it in ironman") == 1);
        session(ANVIL_GAME_SESSION_ENDED, true, false);
        // Multiplayer without its opt-in: only the module goes, and only it hears so.
        session(ANVIL_GAME_SESSION_STARTED, false, true);
        CHECK(w.patched(0x100) && w.patched(0x200) && !w.patched(0x300));
        CHECK(start.policy.suspended(drill->owner) && !start.policy.suspended(plugin.owner) &&
              !start.policy.suspended(ui->owner));
        CHECK((heard == std::vector<std::string>{ANVIL_EVENT_SUSPENDED}) && plugin_heard.empty());
        CHECK(start.logged_containing("smithy.drill_all: suspended: affects gameplay and is not allowed in multiplayer (add "
                                      "it to allow_multiplayer in anvil.toml); 1 change(s) out of the game") == 1);
        session(ANVIL_GAME_SESSION_ENDED, false, true);
        CHECK(w.patched(0x300) && (heard == std::vector<std::string>{ANVIL_EVENT_SUSPENDED, ANVIL_EVENT_RESUMED}));

        // The wrappers remove with the plugin's handle: a module's change goes too.
        ChangeId id = 0;
        CHECK(changes.patch_bytes(drill->owner, at(0x400), patch, 2, &id) == ChangeStatus::Ok);
        auto* handle = reinterpret_cast<AnvilChange*>(static_cast<uintptr_t>(id));
        CHECK(host.remove(&plugin, handle) == ANVIL_OK && !w.patched(0x400) && changes.count(drill->owner) == 1);
        CHECK(host.remove(&plugin, handle) == ANVIL_ERROR_NOT_FOUND);
        CHECK(changes.patch_bytes(plugin.owner, at(0x500), patch, 2, &id) == ChangeStatus::Ok);
        CHECK(host.remove(ui, reinterpret_cast<AnvilChange*>(static_cast<uintptr_t>(id))) == ANVIL_OK);
        if (g_failures != 0) start.dump();
    });

    run("gameplay: a module that affects gameplay needs anvil-game (ADR 0011)", [] {
        ResolveInput input;
        input.host_api = kManifestApi;
        input.manifests = {manifest(plugin_toml("smithy", "[[modules]]\nid = \"drill_all\"\nname = \"Drill\"\n"
                                                          "affects_gameplay = true\n"))};
        const ResolveResult r = resolve(input);
        CHECK(skipped_with(r, "smithy", "anvil-game"));
        input.manifests = {manifest(plugin_toml("smithy", "[[modules]]\nid = \"ui_fix\"\nname = \"UI\"\n"
                                                          "affects_gameplay = false\n"))};
        CHECK(ids(resolve(input)) == std::vector<std::string>{"smithy"});
    });
}
