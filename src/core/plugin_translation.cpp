#include "plugin_translation.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace anvil {
namespace {

uint32_t line_of(const toml::source_region& source) { return static_cast<uint32_t>(source.begin.line); }

bool read_limited(const std::filesystem::path& file, std::string& out, bool& too_big) {
    too_big = false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) return false;
    if (size > kMaxPluginTomlBytes) {
        too_big = true;
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    out = std::move(text).str();
    return true;
}

std::string shown_text(std::string_view text) {
    constexpr size_t kMax = 60;
    std::string out = "\"";
    if (text.size() <= kMax) {
        out += text;
    } else {
        // Cut on a UTF-8 boundary.
        size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        out += text.substr(0, cut);
        out += "...";
    }
    return out + "\"";
}

}  // namespace

const std::vector<std::string>& translation_codes() {
    static const std::vector<std::string> codes = {"fr", "de", "es", "zh-Hans", "zh-Hant", "ja", "ko", "pl"};
    return codes;
}

std::string_view PluginTranslation::translate(std::string_view english) const {
    const auto it = text.find(english);
    return it != text.end() ? std::string_view(it->second) : english;
}

PluginTranslationResult parse_plugin_translation(std::string_view text) {
    PluginTranslationResult result;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);  // saved by Notepad
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        result.error = std::string(e.description());
        result.line = line_of(e.source());
        return result;
    }
    PluginTranslation t;
    bool has_status = false;
    for (const auto& [key, node] : root) {
        const std::string name(key.str());
        if (name == "status") {
            const auto* value = node.as_string();
            if (value == nullptr || (value->get() != "draft" && value->get() != "checked")) {
                result.error = "status must be \"draft\" or \"checked\"";
                result.line = line_of(key.source());
                return result;
            }
            t.status = value->get() == "checked" ? TranslationStatus::Checked : TranslationStatus::Draft;
            has_status = true;
        } else if (name == "text") {
            const auto* table = node.as_table();
            if (table == nullptr) {
                result.error = "text must be a table of English texts and their translations";
                result.line = line_of(key.source());
                return result;
            }
            for (const auto& [english, value] : *table) {
                const uint32_t line = line_of(english.source());
                const auto* translated = value.as_string();
                if (translated == nullptr) {
                    result.error = "the translation of " + shown_text(english.str()) + " is not a string";
                    result.line = line;
                    return result;
                }
                const std::string& words = translated->get();
                if (english.str().empty() || words.empty()) {
                    result.error = english.str().empty() ? "an empty English text" : "an empty translation of " + shown_text(english.str());
                    result.line = line;
                    return result;
                }
                if (words.find('\0') != std::string::npos || english.str().find('\0') != std::string_view::npos) {
                    result.error = "a NUL character in " + shown_text(english.str());
                    result.line = line;
                    return result;
                }
                t.text.emplace(std::string(english.str()), words);
                t.lines.emplace(std::string(english.str()), line);
            }
        } else {
            result.error = "unknown key '" + name + "' (status and [text] are allowed)";
            result.line = line_of(key.source());
            return result;
        }
    }
    if (!has_status) {
        result.error = "missing status (\"draft\" or \"checked\")";
        return result;
    }
    result.translation = std::move(t);
    return result;
}

PluginTranslationResult load_plugin_translation(const std::filesystem::path& folder, std::string_view code) {
    PluginTranslationResult result;
    const auto& codes = translation_codes();
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) return result;
    const std::filesystem::path file =
        folder / kTranslationsFolder / std::filesystem::path(std::u8string(code.begin(), code.end()) + u8".toml");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return result;
    std::string text;
    bool too_big = false;
    if (!read_limited(file, text, too_big)) {
        result.error = too_big ? "larger than " + std::to_string(kMaxPluginTomlBytes >> 20) + " MB" : "cannot be read";
        return result;
    }
    return parse_plugin_translation(text);
}

std::vector<std::string> plugin_texts(const Manifest& manifest, const SettingsFile* settings) {
    std::vector<std::string> out;
    std::set<std::string, std::less<>> seen;
    const auto add = [&](const std::string& text) {
        if (!text.empty() && seen.insert(text).second) out.push_back(text);
    };
    add(manifest.description);
    if (settings != nullptr) {
        for (const Setting& s : settings->settings) {
            add(s.label);
            add(s.description);
            add(s.group);
            for (const SettingOption& o : s.options) add(o.label);
        }
    }
    return out;
}

std::vector<TranslationProblem> check_plugin_translations(const std::filesystem::path& folder,
                                                          const std::vector<std::string>& texts, bool complete) {
    std::vector<TranslationProblem> problems;
    const std::string dir = kTranslationsFolder;
    std::error_code ec;
    const std::filesystem::path translations = folder / kTranslationsFolder;
    if (!std::filesystem::is_directory(translations, ec)) {
        if (!texts.empty()) {
            problems.push_back({dir, 0,
                                "no translations/<code>.toml: the overlay and the Manager show this plugin's texts in "
                                "English in every language",
                                complete});
        }
        return problems;
    }
    const auto& codes = translation_codes();
    std::set<std::string> found;
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(translations, ec)) entries.push_back(entry.path());
    std::sort(entries.begin(), entries.end());
    for (const auto& path : entries) {
        const auto name8 = path.filename().u8string();
        const std::string name(name8.begin(), name8.end());
        const std::string file = dir + "/" + name;
        if (!std::filesystem::is_regular_file(path, ec)) {
            problems.push_back({file, 0, "not a translation file (translations/ holds <code>.toml files only)"});
            continue;
        }
        const std::string code = name.ends_with(".toml") ? name.substr(0, name.size() - 5) : std::string();
        if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
            std::string known;
            for (const auto& c : codes) known += (known.empty() ? "" : ", ") + c;
            problems.push_back({file, 0, "not a language Anvil shows: name the file <code>.toml, with a code of " + known});
            continue;
        }
        found.insert(code);
        std::string text;
        bool too_big = false;
        if (!read_limited(path, text, too_big)) {
            problems.push_back({file, 0, too_big ? "larger than 16 MB" : "cannot be read"});
            continue;
        }
        const PluginTranslationResult parsed = parse_plugin_translation(text);
        if (!parsed.translation) {
            problems.push_back({file, parsed.line, parsed.error});
            continue;
        }
        std::set<std::string_view> wanted(texts.begin(), texts.end());
        for (const auto& [english, line] : parsed.translation->lines) {
            if (!wanted.contains(english)) {
                problems.push_back({file, line,
                                    shown_text(english) +
                                        " is not a text of this plugin (any more): its English words changed or it was removed"});
            }
        }
        for (const std::string& english : texts) {
            if (!parsed.translation->text.contains(english)) {
                problems.push_back({file, 0, "no translation of " + shown_text(english), complete});
            }
        }
    }
    if (!texts.empty()) {
        for (const auto& code : codes) {
            if (!found.contains(code)) {
                problems.push_back({dir + "/" + code + ".toml", 0, "missing: the plugin's texts stay English in this language",
                                    complete});
            }
        }
    }
    return problems;
}

}  // namespace anvil
