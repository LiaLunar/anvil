// Shared by the unit test files: the CHECK harness and manifest helpers.
#pragma once

#include "core/manifest.hpp"

#include <cstdio>
#include <functional>
#include <string>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)

inline void run(const char* name, const std::function<void()>& body) {
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
}

inline anvil::Manifest manifest(const std::string& toml) {
    auto r = anvil::parse_manifest_text(toml, L"C:\\plugins\\x");
    if (!r.manifest) std::printf("  manifest error: %s\n", r.error.c_str());
    return *r.manifest;
}

// A minimal valid plugin.toml v2 with `extra` appended.
inline std::string plugin_toml(const std::string& id, const std::string& extra = "",
                               const std::string& version = "1.0.0", const std::string& phase = "main") {
    return "id = \"" + id + "\"\nname = \"" + id + "\"\nversion = \"" + version + "\"\napi = 2\nentry = \"" + id +
           ".dll\"\nphase = \"" + phase + "\"\naffects_gameplay = false\n" + extra;
}

void manifest_tests();  // manifest_tests.cpp
void resolver_tests();  // resolver_tests.cpp
void change_tests();    // change_tests.cpp
void gameplay_tests();  // gameplay_tests.cpp
void settings_tests();  // settings_tests.cpp
void control_tests();   // control_tests.cpp
void benchmark_tests(); // benchmark_tests.cpp
void manager_benchmark_tests();  // manager_benchmark_tests.cpp, in anvil_manager_tests
void manager_runner_tests();     // the same file: the series runner, with
int fake_benchmark_game();       // anvil_manager_tests --bench-game as the game
void manager_report_tests();     // manager_report_tests.cpp, in anvil_manager_tests
