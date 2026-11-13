// The plugin set (src/core/plugin_set.hpp) through its interface: the order
// of owners, modules that are off, what each way a load fails leaves, and the
// one list that anvil.control and the benchmark's start record read.
#include "core/benchmark_record.hpp"
#include "core/control.hpp"
#include "plugin_set_fixture.hpp"

#include <anvil/anvil_game.h>
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

// int f(int x) { return x + 1; } with room for a hook: lea eax, [rcx+1]; nops; ret
const uint8_t kAddOne[] = {0x8D, 0x41, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
using Fn = int (*)(int);

// A page standing in for eu4.exe and, right after it, outside the image, a
// function to hook by address: the registry's stubs must reach the image.
struct Code {
    uint8_t* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    uint8_t* base = image + 0x1000;
    Code() {
        std::memcpy(base, kAddOne, sizeof(kAddOne));
        FlushInstructionCache(GetCurrentProcess(), base, sizeof(kAddOne));
    }
    ~Code() { VirtualFree(image, 0, MEM_RELEASE); }
    uintptr_t image_base() const { return reinterpret_cast<uintptr_t>(image); }
    Fn fn() const { return reinterpret_cast<Fn>(base); }
};

// This executable as a plugin's entry DLL: its load stages a hook through the
// plugin's own handle and one through its module "extra", then fails.
Code* g_failing_target = nullptr;
void* g_failing_next[2] = {};
int failing_detour(int x) { return reinterpret_cast<Fn>(g_failing_next[0])(x) + 100; }
int failing_module_detour(int x) { return reinterpret_cast<Fn>(g_failing_next[1])(x) + 1000; }

fs::path this_executable() {
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    return self;
}

// A plugin whose entry is this DLL (an absolute path), wherever its folder is.
ResolvedPlugin& with_entry(ResolvedPlugin& r, const fs::path& dll) {
    r.manifest.entry = narrow(dll.wstring());
    return r;
}

std::string gameplay_toml(const std::string& id, const std::string& extra = "") {
    std::string toml = plugin_toml(id, extra);
    toml.replace(toml.find("affects_gameplay = false"), 24, "affects_gameplay = true");
    return toml;
}

Fn g_next[3] = {};
int detour_a(int x) { return g_next[0](x) + 1; }
int detour_m(int x) { return g_next[1](x) + 10; }
int detour_b(int x) { return g_next[2](x) + 100; }

}  // namespace

extern "C" __declspec(dllexport) AnvilResult anvil_plugin_load(const AnvilHost* host, AnvilPlugin* self) {
    AnvilPlugin* extra = nullptr;
    if (g_failing_target == nullptr || host->module(self, "extra", &extra) != ANVIL_OK) return ANVIL_ERROR_INTERNAL;
    host->hook_function_at(self, g_failing_target->base, kAddOne, 5, reinterpret_cast<void*>(&failing_detour),
                           &g_failing_next[0], nullptr);
    host->hook_function_at(extra, g_failing_target->base, kAddOne, 5, reinterpret_cast<void*>(&failing_module_detour),
                           &g_failing_next[1], nullptr);
    return ANVIL_ERROR_LIMIT;
}

