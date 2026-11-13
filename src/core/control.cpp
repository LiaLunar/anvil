#include "control.hpp"

#include "anvil_toml_edit.hpp"
#include "game_build.hpp"
#include "log.hpp"
#include "plugin_set.hpp"
#include "plugin_settings.hpp"
#include "plugin_hash.hpp"
#include "symbols.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace anvil {
namespace {

Control* g_control = nullptr;

constexpr size_t kTailBytes = 64 * 1024;

void copy_error(AnvilControlResult* out, const std::string& error) {
    if (out == nullptr) return;
    const size_t n = std::min(error.size(), sizeof(out->error) - 1);
    std::memcpy(out->error, error.data(), n);
    out->error[n] = '\0';
}

void reset_result(AnvilControlResult* out) {
    if (out == nullptr) return;
    *out = AnvilControlResult{};
    out->size = sizeof(*out);
}

AnvilResult finish_change(const PluginSettings::Change& change, AnvilControlResult* out) {
    if (out != nullptr) {
        out->applied = change.applied ? 1 : 0;
        out->saved = change.saved ? 1 : 0;
        out->notified = static_cast<uint32_t>(change.notified);
        copy_error(out, change.error);
    }
    return change.result;
}

// ---- the C table ---------------------------------------------------------------

void svc_status(AnvilControlStatus* out) {
    if (out == nullptr) return;
    if (g_control == nullptr) {
        *out = AnvilControlStatus{};
        out->size = sizeof(*out);
        return;
    }
    g_control->status(*out);
}

// Fills the caller's struct only as far as its `size` says (an overlay built
// before fields were appended passes a smaller one).
template <typename T, typename Fill> int fill_sized(T* out, Fill&& fill) {
    if (g_control == nullptr || out == nullptr || out->size < sizeof(uint32_t)) return 0;
    T full{};
    if (!fill(full)) return 0;
    const uint32_t size = out->size;
    std::memcpy(out, &full, std::min<size_t>(size, sizeof(T)));
    out->size = std::min<uint32_t>(size, sizeof(T));
    return 1;
}

int svc_plugin(uint32_t index, AnvilControlPlugin* out) {
    return fill_sized(out, [&](AnvilControlPlugin& full) { return g_control->plugin(index, full); });
}

int svc_warning(uint32_t index, AnvilControlSink sink, void* context) {
    if (g_control == nullptr || sink == nullptr) return 0;
    const auto line = g_control->warning(index);
    if (!line) return 0;
    sink(context, line->data(), line->size());
    return 1;
}

PluginSettings* settings_of(const char* plugin_id) {
    return g_control != nullptr && plugin_id != nullptr ? g_control->settings_of(plugin_id) : nullptr;
}

uint32_t svc_setting_count(const char* plugin_id) {
    const PluginSettings* s = settings_of(plugin_id);
    return s == nullptr ? 0 : static_cast<uint32_t>(s->size());
}

int svc_setting_info(const char* plugin_id, AnvilSetting setting, AnvilControlSetting* out) {
    const PluginSettings* s = settings_of(plugin_id);
    const Setting* d = s == nullptr ? nullptr : s->schema().get(setting);
    if (d == nullptr || out == nullptr) return 0;
    *out = AnvilControlSetting{};
    out->size = sizeof(*out);
    out->name = d->name.c_str();
    out->label = d->label.c_str();
    out->description = d->description.c_str();
    out->group = d->group.c_str();
    out->type = static_cast<uint32_t>(d->type);
    out->restart = d->restart ? 1 : 0;
    const bool is_int = d->type == SettingType::Int;
    const auto number = [&](const SettingValue& v) { return is_int ? static_cast<double>(v.integer) : v.real; };
    if (d->min) out->has_min = 1, out->min = number(*d->min);
    if (d->max) out->has_max = 1, out->max = number(*d->max);
    if (d->step) out->has_step = 1, out->step = number(*d->step);
    out->options = static_cast<uint32_t>(d->options.size());
    return 1;
}

int svc_setting_option(const char* plugin_id, AnvilSetting setting, uint32_t index, const char** name,
                       const char** label) {
    const PluginSettings* s = settings_of(plugin_id);
    const Setting* d = s == nullptr ? nullptr : s->schema().get(setting);
    if (d == nullptr || index >= d->options.size()) return 0;
    const SettingOption& o = d->options[index];
    if (name != nullptr) *name = o.name.c_str();
    if (label != nullptr) *label = o.label.empty() ? o.name.c_str() : o.label.c_str();
    return 1;
}

int svc_setting_value(const char* plugin_id, AnvilSetting setting, uint32_t which, AnvilControlSink sink,
                      void* context) {
    const PluginSettings* s = settings_of(plugin_id);
    const Setting* d = s == nullptr ? nullptr : s->schema().get(setting);
    if (d == nullptr || sink == nullptr) return 0;
    std::optional<SettingValue> value;
    switch (which) {
    case ANVIL_CONTROL_VALUE_CURRENT: value = s->current(setting); break;
    case ANVIL_CONTROL_VALUE_SAVED: value = s->saved(setting); break;
    case ANVIL_CONTROL_VALUE_DEFAULT: value = d->default_value; break;
    default: return 0;
    }
    if (!value) return 0;
    const std::string text = setting_text(*d, *value);
    sink(context, text.data(), text.size());
    return 1;
}

AnvilResult svc_setting_set(const char* plugin_id, AnvilSetting setting, const AnvilControlValue* value,
                            AnvilControlResult* out) {
    reset_result(out);
    PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr) {
        copy_error(out, "no such plugin");
        return ANVIL_ERROR_NOT_FOUND;
    }
    if (value == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    SettingValue v;
    v.integer = value->integer;
    v.real = value->real;
    if (value->text != nullptr) v.text = value->text;
    return finish_change(s->set(setting, v), out);
}

AnvilResult svc_setting_set_text(const char* plugin_id, AnvilSetting setting, const char* text,
                                 AnvilControlResult* out) {
    reset_result(out);
    PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr) {
        copy_error(out, "no such plugin");
        return ANVIL_ERROR_NOT_FOUND;
    }
    if (text == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return finish_change(s->set_text(setting, text), out);
}

AnvilResult svc_setting_reset(const char* plugin_id, AnvilSetting setting, AnvilControlResult* out) {
    reset_result(out);
    PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr) {
        copy_error(out, "no such plugin");
        return ANVIL_ERROR_NOT_FOUND;
    }
    return finish_change(s->reset(setting), out);
}

