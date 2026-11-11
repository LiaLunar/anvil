#include "control_client.hpp"

#include "panels.hpp"

#include <atomic>
#include <cstddef>

namespace overlay {
namespace {

void to_string(void* context, const char* text, size_t size) {
    static_cast<std::string*>(context)->assign(text, size);
}

std::string text_or_empty(const char* text) { return text != nullptr ? std::string(text) : std::string(); }

std::atomic<bool> g_translated_cjk{false};

// The core has the functions of 1.20 (appended to the table).
bool has_plugin_text(const AnvilControlService* service) {
    return service != nullptr && service->size >= offsetof(AnvilControlService, mod_plugin_text) + sizeof(void*);
}

const char* noted(const char* english, const char* text) {
    if (text == nullptr) return english;
    if (text != english && !g_translated_cjk.load(std::memory_order_relaxed) && has_cjk(text)) {
        g_translated_cjk.store(true, std::memory_order_release);
    }
    return text;
}

std::string result_text(AnvilResult result, const AnvilControlResult& out) {
    if (result == ANVIL_OK) return {};
    if (out.error[0] != '\0') return out.error;
    return anvil::i18n::trf("refused ({0})", {std::to_string(static_cast<int>(result))});
}

}  // namespace

anvil::i18n::Language ControlClient::language() const {
    if (service_ == nullptr || service_->size < offsetof(AnvilControlService, language) + sizeof(void*)) {
        return anvil::i18n::Language::English;
    }
    return anvil::i18n::language_named(service_->language()).value_or(anvil::i18n::Language::English);
}

AnvilControlStatus ControlClient::status() const {
    AnvilControlStatus s{};
    s.size = sizeof(s);
    if (service_ != nullptr) service_->status(&s);
    return s;
}

std::vector<PluginRow> ControlClient::plugins() const {
    std::vector<PluginRow> rows;
    if (service_ == nullptr) return rows;
    for (uint32_t i = 0;; ++i) {
        AnvilControlPlugin p{};
        p.size = sizeof(p);
        if (service_->plugin(i, &p) == 0) break;
        PluginRow row;
        row.id = text_or_empty(p.id);
        row.name = text_or_empty(p.name);
        row.version = text_or_empty(p.version);
        row.state = text_or_empty(p.state);
        row.reason = text_or_empty(p.reason);
        row.issues = text_or_empty(p.issues);
        row.early = p.early != 0;
        row.gameplay = p.affects_gameplay != 0;
        row.suspended = p.suspended != 0;
        row.disabled = p.disabled != 0;
        row.disabled_next = p.disabled_next_start != 0;
        row.settings = p.settings;
        if (p.size > offsetof(AnvilControlPlugin, awaiting_approval)) {
            row.mod = text_or_empty(p.mod);
            row.awaiting_approval = p.awaiting_approval != 0;
        }
        if (p.size > offsetof(AnvilControlPlugin, reason_message)) row.why = anvil::i18n::parse(text_or_empty(p.reason_message));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::string> ControlClient::warnings() const {
    std::vector<std::string> lines;
    if (service_ == nullptr) return lines;
    for (uint32_t i = 0;; ++i) {
        std::string line;
        if (service_->warning(i, to_string, &line) == 0) break;
        lines.push_back(std::move(line));
    }
    return lines;
}

std::optional<CrashInfo> ControlClient::previous_crash() const {
    if (service_ == nullptr) return std::nullopt;
    AnvilControlCrash c{};
    c.size = sizeof(c);
    if (service_->previous_crash(&c) == 0) return std::nullopt;
    CrashInfo info;
    info.name = text_or_empty(c.name);
    info.level = text_or_empty(c.level);
    info.summary = text_or_empty(c.summary);
    info.issues = text_or_empty(c.issues);
    info.report = text_or_empty(c.report);
    info.plugins = split_list(text_or_empty(c.plugins));
    if (c.path != nullptr) info.path = c.path;
    info.before_game_ready = c.before_game_ready != 0;
    info.is_new = c.is_new != 0;
    return info;
}

std::string ControlClient::log_tail(uint32_t max_lines) const {
    std::string text;
    if (service_ != nullptr) service_->log_tail(max_lines, to_string, &text);
    return text;
}

uint32_t ControlClient::setting_count(const std::string& plugin) const {
    return service_ != nullptr ? service_->setting_count(plugin.c_str()) : 0;
}

std::optional<AnvilControlSetting> ControlClient::setting_info(const std::string& plugin, AnvilSetting setting) const {
    AnvilControlSetting info{};
    info.size = sizeof(info);
    if (service_ == nullptr || service_->setting_info(plugin.c_str(), setting, &info) == 0) return std::nullopt;
    info.label = plugin_text(plugin, info.label);
    info.description = plugin_text(plugin, info.description);
    info.group = plugin_text(plugin, info.group);
    return info;
}

const char* ControlClient::plugin_text(const std::string& plugin, const char* english) const {
    if (english == nullptr || english[0] == '\0' || !has_plugin_text(service_)) return english;
    return noted(english, service_->plugin_text(plugin.c_str(), english));
}

bool ControlClient::translated_cjk() { return g_translated_cjk.load(std::memory_order_acquire); }

std::vector<std::pair<std::string, std::string>> ControlClient::setting_options(const std::string& plugin,
                                                                                AnvilSetting setting) const {
    std::vector<std::pair<std::string, std::string>> options;
    if (service_ == nullptr) return options;
    for (uint32_t i = 0;; ++i) {
        const char* name = nullptr;
        const char* label = nullptr;
        if (service_->setting_option(plugin.c_str(), setting, i, &name, &label) == 0) break;
        options.emplace_back(text_or_empty(name), text_or_empty(plugin_text(plugin, label)));
    }
    return options;
}

std::string ControlClient::setting_value(const std::string& plugin, AnvilSetting setting, uint32_t which) const {
    std::string text;
    if (service_ != nullptr) service_->setting_value(plugin.c_str(), setting, which, to_string, &text);
    return text;
}

std::vector<std::string> ControlClient::setting_warnings(const std::string& plugin) const {
    std::vector<std::string> lines;
    if (service_ == nullptr) return lines;
    for (uint32_t i = 0;; ++i) {
        std::string line;
        if (service_->setting_warning(plugin.c_str(), i, to_string, &line) == 0) break;
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string ControlClient::set(const std::string& plugin, AnvilSetting setting, const AnvilControlValue& value) const {
    if (service_ == nullptr) return "no anvil.control";
    AnvilControlResult out{};
    out.size = sizeof(out);
    return result_text(service_->setting_set(plugin.c_str(), setting, &value, &out), out);
}

std::string ControlClient::set_text(const std::string& plugin, AnvilSetting setting, const std::string& text) const {
    if (service_ == nullptr) return "no anvil.control";
    AnvilControlResult out{};
    out.size = sizeof(out);
    return result_text(service_->setting_set_text(plugin.c_str(), setting, text.c_str(), &out), out);
}

std::string ControlClient::reset(const std::string& plugin, AnvilSetting setting) const {
    if (service_ == nullptr) return "no anvil.control";
    AnvilControlResult out{};
    out.size = sizeof(out);
    return result_text(service_->setting_reset(plugin.c_str(), setting, &out), out);
}

std::string ControlClient::disable_next_start(const std::string& plugin) const {
    if (service_ == nullptr) return "no anvil.control";
    AnvilControlResult out{};
    out.size = sizeof(out);
    return result_text(service_->disable_next_start(plugin.c_str(), &out), out);
}

namespace {
// The core has the functions of 1.3 (appended to the table).
bool has_mod_plugins(const AnvilControlService* service) {
    return service != nullptr && service->size >= offsetof(AnvilControlService, revoke_mod_plugin) + sizeof(void*);
}
}  // namespace

std::vector<ModRow> ControlClient::mod_plugins() const {
    std::vector<ModRow> rows;
    if (!has_mod_plugins(service_)) return rows;
    const uint32_t count = service_->mod_plugin_count();
    for (uint32_t i = 0; i < count; ++i) {
        AnvilControlModPlugin m{};
        m.size = sizeof(m);
        if (service_->mod_plugin(i, &m) == 0) break;
        ModRow row;
        row.index = i;
        row.id = text_or_empty(m.id);
        row.name = text_or_empty(m.name);
        row.version = text_or_empty(m.version);
        row.authors = text_or_empty(m.authors);
        row.description = text_or_empty(m.description != nullptr && m.description[0] != '\0' && has_plugin_text(service_)
                                             ? noted(m.description, service_->mod_plugin_text(i, m.description))
                                             : m.description);
        row.repository = text_or_empty(m.repository);
        row.issues = text_or_empty(m.issues);
        row.mod = text_or_empty(m.mod);
        row.workshop_id = text_or_empty(m.workshop_id);
        row.state = text_or_empty(m.state);
        row.reason = text_or_empty(m.reason);
        row.hash = text_or_empty(m.hash);
        row.others = text_or_empty(m.others);
        row.approved_version = text_or_empty(m.approved_version);
        row.approved_hash = text_or_empty(m.approved_hash);
        row.approved_date = text_or_empty(m.approved_date);
        row.gameplay = m.affects_gameplay != 0;
        row.symbols = m.symbols;
        row.loaded = m.loaded != 0;
        row.can_approve = m.can_approve != 0;
        row.can_revoke = m.can_revoke != 0;
        row.approved_next = m.approved_next_start != 0;
        row.revoked_next = m.revoked_next_start != 0;
        if (m.size > offsetof(AnvilControlModPlugin, reason_message)) row.why = anvil::i18n::parse(text_or_empty(m.reason_message));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string ControlClient::approve_mod_plugin(const ModRow& plugin) const {
    if (!has_mod_plugins(service_)) return anvil::i18n::tr("this Anvil has no plugins from mods");
    AnvilControlResult out{};
    out.size = sizeof(out);
    return result_text(service_->approve_mod_plugin(plugin.index, plugin.hash.c_str(), &out), out);
}

std::string ControlClient::revoke_mod_plugin(const ModRow& plugin) const {
    if (!has_mod_plugins(service_)) return anvil::i18n::tr("this Anvil has no plugins from mods");
    AnvilControlResult out{};
    out.size = sizeof(out);
    return result_text(service_->revoke_mod_plugin(plugin.index, &out), out);
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> items;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(at, end - at);
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) items.push_back(std::move(item));
        at = end + 1;
    }
    return items;
}

}  // namespace overlay