void plugin_set_tests() {
    run("plugin set: owners in load order, a plugin's modules that are on right after it, the core last", [] {
        Code code;
        TestStart start(code.image_base(), 0x1000);
        start.add(plugin_toml("a", "[[modules]]\nid = \"m1\"\nname = \"M1\"\naffects_gameplay = false\n"
                                   "\n[[modules]]\nid = \"m2\"\nname = \"M2\"\naffects_gameplay = false\ndefault = false\n"
                                   "\n[[modules]]\nid = \"m3\"\nname = \"M3\"\naffects_gameplay = false\n"));
        start.add(plugin_toml("b"));
        start.skip(skipped_for("c", i18n::Message("safe_mode", {}), "c", SkipKind::safe_mode));
        PluginSet& set = start.make();
        // a, a.m1, a.m3 (a.m2 is off: no owner), b, then the core; a skipped plugin has none.
        const std::vector<std::string> expected{"a", "a.m1", "a.m3", "b", "anvil"};
        for (OwnerId i = 0; i < expected.size(); ++i) CHECK(start.changes.owner_name(i) == expected[i]);
        CHECK(start.changes.owner_name(static_cast<OwnerId>(expected.size())) == "?");
        CHECK(set.core_owner() == 4 && set.handle_of(set.core_owner()) == nullptr && set.handle_of(99) == nullptr);
        CHECK(set.handle_of(start.handle("a")->owner) == start.handle("a"));
        CHECK(set.handle_of(1) == start.module("a", "a.m1") && set.handle_of(2) == start.module("a", "a.m3"));
        CHECK(set.handle_of(3) == start.handle("b") && start.handle("c") == nullptr);
        // Each handle names its plugin's record; a module's names its declared module.
        CHECK(start.module("a", "a.m3")->record == &start.plugin("a") && start.module("a", "a.m3")->module->id == "m3");
        CHECK(start.handle("a")->module == nullptr);
        // So in a chain the earlier plugin is outermost, its module next, whatever the order of requests.
        void** next = reinterpret_cast<void**>(g_next);
        CHECK(start.changes.hook_function(start.handle("b")->owner, {reinterpret_cast<uintptr_t>(code.base), false, kAddOne, nullptr, 5},
                                          reinterpret_cast<void*>(&detour_b), &next[2]) == ChangeStatus::Ok);
        CHECK(start.changes.hook_function(start.handle("a")->owner, {reinterpret_cast<uintptr_t>(code.base), false, kAddOne, nullptr, 5},
                                          reinterpret_cast<void*>(&detour_a), &next[0]) == ChangeStatus::Ok);
        CHECK(start.changes.hook_function(start.module("a", "a.m1")->owner,
                                          {reinterpret_cast<uintptr_t>(code.base), false, kAddOne, nullptr, 5},
                                          reinterpret_cast<void*>(&detour_m), &next[1]) == ChangeStatus::Ok);
        CHECK((start.changes.chain_at(reinterpret_cast<uintptr_t>(code.base)) ==
               std::vector<OwnerId>{start.handle("a")->owner, start.module("a", "a.m1")->owner, start.handle("b")->owner}));
        CHECK(code.fn()(1) == 113);
    });

    run("plugin set: a module that is off has no handle, no owner and no policy entry", [] {
        TestStart start;
        std::string toml = gameplay_toml("smithy", "[[modules]]\nid = \"on\"\nname = \"On\"\naffects_gameplay = true\n"
                                                   "\n[[modules]]\nid = \"off\"\nname = \"Off\"\naffects_gameplay = true\n");
        ResolvedPlugin& smithy = start.add(toml);
        smithy.modules_on = {true, false};
        start.make();
        const PluginRecord& p = start.plugin("smithy");
        CHECK(p.modules().size() == 2 && p.modules()[0].handle != nullptr && p.modules()[1].handle == nullptr);
        CHECK(p.modules()[1].key == "smithy.off" && p.modules()[1].declared->id == "off");
        CHECK(start.changes.owner_name(2) == "anvil");  // smithy, smithy.on, the core
        CHECK(start.logged("anvil: smithy.off: module off at this start (anvil.toml)"));
        AnvilPlugin* off = start.handle("smithy");
        CHECK(start.host.module(start.handle("smithy"), "off", &off) == ANVIL_ERROR_DISABLED && off == nullptr);
        // In ironman the plugin and its module that is on are suspended; the
        // policy has nothing for the one that is off.
        AnvilGameSession s{sizeof(s), ANVIL_GAME_SESSION_STARTED, 1, 0, 1};
        start.bus.publish(ANVIL_GAME_EVENT_SESSION, &s);
        CHECK(start.policy.suspended(start.handle("smithy")->owner) && start.policy.suspended(p.modules()[0].handle->owner));
        CHECK(start.logged_containing("smithy.on: suspended") == 1 && start.logged_containing("smithy.off") == 1);
    });

    run("plugin set: each way a load fails leaves the outcome failed, its reason, no policy entry and no changes", [] {
        Code target;
        g_failing_target = &target;
        TestStart start(target.image_base(), 0x1000);
        const fs::path probe = mod_probe_dir() / "mod-probe.dll";
        // Gameplay plugins all: the policy would suspend them in ironman.
        start.add(gameplay_toml("missing"));                                                // 1. LoadLibrary fails
        with_entry(start.add(gameplay_toml("base")), probe);                        // loads
        start.add(gameplay_toml("needs", "[[dependencies]]\nid = \"missing\"\n"));          // 2. its dependency failed
        with_entry(start.add(gameplay_toml("copy"), mod_probe_dir()), probe).approved_hash = std::string(64, '0');  // 3. the approved copy changed
        wchar_t kernel32[MAX_PATH] = {};
        GetModuleFileNameW(GetModuleHandleW(L"kernel32.dll"), kernel32, MAX_PATH);
        with_entry(start.add(gameplay_toml("no-export")), kernel32);           // 4. no anvil_plugin_load
        with_entry(start.add(gameplay_toml("refuses", "[[modules]]\nid = \"extra\"\nname = \"Extra\"\n"
                                                    "affects_gameplay = true\n")),
                   this_executable());                               // 5. the load fails
        PluginSet& set = start.make();
        set.load(Phase::Main);
        CHECK(start.loaded("base") && set.count(Outcome::Kind::loaded) == 1 && set.count(Outcome::Kind::failed) == 5);
        const auto reason = [&](const char* id) {
            const Outcome* o = start.plugin(id).outcome();
            CHECK(o != nullptr && o->kind == Outcome::Kind::failed && !o->why.empty() && o->reason == i18n::english(o->why));
            return o != nullptr ? o->reason : std::string();
        };
        CHECK(reason("missing").starts_with("cannot load missing.dll (error "));
        CHECK(reason("needs") == "requires plugin 'missing', which failed to load" && start.plugin("needs").outcome()->why.code == "dependency_failed");
        CHECK(reason("copy").starts_with("its approved copy changed (hash "));
        CHECK(reason("no-export") == "the entry DLL does not export anvil_plugin_load" && start.plugin("no-export").dll() == nullptr);
        CHECK(reason("refuses") == "anvil_plugin_load returned over the limit");
        // Its log line, one per plugin.
        CHECK(start.logged_containing("anvil: missing 1.0.0: cannot load ") == 1);
        CHECK(start.logged("anvil: needs 1.0.0: not loaded; requires plugin 'missing', which failed to load"));
        CHECK(start.logged_containing("anvil: copy 1.0.0: not loaded; its approved copy changed") == 1);
        CHECK(start.logged("anvil: no-export 1.0.0: entry DLL does not export anvil_plugin_load"));
        CHECK(start.logged("anvil: refuses 1.0.0: anvil_plugin_load returned over the limit; 2 change(s) rolled back; "
                           "plugin stays inactive"));
        // No changes: the two the failing load staged were rolled back, through both its handles.
        AnvilPlugin* refuses = start.handle("refuses");
        AnvilPlugin* extra = start.module("refuses", "refuses.extra");
        CHECK(extra != nullptr && start.changes.count(refuses->owner) == 0 && start.changes.count(extra->owner) == 0);
        CHECK(start.changes.staged_count(refuses->owner) == 0 && target.fn()(1) == 2);
        // No policy entry: in ironman only the plugin that loaded is suspended.
        AnvilGameSession s{sizeof(s), ANVIL_GAME_SESSION_STARTED, 1, 0, 1};
        start.bus.publish(ANVIL_GAME_EVENT_SESSION, &s);
        CHECK(start.policy.suspended(start.handle("base")->owner));
        for (const char* id : {"missing", "needs", "copy", "no-export", "refuses"}) {
            CHECK(!start.policy.suspended(start.handle(id)->owner));
            CHECK(start.logged_containing(std::string(id) + ": suspended") == 0);
        }
        CHECK(!start.policy.suspended(extra->owner) && start.logged_containing("refuses.extra: suspended") == 0);
        // A phase loads a plugin once: another call changes nothing.
        set.load(Phase::Main);
        CHECK(set.count(Outcome::Kind::failed) == 5 && start.logged_containing("anvil: missing 1.0.0: cannot load ") == 1);
        g_failing_target = nullptr;
    });

    run("plugin set: one list answers anvil.control and the start record alike for a disabled plugin", [] {
        TestStart start;
        with_entry(start.add(plugin_toml("first")), mod_probe_dir() / "mod-probe.dll");
        start.skip(skipped_for("by-player", i18n::Message("disabled", {}), "p", SkipKind::other, true));
        Skipped by_arm = skipped_for("by-arm", i18n::Message("disabled", {}), "a", SkipKind::other, true);
        by_arm.benchmark_arm = true;
        start.skip(by_arm);
        start.skip(skipped_for("broken", i18n::Message("safe_mode", {}), "b", SkipKind::safe_mode));
        PluginSet& set = start.make();
        ControlSources sources;
        sources.plugins = &set;
        Control control(sources);
        const auto record_before = record_plugins(set, {});
        set.load(Phase::Main);
        control.loading_done();
        const auto record = record_plugins(set, {});
        CHECK(record.size() == set.size() && record_before.size() == set.size());
        CHECK(!record_before[0].outcome && record[0].outcome == Outcome::Kind::loaded);  // pending, then loaded
        for (uint32_t i = 0; i < set.size(); ++i) {
            AnvilControlPlugin p{};
            p.size = sizeof(p);
            CHECK(control.plugin(i, p) && std::string(p.id) == record[i].id);
        }
        // Disabled: "skipped" with the disabled flag in anvil.control, "disabled" with who did it in the record.
        AnvilControlPlugin p{};
        p.size = sizeof(p);
        CHECK(control.plugin(1, p) && std::string(p.state) == "skipped" && p.disabled == 1 && std::string(p.reason) == "disabled in anvil.toml");
        CHECK(control.plugin(2, p) && std::string(p.state) == "skipped" && p.disabled == 1);
        CHECK(control.plugin(3, p) && std::string(p.state) == "skipped" && p.disabled == 0);
        BenchmarkRecord r;
        r.plugins = record;
        const std::string json = benchmark_record_json(r);
        CHECK(json.find("{\"id\":\"first\",\"version\":\"1.0.0\",\"state\":\"loaded\",\"participant\":false}") != std::string::npos);
        CHECK(json.find("{\"id\":\"by-player\",\"state\":\"disabled\",\"by\":\"player\",\"participant\":false}") != std::string::npos);
        CHECK(json.find("{\"id\":\"by-arm\",\"state\":\"disabled\",\"by\":\"arm\",\"participant\":false}") != std::string::npos);
        CHECK(json.find("{\"id\":\"broken\",\"state\":\"skipped\",\"reason\":\"safe_mode\",\"participant\":false}") != std::string::npos);
        // The summary line's counts come from the same list.
        CHECK(set.count(Outcome::Kind::loaded) == 1 && set.count(Outcome::Kind::disabled) == 2 &&
              set.count(Outcome::Kind::skipped) == 1 && set.size() - set.resolved_count() == 3);
    });
}
