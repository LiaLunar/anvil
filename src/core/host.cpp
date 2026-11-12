#include "host.hpp"

#include "log.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace anvil {
namespace {

bool valid(const AnvilPlugin* self) { return self != nullptr && self->context != nullptr; }

ChangeRegistry& changes_of(AnvilPlugin* self) { return *self->context->changes; }

// A module's handle uses its plugin's symbols, settings, folders and names
// (ADR 0011); a plugin's own handle is its own root.
AnvilPlugin& root_of(AnvilPlugin* self) { return self->parent != nullptr ? *self->parent : *self; }
const AnvilPlugin& root_of(const AnvilPlugin* self) { return self->parent != nullptr ? *self->parent : *self; }

void write_log(const AnvilPlugin& self, AnvilLogLevel level, std::string_view message) {
    if (self.context->log) {
        self.context->log(self, level, message);
    } else {
        Log::instance().write(level, self.manifest.id, message);
    }
}

// True once the process has started to exit. Plugin DLLs detach before
// Anvil's, and a thread killed by the exit may hold the registry's lock, so
// changes are no longer touched (a global anvil::Change removes itself in its
// destructor).
bool process_exiting() {
    using Fn = BOOLEAN(NTAPI*)();
    static const Fn fn =
        reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress"));
    return fn != nullptr && fn() != FALSE;
}

std::string symbol_label(const AnvilPlugin* self, AnvilSymbol symbol) {
    if (const PluginSymbol* s = root_of(self).symbols.get(symbol)) {
        return s->absent ? s->declaration.name + " (absent on this game build)" : s->declaration.name;
    }
    return "symbol " + std::to_string(symbol) + " (not declared)";
}

std::string address_label(const void* address) {
    char text[32];
    std::snprintf(text, sizeof(text), "%p", address);
    return text;
}

// A change request: the handle out, the refusal in the plugin's log.
AnvilResult finish(AnvilPlugin* self, const char* call, const std::string& what, ChangeStatus status, ChangeId id,
                   AnvilChange** out) {
    const AnvilResult r = to_result(status);
    if (out != nullptr) *out = r == ANVIL_OK ? reinterpret_cast<AnvilChange*>(static_cast<uintptr_t>(id)) : nullptr;
    if (r != ANVIL_OK) {
        write_log(*self, ANVIL_LOG_WARNING, std::string(call) + " " + what + " refused: " + result_name(r));
    }
    return r;
}

ChangeId change_id(const AnvilChange* change) { return static_cast<ChangeId>(reinterpret_cast<uintptr_t>(change)); }

// ---- environment -------------------------------------------------------------

void host_log(AnvilPlugin* self, AnvilLogLevel level, const char* message) {
    if (message == nullptr) return;
    if (!valid(self)) {
        Log::instance().write(level, "?", message);
        return;
    }
    write_log(*self, level, message);
}

const wchar_t* host_plugin_dir(AnvilPlugin* self) { return valid(self) ? root_of(self).plugin_dir.c_str() : nullptr; }

const wchar_t* host_data_dir(AnvilPlugin* self) {
    if (!valid(self)) return nullptr;
    std::error_code ignored;
    std::filesystem::create_directories(root_of(self).data_dir, ignored);
    return root_of(self).data_dir.c_str();
}

// ---- changes on symbols ---------------------------------------------------------

AnvilResult host_hook_function(AnvilPlugin* self, AnvilSymbol fn, void* detour, void** next, AnvilChange** out) {
    if (!valid(self) || detour == nullptr || next == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = hook_function(changes_of(self), self->owner, root_of(self).symbols, fn, detour, next, &id);
    return finish(self, "hook_function", symbol_label(self, fn), s, id, out);
}

AnvilResult host_mid_hook(AnvilPlugin* self, AnvilSymbol site, AnvilCpuCallback callback, void* user,
                          AnvilChange** out) {
    if (!valid(self) || callback == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = mid_hook(changes_of(self), self->owner, root_of(self).symbols, site, callback, user, &id);
    return finish(self, "mid_hook", symbol_label(self, site), s, id, out);
}

AnvilResult host_replace_block(AnvilPlugin* self, AnvilSymbol block, AnvilCpuCallback callback, void* user,
                               AnvilChange** out) {
    if (!valid(self) || callback == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = replace_block(changes_of(self), self->owner, root_of(self).symbols, block, callback, user, &id);
    return finish(self, "replace_block", symbol_label(self, block), s, id, out);
}

AnvilResult host_redirect_call(AnvilPlugin* self, AnvilSymbol call_site, void* target, void** next,
                               AnvilChange** out) {
    if (!valid(self) || target == nullptr || next == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = redirect_call(changes_of(self), self->owner, root_of(self).symbols, call_site, target, next, &id);
    return finish(self, "redirect_call", symbol_label(self, call_site), s, id, out);
}

AnvilResult host_patch_bytes(AnvilPlugin* self, AnvilSymbol site, const void* bytes, size_t size, AnvilChange** out) {
    if (!valid(self) || bytes == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = patch_bytes(changes_of(self), self->owner, root_of(self).symbols, site, bytes, size, &id);
    return finish(self, "patch_bytes", symbol_label(self, site), s, id, out);
}

AnvilResult host_swap_pointer(AnvilPlugin* self, AnvilSymbol slot, void* value, void** previous, AnvilChange** out) {
    if (!valid(self)) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = swap_pointer(changes_of(self), self->owner, root_of(self).symbols, slot, value, previous, &id);
    return finish(self, "swap_pointer", symbol_label(self, slot), s, id, out);
}

AnvilResult host_replace_import(AnvilPlugin* self, const char* module, const char* function, void* replacement,
                                void** next, AnvilChange** out) {
    if (!valid(self) || module == nullptr || function == nullptr || replacement == nullptr || next == nullptr) {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    ChangeId id = 0;
    const ChangeStatus s =
        changes_of(self).replace_import(self->owner, self->context->image, module, function, replacement, next, &id);
    return finish(self, "replace_import", std::string(module) + "!" + function, s, id, out);
}

// ---- changes outside eu4.exe --------------------------------------------------------

ChangeTarget raw_target(void* address, const void* expected, size_t size) {
    ChangeTarget t;
    t.address = reinterpret_cast<uintptr_t>(address);
    t.inside_image = false;
    t.expected = static_cast<const uint8_t*>(expected);
    t.expected_size = expected != nullptr ? size : 0;
    return t;
}

AnvilResult host_hook_function_at(AnvilPlugin* self, void* address, const void* expected, size_t size, void* detour,
                                  void** next, AnvilChange** out) {
    if (!valid(self) || address == nullptr || detour == nullptr || next == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = changes_of(self).hook_function(self->owner, raw_target(address, expected, size), detour, next, &id);
    return finish(self, "hook_function_at", address_label(address), s, id, out);
}

AnvilResult host_mid_hook_at(AnvilPlugin* self, void* address, const void* expected, size_t size,
                             AnvilCpuCallback callback, void* user, AnvilChange** out) {
    if (!valid(self) || address == nullptr || callback == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeId id = 0;
    const ChangeStatus s = changes_of(self).mid_hook(self->owner, raw_target(address, expected, size), callback, user, &id);
    return finish(self, "mid_hook_at", address_label(address), s, id, out);
}

AnvilResult host_remove(AnvilPlugin* self, AnvilChange* change) {
    if (process_exiting()) return ANVIL_OK;
    if (!valid(self) || change == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    ChangeRegistry& changes = changes_of(self);
    const ChangeStatus status = changes.remove(self->owner, change_id(change));
    if (status != ChangeStatus::NotFound) return to_result(status);
    // Any handle of the same plugin may remove a change of its plugin or of
    // one of its modules (ADR 0011): the C++ and Rust wrappers remove with the
    // handle the plugin was loaded with.
    AnvilPlugin& root = root_of(self);
    if (&root != self) {
        if (const ChangeStatus s = changes.remove(root.owner, change_id(change)); s != ChangeStatus::NotFound) {
            return to_result(s);
        }
    }
    for (const auto& m : root.modules) {
        if (m.get() == self) continue;
        if (const ChangeStatus s = changes.remove(m->owner, change_id(change)); s != ChangeStatus::NotFound) {
            return to_result(s);
        }
    }
    return ANVIL_ERROR_NOT_FOUND;
}

AnvilResult host_begin_group(AnvilPlugin* self) {
    if (!valid(self)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (root_of(self).loading) return ANVIL_ERROR_WRONG_PHASE;
    return to_result(changes_of(self).begin_group(self->owner));
}

AnvilResult host_commit_group(AnvilPlugin* self) {
    if (!valid(self)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (root_of(self).loading) return ANVIL_ERROR_WRONG_PHASE;
    return to_result(changes_of(self).commit_group(self->owner));
}

void host_abort_group(AnvilPlugin* self) {
    if (!valid(self) || root_of(self).loading) return;
    changes_of(self).abort_group(self->owner);
}

void* host_address_of(AnvilPlugin* self, AnvilSymbol symbol) {
    return valid(self) ? root_of(self).symbols.address_of(symbol) : nullptr;
}

AnvilResult host_symbol_value(AnvilPlugin* self, AnvilSymbol symbol, const char* name, uint64_t* out) {
    if (!valid(self) || name == nullptr || out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    const auto value = root_of(self).symbols.value(symbol, name);
    if (!value) return ANVIL_ERROR_NOT_DECLARED;
    *out = *value;
    return ANVIL_OK;
}

// ---- settings (design §11) ------------------------------------------------------------

int host_setting_bool(AnvilPlugin* self, AnvilSetting setting) {
    return valid(self) ? root_of(self).settings.get_bool(setting) : 0;
}

int64_t host_setting_int(AnvilPlugin* self, AnvilSetting setting) {
    return valid(self) ? root_of(self).settings.get_int(setting) : 0;
}

double host_setting_float(AnvilPlugin* self, AnvilSetting setting) {
    return valid(self) ? root_of(self).settings.get_float(setting) : 0.0;
}

const char* host_setting_string(AnvilPlugin* self, AnvilSetting setting) {
    return valid(self) ? root_of(self).settings.get_string(setting) : "";
}

AnvilResult host_subscribe_setting(AnvilPlugin* self, AnvilSetting setting, AnvilSettingCallback callback,
                                   void* context) {
    if (!valid(self)) return ANVIL_ERROR_INVALID_ARGUMENT;
    PluginSettings& settings = root_of(self).settings;
    const AnvilResult r = settings.subscribe(setting, callback, context);
    if (r != ANVIL_OK) {
        const Setting* s = settings.schema().get(setting);
        const std::string what = s != nullptr ? s->name : "setting " + std::to_string(setting) + " (not declared)";
        write_log(*self, ANVIL_LOG_WARNING,
                  "subscribe_setting " + what + " refused: " +
                      (r == ANVIL_ERROR_WRONG_KIND ? "it applies at the next start (restart = true)" : result_name(r)));
    }
    return r;
}

// ---- services and events ------------------------------------------------------------

AnvilResult host_register_service(AnvilPlugin* self, const char* name, uint32_t version, const void* service) {
    if (!valid(self) || name == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    const AnvilResult r = self->context->bus->register_service(root_of(self).manifest.id, name, version, service);
    if (r != ANVIL_OK) {
        write_log(*self, ANVIL_LOG_WARNING, std::string("register_service ") + name + " refused: " + result_name(r));
    }
    return r;
}

const void* host_get_service(AnvilPlugin* self, const char* name, uint32_t version) {
    if (!valid(self) || name == nullptr) return nullptr;
    return self->context->bus->get_service(name, version);
}

AnvilResult host_subscribe(AnvilPlugin* self, const char* event, AnvilEventCallback callback, void* context) {
    if (!valid(self) || event == nullptr || callback == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    // A module subscribes as "<plugin>.<module>", so anvil.suspended for the
    // module reaches only what it subscribed (ADR 0011).
    return self->context->bus->subscribe(self->manifest.id, event, callback, context);
}

AnvilResult host_publish(AnvilPlugin* self, const char* event, const void* payload) {
    if (!valid(self) || event == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    // "anvil." names are Anvil's; a plugin publishes "<its id>.<name>" only,
    // as for services, so no plugin can forge another's events (Anvil acts on
    // anvil-game's session event, design §13).
    const std::string& id = root_of(self).manifest.id;
    if (std::string_view(event).starts_with("anvil.") || !is_valid_service_name(id, event)) {
        write_log(*self, ANVIL_LOG_WARNING,
                  std::string("publish ") + event + " refused: plugins publish only events named " + id + ".<name>");
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    self->context->bus->publish(event, payload);
    return ANVIL_OK;
}

// ---- modules (ADR 0011) ------------------------------------------------------------------

AnvilResult host_module(AnvilPlugin* self, const char* module, AnvilPlugin** out) {
    if (out != nullptr) *out = nullptr;
    if (!valid(self) || module == nullptr || out == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (self->parent != nullptr) return ANVIL_ERROR_NOT_DECLARED;  // modules have no modules
    for (const auto& m : self->modules) {
        if (m->module_id != module) continue;
        if (!m->enabled) return ANVIL_ERROR_DISABLED;
        *out = m.get();
        return ANVIL_OK;
    }
    write_log(*self, ANVIL_LOG_WARNING, std::string("module ") + module + " refused: not declared in plugin.toml");
    return ANVIL_ERROR_NOT_DECLARED;
}

}  // namespace

AnvilResult to_result(ChangeStatus status) {
    switch (status) {
    case ChangeStatus::Ok: return ANVIL_OK;
    case ChangeStatus::InvalidArgument: return ANVIL_ERROR_INVALID_ARGUMENT;
    case ChangeStatus::NotDeclared: return ANVIL_ERROR_NOT_DECLARED;
    case ChangeStatus::WrongKind: return ANVIL_ERROR_WRONG_KIND;
    case ChangeStatus::Conflict: return ANVIL_ERROR_CONFLICT;
    case ChangeStatus::UnsafeSite: return ANVIL_ERROR_UNSAFE_SITE;
    case ChangeStatus::BytesMismatch: return ANVIL_ERROR_BYTES_MISMATCH;
    case ChangeStatus::OutOfMemory: return ANVIL_ERROR_OUT_OF_MEMORY;
    case ChangeStatus::WrongPhase: return ANVIL_ERROR_WRONG_PHASE;
    case ChangeStatus::NotFound: return ANVIL_ERROR_NOT_FOUND;
    case ChangeStatus::Internal: return ANVIL_ERROR_INTERNAL;
    }
    return ANVIL_ERROR_INTERNAL;
}

const char* result_name(AnvilResult result) {
    switch (result) {
    case ANVIL_OK: return "ok";
    case ANVIL_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case ANVIL_ERROR_NOT_DECLARED: return "not declared in symbols.toml or settings.toml";
    case ANVIL_ERROR_WRONG_KIND: return "wrong kind of symbol or setting";
    case ANVIL_ERROR_CONFLICT: return "conflict with another change";
    case ANVIL_ERROR_UNSAFE_SITE: return "unsafe site";
    case ANVIL_ERROR_BYTES_MISMATCH: return "bytes mismatch";
    case ANVIL_ERROR_OUT_OF_MEMORY: return "out of memory";
    case ANVIL_ERROR_WRONG_PHASE: return "wrong phase";
    case ANVIL_ERROR_UNSUPPORTED: return "unsupported";
    case ANVIL_ERROR_INTERNAL: return "internal error";
    case ANVIL_ERROR_NOT_FOUND: return "not found";
    case ANVIL_ERROR_ALREADY_EXISTS: return "already exists";
    case ANVIL_ERROR_SUSPENDED: return "suspended in this session";
    case ANVIL_ERROR_LIMIT: return "over the limit";
    case ANVIL_ERROR_DISABLED: return "turned off by the player";
    }
    return "unknown";
}

AnvilHost make_host(const AnvilGameInfo* game) {
    AnvilHost h{};
    h.size = sizeof(AnvilHost);
    h.api_version = ANVIL_API_VERSION;
    h.anvil_version = ANVIL_VERSION;
    h.game = game;
    h.log = host_log;
    h.plugin_dir = host_plugin_dir;
    h.data_dir = host_data_dir;
    h.hook_function = host_hook_function;
    h.mid_hook = host_mid_hook;
    h.replace_block = host_replace_block;
    h.redirect_call = host_redirect_call;
    h.patch_bytes = host_patch_bytes;
    h.swap_pointer = host_swap_pointer;
    h.replace_import = host_replace_import;
    h.hook_function_at = host_hook_function_at;
    h.mid_hook_at = host_mid_hook_at;
    h.remove = host_remove;
    h.begin_group = host_begin_group;
    h.commit_group = host_commit_group;
    h.abort_group = host_abort_group;
    h.address_of = host_address_of;
    h.symbol_value = host_symbol_value;
    h.setting_bool = host_setting_bool;
    h.setting_int = host_setting_int;
    h.setting_float = host_setting_float;
    h.setting_string = host_setting_string;
    h.subscribe_setting = host_subscribe_setting;
    h.register_service = host_register_service;
    h.get_service = host_get_service;
    h.subscribe = host_subscribe;
    h.publish = host_publish;
    h.module = host_module;
    return h;
}

void add_modules(AnvilPlugin& plugin, ChangeRegistry& changes, const ModuleEnabled& enabled) {
    plugin.modules.reserve(plugin.manifest.modules.size());
    for (const Module& m : plugin.manifest.modules) {
        auto handle = std::make_unique<AnvilPlugin>();
        handle->context = plugin.context;
        handle->parent = &plugin;
        handle->module_id = m.id;
        handle->manifest = module_manifest(plugin.manifest, m);
        handle->owner = changes.add_owner(handle->manifest.id);
        handle->enabled = enabled ? enabled(handle->manifest.id, m.enabled_by_default) : m.enabled_by_default;
        handle->state = handle->enabled ? "pending" : "disabled";
        plugin.modules.push_back(std::move(handle));
    }
}

LoadOutcome run_plugin_load(AnvilPlugin& plugin, const AnvilHost& host, PluginLoadFn fn) {
    ChangeRegistry& changes = *plugin.context->changes;
    LoadOutcome out;
    // The load is one group for the plugin and one for each module: changes
    // requested through any handle are enabled when the load returns ANVIL_OK,
    // and none are when it fails (ADR 0011).
    changes.begin_group(plugin.owner);
    for (const auto& m : plugin.modules) changes.begin_group(m->owner);
    plugin.loading = true;
    out.result = call_plugin_load(fn, &host, &plugin, &out.exception);
    plugin.loading = false;
    out.staged = changes.staged_count(plugin.owner);
    for (const auto& m : plugin.modules) out.staged += changes.staged_count(m->owner);
    if (out.result == ANVIL_OK) {
        bool committed = changes.commit_group(plugin.owner) == ChangeStatus::Ok;
        for (const auto& m : plugin.modules) committed = changes.commit_group(m->owner) == ChangeStatus::Ok && committed;
        if (!committed) {
            out.result = ANVIL_ERROR_INTERNAL;
            out.commit_failed = true;
        }
    }
    if (out.result != ANVIL_OK) {
        changes.rollback(plugin.owner);
        plugin.context->bus->remove_owner(plugin.manifest.id);
        for (const auto& m : plugin.modules) {
            changes.rollback(m->owner);
            plugin.context->bus->remove_owner(m->manifest.id);
        }
        plugin.settings.drop_subscriptions();
    }
    const char* state = out.result == ANVIL_OK ? "loaded" : "failed";
    for (const auto& m : plugin.modules) {
        if (m->enabled) m->state = state;
    }
    return out;
}

}  // namespace anvil
