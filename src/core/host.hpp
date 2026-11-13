#pragma once

#include "changes.hpp"
#include "services.hpp"

#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <functional>
#include <string_view>

// A handle (plugin_set.hpp).
struct AnvilPlugin;

namespace anvil {

// What the host table acts on. The game has one for the process
// (runtime.cpp); the tests build their own around a synthetic image.
struct HostContext {
    ChangeRegistry* changes = nullptr;
    ServiceBus* bus = nullptr;
    HMODULE image = nullptr;  // whose imports replace_import changes (eu4.exe)
    // Plugin log lines; empty: the process log (Log::instance()).
    std::function<void(const AnvilPlugin& plugin, AnvilLogLevel level, std::string_view message)> log;
};

// The API 2 host table: every field filled, `game` as given. The functions
// reach their context through `self`, so one table serves every plugin.
AnvilHost make_host(const AnvilGameInfo* game);

AnvilResult to_result(ChangeStatus status);
const char* result_name(AnvilResult result);

}  // namespace anvil