int svc_setting_warning(const char* plugin_id, uint32_t index, AnvilControlSink sink, void* context) {
    const PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr || sink == nullptr || index >= s->warnings().size()) return 0;
    const std::string& line = s->warnings()[index];
    sink(context, line.data(), line.size());
    return 1;
}

int svc_previous_crash(AnvilControlCrash* out) {
    if (g_control == nullptr || out == nullptr) return 0;
    const AnvilControlCrash* crash = g_control->previous_crash();
    if (crash == nullptr) return 0;
    *out = *crash;
    return 1;
}

uint32_t svc_log_tail(uint32_t max_lines, AnvilControlSink sink, void* context) {
    if (g_control == nullptr || sink == nullptr) return 0;
    const std::string tail = g_control->log_tail(max_lines);
    sink(context, tail.data(), tail.size());
    return static_cast<uint32_t>(std::count(tail.begin(), tail.end(), '\n') +
                                 (!tail.empty() && tail.back() != '\n' ? 1 : 0));
}

AnvilResult svc_disable_next_start(const char* plugin_id, AnvilControlResult* out) {
    reset_result(out);
    if (g_control == nullptr || plugin_id == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string error;
    const AnvilResult r = g_control->disable_next_start(plugin_id, error);
    if (out != nullptr) out->saved = r == ANVIL_OK ? 1 : 0;
    copy_error(out, error);
    return r;
}

uint32_t svc_mod_plugin_count() { return g_control != nullptr ? g_control->mod_plugin_count() : 0; }

int svc_mod_plugin(uint32_t index, AnvilControlModPlugin* out) {
    return fill_sized(out, [&](AnvilControlModPlugin& full) { return g_control->mod_plugin(index, full); });
}

AnvilResult svc_approve_mod_plugin(uint32_t index, const char* hash, AnvilControlResult* out) {
    reset_result(out);
    if (g_control == nullptr || hash == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string error;
    const AnvilResult r = g_control->approve_mod_plugin(index, hash, error);
    if (out != nullptr) out->saved = r == ANVIL_OK ? 1 : 0;
    copy_error(out, error);
    return r;
}

AnvilResult svc_revoke_mod_plugin(uint32_t index, AnvilControlResult* out) {
    reset_result(out);
    if (g_control == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string error;
    const AnvilResult r = g_control->revoke_mod_plugin(index, error);
    if (out != nullptr) out->saved = r == ANVIL_OK ? 1 : 0;
    copy_error(out, error);
    return r;
}

const char* svc_language() { return g_control != nullptr ? g_control->language() : "en"; }

int svc_plugin_of(const AnvilPlugin* plugin, AnvilControlPlugin* out) {
    return fill_sized(out, [&](AnvilControlPlugin& full) { return g_control->plugin_of(plugin, full); });
}

const char* svc_plugin_text(const char* plugin_id, const char* english) {
    if (g_control == nullptr || plugin_id == nullptr) return english;
    return g_control->plugin_text(plugin_id, english);
}

const char* svc_mod_plugin_text(uint32_t index, const char* english) {
    return g_control != nullptr ? g_control->mod_plugin_text(index, english) : english;
}

uint32_t svc_module_count(const char* plugin_id) {
    return g_control != nullptr && plugin_id != nullptr ? g_control->module_count(plugin_id) : 0;
}

int svc_module(const char* plugin_id, uint32_t index, AnvilControlModule* out) {
    if (plugin_id == nullptr) return 0;
    return fill_sized(out, [&](AnvilControlModule& full) { return g_control->module(plugin_id, index, full); });
}

uint32_t svc_mod_plugin_module_count(uint32_t index) {
    return g_control != nullptr ? g_control->mod_plugin_module_count(index) : 0;
}

int svc_mod_plugin_module(uint32_t index, uint32_t module, AnvilControlModule* out) {
    return fill_sized(out, [&](AnvilControlModule& full) { return g_control->mod_plugin_module(index, module, full); });
}

const AnvilControlService kService{
    sizeof(AnvilControlService),
    svc_status,
    svc_plugin,
    svc_warning,
    svc_setting_count,
    svc_setting_info,
    svc_setting_option,
    svc_setting_value,
    svc_setting_set,
    svc_setting_set_text,
    svc_setting_reset,
    svc_setting_warning,
    svc_previous_crash,
    svc_log_tail,
    svc_disable_next_start,
    svc_mod_plugin_count,
    svc_mod_plugin,
    svc_approve_mod_plugin,
    svc_revoke_mod_plugin,
    svc_language,
    svc_plugin_of,
    svc_plugin_text,
    svc_mod_plugin_text,
    svc_module_count,
    svc_module,
    svc_mod_plugin_module_count,
    svc_mod_plugin_module,
};

// The reason as a message for the overlay (i18n::serialize).
std::string reason_message(const Outcome& o) { return i18n::serialize(o.why.empty() ? i18n::Message(o.reason) : o.why); }

// A module that is on shares its plugin's outcome. It reads "pending" when its
// plugin failed before anvil_plugin_load ran (a dependency, the approved copy,
// the DLL), as anvil.control answered before the plugin set.
const char* module_state(const PluginRecord& p) {
    const Outcome* o = p.outcome();
    if (o != nullptr && o->kind == Outcome::Kind::failed && p.dll() == nullptr) return "pending";
    return outcome_name(o);
}

}  // namespace

// ---- Control ---------------------------------------------------------------------

namespace {

// The translation's own copy of the words, which lives as long as `t`.
const char* translated(const PluginTranslation& t, const char* english) {
    if (english == nullptr) return english;
    const auto it = t.text.find(std::string_view(english));
    return it != t.text.end() ? it->second.c_str() : english;
}

}  // namespace

const char* Control::plugin_text(std::string_view plugin_id, const char* english) const {
    const auto it = sources_.translations.find(plugin_id);
    return it != sources_.translations.end() ? translated(it->second, english) : english;
}

const char* Control::mod_plugin_text(uint32_t index, const char* english) const {
    const auto it = sources_.mod_translations.find(index);
    return it != sources_.mod_translations.end() ? translated(it->second, english) : english;
}

Control::Control(ControlSources sources) : sources_(std::move(sources)) {
    if (sources_.plugins != nullptr) {
        for (const PluginRecord& p : *sources_.plugins) {
            const bool skipped = p.handle() == nullptr;
            versions_.push_back(skipped ? std::string() : p.manifest().version.to_string());
            messages_.push_back(skipped ? reason_message(*p.outcome()) : std::string());
        }
    }
    disabled_next_.insert(sources_.disabled.begin(), sources_.disabled.end());
    if (sources_.mods != nullptr) {
        for (const TrustEntry& e : sources_.mods->trust.entries) bindings_[e.id] = {e.mod, e.hash};
        for (const ModPlugin& p : sources_.mods->plugins) {
            ModView v;
            if (p.manifest) {
                const Manifest& m = *p.manifest;
                v.name = m.name;
                v.version = m.version.to_string();
                for (const auto& a : m.authors) v.authors += (v.authors.empty() ? "" : ", ") + a;
                v.description = m.description;
                v.repository = m.repository;
                v.issues = m.issues;
                for (const Module& mod : m.modules) v.module_keys.push_back(module_key(m, mod));
                if (!m.symbols.empty()) {
                    const SymbolFileResult symbols = load_symbols(p.folder / m.symbols);
                    if (symbols.file) v.symbols = static_cast<uint32_t>(symbols.file->symbols.size());
                }
            }
            for (const auto& o : p.others) v.others += (v.others.empty() ? "" : ", ") + o;
            v.reason_message = i18n::serialize(p.why.empty() ? p.reason : p.why);
            if (p.approval && p.bound) {
                v.approved_version = p.approval->version;
                v.approved_hash = p.approval->hash;
                v.approved_date = p.approval->approved;
            }
            mod_views_.push_back(std::move(v));
        }
    }
    g_control = this;
}

Control::~Control() {
    if (g_control == this) g_control = nullptr;
}

const AnvilControlService* Control::service() const { return &kService; }

void Control::loading_done() {
    if (sources_.plugins != nullptr && !loading_done_.load(std::memory_order_acquire)) {
        for (size_t i = 0; i < sources_.plugins->resolved_count(); ++i) {
            const Outcome* o = sources_.plugins->at(i).outcome();
            messages_[i] = o != nullptr ? reason_message(*o) : std::string();
        }
    }
    loading_done_.store(true, std::memory_order_release);
}

const PluginRecord* Control::loadable(std::string_view plugin_id) const {
    const PluginRecord* p = sources_.plugins != nullptr ? sources_.plugins->find(plugin_id) : nullptr;
    return p != nullptr && p->handle() != nullptr ? p : nullptr;
}

void Control::status(AnvilControlStatus& out) const {
    out = AnvilControlStatus{};
    out.size = sizeof(out);
    out.anvil_version = sources_.anvil_version;
    out.game_build = sources_.game_build;
    out.safe_mode = sources_.safe_mode ? 1 : 0;
    out.unfinished_starts = sources_.unfinished_starts;
    const size_t all = sources_.plugins ? sources_.plugins->size() : 0;
    const size_t resolved = sources_.plugins ? sources_.plugins->resolved_count() : 0;
    out.plugins = static_cast<uint32_t>(all);
    out.skipped = static_cast<uint32_t>(all - resolved);
    out.warnings = static_cast<uint32_t>(Log::instance().warning_count());
    const bool done = loading_done_.load(std::memory_order_acquire);
    out.loading_done = done ? 1 : 0;
    if (sources_.policy != nullptr) {
        if (const auto session = sources_.policy->session()) {
            out.session = 1;
            out.ironman = session->ironman ? 1 : 0;
            out.multiplayer = session->multiplayer ? 1 : 0;
        }
    }
    if (!done || sources_.plugins == nullptr) return;
    out.loaded = static_cast<uint32_t>(sources_.plugins->count(Outcome::Kind::loaded));
    out.failed = static_cast<uint32_t>(sources_.plugins->count(Outcome::Kind::failed));
    std::vector<std::string> active;
    for (size_t i = 0; i < resolved; ++i) {
        const PluginRecord& p = sources_.plugins->at(i);
        const Outcome* o = p.outcome();
        if (o == nullptr || o->kind != Outcome::Kind::loaded) continue;
        // One line for the plugin's own handle and one per module that
        // affects gameplay and acts now (ADR 0011): "<plugin>.<module> <version>".
        const std::string version = p.manifest().version.to_string();
        const auto acting = [&](const AnvilPlugin& h) {
            return h.affects_gameplay && (sources_.policy == nullptr || !sources_.policy->suspended(h.owner));
        };
        if (acting(*p.handle())) active.push_back(p.id() + " " + version);
        for (const auto& m : p.modules()) {
            if (m.handle != nullptr && acting(*m.handle)) active.push_back(m.key + " " + version);
        }
    }
    out.gameplay_active = static_cast<uint32_t>(active.size());
    const std::string hash = plugin_set_hash(std::move(active));
    std::snprintf(out.gameplay_hash, sizeof(out.gameplay_hash), "%s", hash.c_str());
}

bool Control::plugin(uint32_t index, AnvilControlPlugin& out) const {
    if (sources_.plugins == nullptr || index >= sources_.plugins->size()) return false;
    const PluginRecord& p = sources_.plugins->at(index);
    out = AnvilControlPlugin{};
    out.size = sizeof(out);
    out.id = p.id().c_str();
    out.version = versions_[index].c_str();
    if (const AnvilPlugin* self = p.handle()) {
        const Manifest& m = p.manifest();
        out.name = m.name.c_str();
        out.issues = m.issues.c_str();
        out.early = m.phase == Phase::Early ? 1 : 0;
        out.affects_gameplay = m.affects_gameplay ? 1 : 0;
        out.settings = static_cast<uint32_t>(p.settings().size());
        const Outcome* o = loading_done_.load(std::memory_order_acquire) ? p.outcome() : nullptr;
        out.state = outcome_name(o);
        out.reason = o != nullptr ? o->reason.c_str() : "";
        out.reason_message = o != nullptr ? messages_[index].c_str() : "";
        out.suspended = sources_.policy != nullptr && sources_.policy->suspended(self->owner) ? 1 : 0;
        out.mod = "";
        if (sources_.mods != nullptr && !p.approved_hash().empty()) {
            for (const ModPlugin& mp : sources_.mods->plugins) {
                if (mp.loads() && mp.load_folder.wstring() == p.plugin_dir()) out.mod = mp.mod.name.c_str();
            }
        }
    } else {
        // Skipped, or disabled (by the player or a benchmark arm): "skipped"
        // with the `disabled` flag on the wire.
        const Outcome& o = *p.outcome();
        out.name = "";
        out.issues = "";
        out.state = "skipped";
        out.reason = o.reason.c_str();
        out.reason_message = messages_[index].c_str();
        out.disabled = o.kind == Outcome::Kind::disabled ? 1 : 0;
        out.mod = p.mod().c_str();
        out.awaiting_approval = p.awaiting_approval() ? 1 : 0;
    }
    out.disabled_next_start = disabled_next_start(p.id()) ? 1 : 0;
    return true;
}

bool Control::plugin_of(const AnvilPlugin* plugin, AnvilControlPlugin& out) const {
    // Only compared: a caller may pass any pointer.
    if (plugin == nullptr || sources_.plugins == nullptr) return false;
    for (size_t i = 0; i < sources_.plugins->resolved_count(); ++i) {
        const PluginRecord& p = sources_.plugins->at(i);
        if (p.handle() == plugin) return this->plugin(static_cast<uint32_t>(i), out);
        // A module's handle (ADR 0011): its plugin's entry with the module's
        // key, flags and state, so a service that checks its caller
        // (anvil-game.commands, anvil-script) gates the module, not the plugin.
        for (const auto& m : p.modules()) {
            if (m.handle == nullptr || m.handle != plugin) continue;
            if (!this->plugin(static_cast<uint32_t>(i), out)) return false;
            out.id = m.key.c_str();
            out.name = m.name.c_str();
            out.affects_gameplay = m.handle->affects_gameplay ? 1 : 0;
            out.suspended = sources_.policy != nullptr && sources_.policy->suspended(m.handle->owner) ? 1 : 0;
            if (loading_done_.load(std::memory_order_acquire)) out.state = module_state(p);
            return true;
        }
    }
    return false;
}

std::optional<std::string> Control::warning(uint32_t index) const {
    const auto lines = Log::instance().warnings();
    if (index >= lines.size()) return std::nullopt;
    return lines[index];
}

PluginSettings* Control::settings_of(std::string_view plugin_id) const {
    const PluginRecord* p = loadable(plugin_id);
    return p != nullptr ? &p->settings() : nullptr;
}

const AnvilControlCrash* Control::previous_crash() {
    std::lock_guard lock(mutex_);
    if (crash_loaded_) return crash_ ? &crash_->view : nullptr;
    crash_loaded_ = true;
    if (sources_.previous_crash.empty()) return nullptr;
    const std::filesystem::path dir = sources_.anvil_dir / "crashes";
    auto summary = load_crash_summary(dir, sources_.previous_crash);
    if (!summary) return nullptr;
    auto cache = std::make_unique<CrashCache>();
    cache->summary = std::move(*summary);
    for (const auto& id : cache->summary.plugins) {
        if (!cache->plugins.empty()) cache->plugins += ", ";
        cache->plugins += id;
    }
    cache->path = (dir / (sources_.previous_crash + ".txt")).wstring();
    cache->report = read_text_file(cache->path, 1 << 20).value_or(std::string());
    // Shown before? The marker holds the name of the last report a start showed.
    const std::filesystem::path shown = sources_.anvil_dir / "data" / "anvil" / "crash-shown.txt";
    std::string marker = read_text_file(shown, 256).value_or(std::string());
    while (!marker.empty() && (marker.back() == '\n' || marker.back() == '\r')) marker.pop_back();
    const bool is_new = marker != sources_.previous_crash;
    if (is_new) {
        std::error_code ignored;
        std::filesystem::create_directories(shown.parent_path(), ignored);
        const std::string text = sources_.previous_crash + "\n";
        const HANDLE h = CreateFileW(shown.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            CloseHandle(h);
        }
    }
    AnvilControlCrash& v = cache->view;
    v.size = sizeof(v);
    v.name = sources_.previous_crash.c_str();
    v.level = cache->summary.level.c_str();
    v.summary = cache->summary.summary.c_str();
    v.plugins = cache->plugins.c_str();
    v.issues = cache->summary.issues.c_str();
    v.report = cache->report.c_str();
    v.path = cache->path.c_str();
    v.before_game_ready = cache->summary.before_game_ready ? 1 : 0;
    v.is_new = is_new ? 1 : 0;
    crash_ = std::move(cache);
    return &crash_->view;
}

std::string Control::log_tail(uint32_t max_lines) const {
    return read_text_tail(sources_.anvil_dir / "anvil.log", max_lines, kTailBytes);
}

bool Control::disabled_next_start(std::string_view plugin_id) const {
    std::lock_guard lock(mutex_);
    return disabled_next_.contains(plugin_id);
}

AnvilResult Control::disable_next_start(std::string_view plugin_id, std::string& error) {
    if (sources_.plugins == nullptr || sources_.plugins->find(plugin_id) == nullptr) {
        error = "no plugin '" + std::string(plugin_id) + "' at this start";
        return ANVIL_ERROR_NOT_FOUND;
    }
    std::lock_guard lock(mutex_);
    if (!add_disabled_plugin(sources_.anvil_dir / "anvil.toml", plugin_id, error)) {
        Log::instance().write(ANVIL_LOG_WARNING, "anvil", "cannot disable " + std::string(plugin_id) +
                                                              " at the next start: " + error);
        return ANVIL_ERROR_INTERNAL;
    }
    disabled_next_.insert(std::string(plugin_id));
    Log::instance().write(ANVIL_LOG_INFO, "anvil",
                          std::string(plugin_id) + " is disabled from the next start (disabled_plugins in anvil.toml)");
    return ANVIL_OK;
}

// ---- plugins in mods ---------------------------------------------------------------

uint32_t Control::mod_plugin_count() const {
    return sources_.mods != nullptr ? static_cast<uint32_t>(sources_.mods->plugins.size()) : 0;
}

std::string Control::running_hash(const std::string& id) const {
    if (sources_.mods == nullptr || sources_.plugins == nullptr) return {};
    for (const ModPlugin& m : sources_.mods->plugins) {
        if (m.id != id || !m.loads()) continue;
        for (size_t i = 0; i < sources_.plugins->resolved_count(); ++i) {
            const PluginRecord& p = sources_.plugins->at(i);
            if (p.plugin_dir() == m.load_folder.wstring()) return p.approved_hash();
        }
    }
    return {};
}

bool Control::mod_plugin(uint32_t index, AnvilControlModPlugin& out) const {
    if (index >= mod_plugin_count()) return false;
    const ModPlugin& p = sources_.mods->plugins[index];
    const ModView& v = mod_views_[index];
    out = AnvilControlModPlugin{};
    out.size = sizeof(out);
    out.id = p.id.c_str();
    out.name = v.name.c_str();
    out.version = v.version.c_str();
    out.authors = v.authors.c_str();
    out.description = v.description.c_str();
    out.repository = v.repository.c_str();
    out.issues = v.issues.c_str();
    out.mod = p.mod.name.c_str();
    out.workshop_id = p.mod.workshop_id.c_str();
    out.state = state_name(p.state);
    out.reason = p.reason.c_str();
    out.reason_message = v.reason_message.c_str();
    out.hash = p.hash.c_str();
    out.others = v.others.c_str();
    out.approved_version = v.approved_version.c_str();
    out.approved_hash = v.approved_hash.c_str();
    out.approved_date = v.approved_date.c_str();
    out.affects_gameplay = p.manifest && p.manifest->affects_gameplay ? 1 : 0;
    out.symbols = v.symbols;
    if (p.loads() && loading_done_.load(std::memory_order_acquire) && sources_.plugins != nullptr) {
        for (size_t i = 0; i < sources_.plugins->resolved_count(); ++i) {
            const PluginRecord& r = sources_.plugins->at(i);
            const Outcome* o = r.outcome();
            if (r.plugin_dir() == p.load_folder.wstring() && o != nullptr && o->kind == Outcome::Kind::loaded) out.loaded = 1;
        }
    }
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(p.id);
    const bool bound_now = it != bindings_.end() && it->second.mod == p.mod.descriptor;
    const bool bound_at_start = p.bound;
    out.approved_next_start = bound_now && it->second.hash == p.hash && !(bound_at_start && p.approval->hash == p.hash);
    out.revoked_next_start = bound_at_start && !bound_now;
    const bool approvable = p.problem.empty() && !p.hash.empty() && p.manifest && p.state != ModPluginState::Shadowed;
    out.can_approve = approvable && !(bound_now && it->second.hash == p.hash) ? 1 : 0;
    out.can_revoke = bound_now ? 1 : 0;
    return true;
}

// ---- modules (ADR 0011) ------------------------------------------------------------

namespace {

void fill_declared(const Module& m, const char* key, AnvilControlModule& out) {
    out = AnvilControlModule{};
    out.size = sizeof(out);
    out.key = key;
    out.id = m.id.c_str();
    out.name = m.name.c_str();
    out.description = m.description.c_str();
    out.affects_gameplay = m.affects_gameplay ? 1 : 0;
    out.offers_ironman = m.allow_ironman == Allow::OptIn ? 1 : 0;
    out.offers_multiplayer = m.allow_multiplayer == Allow::OptIn ? 1 : 0;
    out.on_by_default = m.enabled_by_default ? 1 : 0;
    out.state = "";
}

}  // namespace

uint32_t Control::module_count(std::string_view plugin_id) const {
    const PluginRecord* p = loadable(plugin_id);
    return p != nullptr ? static_cast<uint32_t>(p->modules().size()) : 0;
}

bool Control::module(std::string_view plugin_id, uint32_t index, AnvilControlModule& out) const {
    const PluginRecord* p = loadable(plugin_id);
    if (p == nullptr || index >= p->modules().size()) return false;
    const PluginRecord::ModuleSlot& m = p->modules()[index];
    fill_declared(*m.declared, m.key.c_str(), out);
    out.enabled = m.handle != nullptr ? 1 : 0;
    out.state = m.handle == nullptr                              ? "disabled"
                : loading_done_.load(std::memory_order_acquire) ? module_state(*p)
                                                                : "pending";
    out.suspended = sources_.policy != nullptr && m.handle != nullptr && sources_.policy->suspended(m.handle->owner) ? 1 : 0;
    return true;
}

uint32_t Control::mod_plugin_module_count(uint32_t index) const {
    if (index >= mod_plugin_count()) return 0;
    const ModPlugin& p = sources_.mods->plugins[index];
    return p.manifest ? static_cast<uint32_t>(p.manifest->modules.size()) : 0;
}

bool Control::mod_plugin_module(uint32_t index, uint32_t module, AnvilControlModule& out) const {
    if (module >= mod_plugin_module_count(index)) return false;
    fill_declared(sources_.mods->plugins[index].manifest->modules[module],
                  mod_views_[index].module_keys[module].c_str(), out);
    return true;
}

AnvilResult Control::approve_mod_plugin(uint32_t index, std::string_view hash, std::string& error) {
    if (index >= mod_plugin_count()) {
        error = "no such plugin from a mod";
        return ANVIL_ERROR_NOT_FOUND;
    }
    const ModPlugin& p = sources_.mods->plugins[index];
    if (hash != p.hash) {
        error = "the plugin's hash is not the one shown";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    const std::string label = p.id + " from mod '" + p.mod.name + "'";
    std::lock_guard lock(mutex_);
    if (!anvil::approve_mod_plugin(sources_.anvil_dir, p, p.hash, running_hash(p.id), error)) {
        Log::instance().write(ANVIL_LOG_WARNING, "anvil", "cannot approve " + label + ": " + error);
        return ANVIL_ERROR_INTERNAL;
    }
    bindings_[p.id] = {p.mod.descriptor, p.hash};
    Log::instance().write(ANVIL_LOG_INFO, "anvil",
                          label + " " + (p.manifest ? p.manifest->version.to_string() : std::string()) + " is approved (hash " +
                              short_hash(p.hash) + "); it loads from the next start");
    return ANVIL_OK;
}

AnvilResult Control::revoke_mod_plugin(uint32_t index, std::string& error) {
    if (index >= mod_plugin_count()) {
        error = "no such plugin from a mod";
        return ANVIL_ERROR_NOT_FOUND;
    }
    const ModPlugin& p = sources_.mods->plugins[index];
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(p.id);
    if (it == bindings_.end() || it->second.mod != p.mod.descriptor) {
        error = "no approval binds " + p.id + " to mod '" + p.mod.name + "'";
        return ANVIL_ERROR_NOT_FOUND;
    }
    if (!anvil::revoke_mod_plugin(sources_.anvil_dir, p.id, running_hash(p.id), error)) {
        Log::instance().write(ANVIL_LOG_WARNING, "anvil", "cannot revoke " + p.id + ": " + error);
        return ANVIL_ERROR_INTERNAL;
    }
    bindings_.erase(it);
    Log::instance().write(ANVIL_LOG_INFO, "anvil",
                          "the approval of " + p.id + " from mod '" + p.mod.name + "' is revoked; it does not load from the next start");
    return ANVIL_OK;
}

// ---- helpers ---------------------------------------------------------------------

std::string setting_text(const Setting& setting, const SettingValue& value) {
    switch (setting.type) {
    case SettingType::Bool: return value.integer != 0 ? "true" : "false";
    case SettingType::Int: return std::to_string(value.integer);
    case SettingType::Float: {
        char text[64];
        const auto [end, ec] = std::to_chars(text, text + sizeof(text), value.real);
        return std::string(text, ec == std::errc() ? end : text);
    }
    case SettingType::Enum:
        return value.integer >= 0 && static_cast<size_t>(value.integer) < setting.options.size()
                   ? setting.options[static_cast<size_t>(value.integer)].name
                   : std::string();
    case SettingType::String: return value.text;
    case SettingType::Key: return key_name(value.integer);
    }
    return {};
}

std::string plugin_set_hash(std::vector<std::string> ids_and_versions) {
    if (ids_and_versions.empty()) return {};
    std::sort(ids_and_versions.begin(), ids_and_versions.end());
    std::string text;
    for (const auto& line : ids_and_versions) text += line + "\n";
    const std::string sha = sha256_bytes(text.data(), text.size());
    if (sha.size() < 16) return {};
    return sha.substr(0, 4) + "-" + sha.substr(4, 4) + "-" + sha.substr(8, 4) + "-" + sha.substr(12, 4);
}

}  // namespace anvil
