#include "i18n/message.hpp"

#include <windows.h>

#include <cctype>
#include <fstream>
#include <iterator>

namespace anvil::i18n {
namespace {

// Every code the core, `anvil check` and the Manager's checks give, with its
// English words. The words are the catalogs' keys (i18n/catalog_*.cpp): a
// change here needs the translations changed too, which the tests enforce.
const std::vector<Pattern> kPatterns = {
    // The resolver (design §10).
    {"disabled", "disabled in anvil.toml"},
    {"all_disabled", "anvil.toml has enabled = false: no plugins load"},
    {"benchmark_without", "left out by the start-up benchmark (ANVIL_BENCHMARK=without:{0})"},
    {"duplicate_id", "duplicate id; already provided by {0}"},
    {"api_mismatch", "built for plugin API {0}, Anvil provides API {1}"},
    {"anvil_version", "requires Anvil {0}, this is Anvil {1}"},
    {"conflicts", "conflicts with {0} {1}; disable one of them"},
    {"needs_anvil_game",
     "affects gameplay, so it needs plugin '{0}', which is not loaded (Anvil suspends gameplay plugins in ironman and "
     "multiplayer through it)"},
    {"requires_missing", "requires plugin '{0}' ({1}), which is not loaded"},
    {"requires_version", "requires '{0}' {1}, found {2}"},
    {"early_needs_main", "early plugin cannot depend on main-phase plugin '{0}'"},
    {"cycle_early", "dependency/load_after cycle in early phase"},
    {"cycle_main", "dependency/load_after cycle in main phase"},
    // Discovery and the checks of a plugin's files.
    {"entry_missing", "entry {0} not found"},
    {"plugin_toml_missing", "plugin.toml not found"},
    {"cannot_read", "cannot read {0}"},
    {"build_unknown", "game build unknown (EU4 updated?)"},
    {"symbols_build", "symbols.toml is for build {0}, the game is {1}"},
    {"symbols_builds", "symbols.toml is for builds {0}, the game is {1}"},
    {"image_unreadable", "cannot read the game image: {0}"},
    {"symbol_mismatch", "{0} (EU4 updated or eu4.exe modified?)"},
    {"symbol_mismatch_more", "{0} (EU4 updated or eu4.exe modified?) and {1} more"},
    // Safe mode (design §12).
    {"safe_mode", "safe mode"},
    {"safe_mode_crash", "safe mode; the crash before it was {0} (report {1})"},
    {"crash_none", "no plugin involved"},
    {"crash_in_code", "in plugin {0}'s code"},
    {"crash_in_change", "in plugin {0}'s change"},
    {"crash_on_stack", "plugin {0} on the stack"},
    {"crash_also", "{0} (also {1})"},
    // Loading (design §10).
    {"dependency_failed", "requires plugin '{0}', which failed to load"},
    {"load_error", "cannot load {0} (error {1})"},
    {"no_load_export", "the entry DLL does not export anvil_plugin_load"},
    {"load_exception", "anvil_plugin_load crashed with exception {0}"},
    {"load_returned", "anvil_plugin_load returned {0}"},
    {"copy_changed", "its approved copy changed ({0}, approved {1}); approve it again"},
    {"hash", "hash {0}"},
    // Plugins in mods (design §14).
    {"from_mod", "from mod '{0}' ({1}): {2}"},
    {"local_mod", "local mod"},
    {"workshop_item", "Workshop {0}"},
    {"copy_failed", "cannot make its approved copy: {0}"},
    {"copy_broken", "its approved copy: {0}"},
    {"bad_folder_id", "the folder name is not a plugin id ([a-z0-9-]+, at most 64 characters)"},
    {"id_mismatch", "plugin.toml has id '{0}', but its folder is anvil/{1}"},
    {"approved_other_mod", "approved for another mod ('{0}'); approve it for this mod to use it"},
    {"waiting_approval", "waiting for approval (hash {0})"},
    {"approved_on", "approved on {0} (hash {1})"},
    {"update_waits", "the mod has version {0} (hash {1}), which is not approved; the approved {2} loads until then"},
    {"update_invalid",
     "the mod has a version that cannot be approved ({0}), which is not approved; the approved {1} loads until then"},
    {"approved_gone", "{0}; the approved {1} is gone (no approved copy)"},
    {"changed_copy_missing",
     "changed since {0} was approved and the approved copy is missing; waiting for approval (hash {1})"},
    {"shadowed", "the local plugin in anvil/plugins has this id and wins"},
    {"conflict_bound", "mod '{0}' has this plugin too and is approved for it; approve this one instead to use it"},
    {"conflict_none",
     "{0} enabled mods have a plugin '{1}' ({2}) and none is approved for it, so none loads; approve the one to use"},
};

constexpr char kOpen = '\x02';
constexpr char kNext = '\x1F';
constexpr char kClose = '\x03';

void serialize_into(const Message& m, std::string& out) {
    if (m.code.empty()) {
        for (const char c : m.text) {
            if (c != kOpen && c != kNext && c != kClose) out += c;
        }
        return;
    }
    out += kOpen;
    out += m.code;
    for (const Message& a : m.args) {
        out += kNext;
        serialize_into(a, out);
    }
    out += kClose;
}

Message parse_at(std::string_view text, size_t& at) {
    Message m;
    if (at >= text.size() || text[at] != kOpen) {
        while (at < text.size() && text[at] != kNext && text[at] != kClose) m.text += text[at++];
        return m;
    }
    ++at;
    while (at < text.size() && text[at] != kNext && text[at] != kClose) m.code += text[at++];
    while (at < text.size() && text[at] == kNext) {
        ++at;
        m.args.push_back(parse_at(text, at));
    }
    if (at < text.size() && text[at] == kClose) ++at;
    return m;
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

const std::vector<Pattern>& patterns() { return kPatterns; }

const char* pattern_of(std::string_view code) {
    for (const Pattern& p : kPatterns) {
        if (code == p.code) return p.english;
    }
    return nullptr;
}

std::string format(std::string_view pattern, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(pattern.size() + 32);
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '{' && i + 1 < pattern.size() && pattern[i + 1] == '{') {
            out += '{';
            ++i;
        } else if (c == '{' && i + 2 < pattern.size() && pattern[i + 1] >= '0' && pattern[i + 1] <= '9' &&
                   pattern[i + 2] == '}' && static_cast<size_t>(pattern[i + 1] - '0') < args.size()) {
            out += args[static_cast<size_t>(pattern[i + 1] - '0')];
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

std::string english(const Message& message) {
    if (message.code.empty()) return message.text;
    std::vector<std::string> args;
    for (const Message& a : message.args) args.push_back(english(a));
    if (const char* pattern = pattern_of(message.code)) return i18n::format(pattern, args);
    std::string out = message.code;
    for (const std::string& a : args) out += " " + a;
    return out;
}

std::string serialize(const Message& message) {
    std::string out;
    serialize_into(message, out);
    return out;
}

Message parse(std::string_view text) {
    size_t at = 0;
    return parse_at(text, at);
}

Message crash_message(std::string_view level, const std::vector<std::string>& plugins, std::string_view summary) {
    const char* code = level == "in_plugin_code"     ? "crash_in_code"
                       : level == "in_plugin_change" ? "crash_in_change"
                       : level == "plugin_on_stack"  ? "crash_on_stack"
                                                     : nullptr;
    Message m;
    if (level == "no_plugin" || (code != nullptr && plugins.empty())) {
        m = Message("crash_none", {});
    } else if (code != nullptr) {
        m = Message(code, {plugins.front()});
        std::string others;
        for (size_t i = 1; i < plugins.size(); ++i) others += (i == 1 ? "" : ", ") + plugins[i];
        if (!others.empty()) m = Message("crash_also", {m, others});
    }
    // A report whose words are not these (another Anvil wrote it) is shown as it says.
    if (m.empty() || (!summary.empty() && english(m) != summary)) return std::string(summary);
    return m;
}

bool is_cjk(Language language) {
    return language == Language::SimplifiedChinese || language == Language::TraditionalChinese ||
           language == Language::Japanese || language == Language::Korean;
}

const char* language_code(Language language) {
    switch (language) {
    case Language::English: return "en";
    case Language::French: return "fr";
    case Language::German: return "de";
    case Language::Spanish: return "es";
    case Language::SimplifiedChinese: return "zh-Hans";
    case Language::TraditionalChinese: return "zh-Hant";
    case Language::Japanese: return "ja";
    case Language::Korean: return "ko";
    }
    return "en";
}

std::optional<Language> language_named(std::string_view name) {
    name = trimmed(name);
    std::string code(name);
    for (char& c : code) {
        c = c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (code == "en" || code == "l-english") return Language::English;
    if (code == "fr" || code == "l-french") return Language::French;
    if (code == "de" || code == "l-german") return Language::German;
    if (code == "es" || code == "l-spanish") return Language::Spanish;
    if (code == "zh-hans") return Language::SimplifiedChinese;
    if (code == "zh-hant") return Language::TraditionalChinese;
    if (code == "ja") return Language::Japanese;
    if (code == "ko") return Language::Korean;
    return std::nullopt;
}

std::optional<Language> language_of_settings(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    // A top-level `language="l_french"` line, as EU4 writes it.
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.starts_with("language")) continue;
        line = trimmed(line.substr(8));
        if (!line.starts_with('=')) continue;
        line = trimmed(line.substr(1));
        if (line.size() >= 2 && line.front() == '"' && line.back() == '"') line = line.substr(1, line.size() - 2);
        if (!line.starts_with("l_")) return std::nullopt;
        return language_named(line);
    }
    return std::nullopt;
}

std::optional<Language> cjk_language_of_steam(std::string_view steam_language) {
    steam_language = trimmed(steam_language);
    if (steam_language == "schinese") return Language::SimplifiedChinese;
    if (steam_language == "tchinese") return Language::TraditionalChinese;
    if (steam_language == "japanese") return Language::Japanese;
    if (steam_language == "koreana" || steam_language == "korean") return Language::Korean;
    return std::nullopt;
}

std::optional<Language> cjk_language_of_locale(std::string_view locale_name) {
    std::string name(trimmed(locale_name));
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto is = [&](std::string_view language) {
        return name == language || (name.starts_with(language) && name.size() > language.size() && name[language.size()] == '-');
    };
    if (is("ja")) return Language::Japanese;
    if (is("ko")) return Language::Korean;
    if (!is("zh")) return std::nullopt;
    // zh-Hant, or the regions that write it: Taiwan, Hong Kong, Macao.
    if (is("zh-hant") || is("zh-tw") || is("zh-hk") || is("zh-mo")) return Language::TraditionalChinese;
    return Language::SimplifiedChinese;
}

std::optional<Language> forced_language() {
    char forced[16] = {};
    const DWORD n = GetEnvironmentVariableA("ANVIL_LANGUAGE", forced, sizeof(forced));
    if (n == 0 || n >= sizeof(forced)) return std::nullopt;
    return language_named(forced);
}

SystemLanguages read_system_languages() {
    SystemLanguages out;
    char steam[64] = {};
    DWORD size = sizeof(steam);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "Language", RRF_RT_REG_SZ, nullptr, steam, &size) ==
        ERROR_SUCCESS) {
        out.steam = steam;
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        for (const wchar_t* c = locale; *c != 0; ++c) out.windows += static_cast<char>(*c < 0x80 ? *c : '?');
    }
    return out;
}

LanguageChoice choose_language(const std::filesystem::path& user_dir, bool cjk_text, const SystemLanguages& system) {
    if (const auto language = forced_language()) return {*language, "ANVIL_LANGUAGE"};
    bool says_english = false;  // settings.txt names English
    if (!user_dir.empty()) {
        std::ifstream file(user_dir / "settings.txt", std::ios::binary);
        if (file) {
            std::string text;
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            const auto language = language_of_settings(text.substr(0, 1 << 16));
            if (language && *language != Language::English) return {*language, "settings.txt"};
            says_english = language.has_value();
        }
    }
    if (cjk_text) {
        if (const auto language = cjk_language_of_steam(system.steam)) return {*language, "Steam"};
        if (const auto language = cjk_language_of_locale(system.windows)) return {*language, "Windows"};
    }
    return {Language::English, says_english ? "settings.txt" : "default"};
}

LanguageChoice choose_language(const std::filesystem::path& user_dir, bool cjk_text) {
    return choose_language(user_dir, cjk_text, cjk_text ? read_system_languages() : SystemLanguages{});
}

}  // namespace anvil::i18n
