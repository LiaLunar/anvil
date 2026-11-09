// The player's language (issue #158): messages by code, their English words,
// the language chosen from EU4's settings (and with Mei from Steam or Windows),
// and the catalogs, which must translate every text the overlay and the
// Manager mark (tr, trf, N_) with the same placeholders and only characters
// their fonts have: the Latin font's, or for Chinese, Japanese and Korean
// those of the language's font in src/i18n/fonts, read with ImGui's own
// stb_truetype.
#include "core/settings.hpp"
#include "i18n/cjk_font.hpp"
#include "i18n/message.hpp"
#include "i18n/translate.hpp"
#include "test_support.hpp"

#include <windows.h>

#pragma warning(push, 0)
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>
#pragma warning(pop)

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace anvil::i18n;
namespace fs = std::filesystem;

// {0}..{9} in a text, in order of appearance, without repeats.
std::set<std::string> placeholders(std::string_view text) {
    std::set<std::string> out;
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        if (text[i] == '{' && text[i + 1] >= '0' && text[i + 1] <= '9' && text[i + 2] == '}') out.insert(std::string(text.substr(i, 3)));
    }
    return out;
}

// The code points of a UTF-8 text; 0xFFFFFFFF for a broken sequence.
std::vector<uint32_t> code_points(std::string_view text) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > text.size()) {
            out.push_back(0xFFFFFFFF);
            break;
        }
        uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

// What both fonts draw: the Manager's ranges (tools/manager/app/main.cpp)
// within the overlay's (plugins/anvil-overlay/src/ui.cpp).
bool in_fonts(uint32_t cp) {
    return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0x17F) || cp == 0x2013 || cp == 0x2014 ||
           (cp >= 0x2018 && cp <= 0x201E) || cp == 0x2022 || cp == 0x2026;
}

// The C string literals right after tr(, trf( and N_( in a source file,
// adjacent literals joined, escapes resolved.
std::vector<std::string> marked_texts(const std::string& source) {
    std::vector<std::string> out;
    for (const std::string_view call : {"tr(", "trf(", "N_("}) {
        for (size_t at = source.find(call); at != std::string::npos; at = source.find(call, at + 1)) {
            if (at > 0) {
                const char before = source[at - 1];
                if (std::isalnum(static_cast<unsigned char>(before)) || before == '_') continue;
            }
            size_t i = at + call.size();
            std::string text;
            bool any = false;
            for (;;) {
                while (i < source.size() && std::isspace(static_cast<unsigned char>(source[i]))) ++i;
                if (i >= source.size() || source[i] != '"') break;
                any = true;
                for (++i; i < source.size() && source[i] != '"'; ++i) {
                    if (source[i] != '\\') {
                        text += source[i];
                        continue;
                    }
                    const char e = source[++i];
                    if (e == 'n') text += '\n';
                    else if (e == 't') text += '\t';
                    else if (e == 'x') {
                        unsigned value = 0;
                        while (i + 1 < source.size() && std::isxdigit(static_cast<unsigned char>(source[i + 1]))) {
                            value = value * 16 + static_cast<unsigned>(std::stoi(std::string(1, source[++i]), nullptr, 16));
                        }
                        text += static_cast<char>(value);
                    } else {
                        text += e;
                    }
                }
                ++i;
            }
            if (any) out.push_back(text);
        }
    }
    return out;
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// Every English text the catalogs must translate: the marked texts of the
// overlay's and the Manager's sources (and of anvil_net, which both use), and
// the patterns of every code.
std::set<std::string> catalog_keys(const fs::path& root) {
    std::set<std::string> keys;
    for (const fs::path dir : {root / "plugins" / "anvil-overlay" / "src", root / "tools" / "manager" / "app",
                               root / "tools" / "manager" / "src", root / "src" / "net"}) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".cpp" && entry.path().extension() != ".hpp") continue;
            for (std::string& text : marked_texts(read_file(entry.path()))) keys.insert(std::move(text));
        }
    }
    for (const Pattern& p : patterns()) keys.insert(p.english);
    return keys;
}

const char* name_of(Language l) { return language_code(l); }

// A font that stb_truetype, which ImGui uses, can read.
struct TrueType {
    stbtt_fontinfo info{};
    bool ok = false;
    explicit TrueType(const FontData& font) {
        const auto* data = static_cast<const unsigned char*>(font.data);
        ok = font.size > 0 && stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) != 0;
    }
    bool has(uint32_t cp) const { return ok && stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0; }
    // A name of the font (name table, Windows platform, English) as ASCII.
    std::string name(int id) const {
        int length = 0;
        const char* raw = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                                  STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, id);
        std::string out;
        for (int i = 1; raw != nullptr && i < length; i += 2) out += raw[i];  // UTF-16BE
        return out;
    }
};

}  // namespace

int main(int argc, char** argv) {
    // `anvil_i18n_tests --keys`: every text a catalog must translate, one per line, for translators.
    if (argc > 1 && std::string_view(argv[1]) == "--keys") {
        for (const std::string& key : catalog_keys(ANVIL_SOURCE_DIR)) std::printf("%s\n", key.c_str());
        return 0;
    }
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path(ANVIL_SOURCE_DIR);

    run("format: placeholders, a missing argument, braces", [] {
        CHECK(format("{0} and {1}", {"a", "b"}) == "a and b");
        CHECK(format("{1} before {0}", {"a", "b"}) == "b before a");
        CHECK(format("{0} {0}", {"x"}) == "x x");
        CHECK(format("{2} stays", {"a"}) == "{2} stays");
        CHECK(format("{{0}} is {0}", {"x"}) == "{0}} is x");
        CHECK(format("no placeholder", {}) == "no placeholder");
        CHECK(format("{0}", {"{1}"}) == "{1}");  // an argument is not read again
    });

    run("english: the words of a code, nested messages, literals", [] {
        CHECK(english(Message("requires_missing", {"anvil-game", ">=1.2.0"})) ==
              "requires plugin 'anvil-game' (>=1.2.0), which is not loaded");
        CHECK(english(Message("from_mod", {"Mod X", Message("workshop_item", {"123"}), Message("waiting_approval", {"abcd1234"})})) ==
              "from mod 'Mod X' (Workshop 123): waiting for approval (hash abcd1234)");
        CHECK(english(Message("plugin.toml: line 3: bad")) == "plugin.toml: line 3: bad");
        CHECK(english(Message("no_such_code", {"a"})) == "no_such_code a");
        CHECK(english(Message()).empty() && Message().empty());
        for (const Pattern& p : patterns()) CHECK(pattern_of(p.code) == p.english);
        std::set<std::string> codes;
        for (const Pattern& p : patterns()) CHECK(codes.insert(p.code).second);
    });

    run("crash_message: the level and the plugins of a report", [] {
        CHECK(english(crash_message("in_plugin_code", {"tongs"}, "")) == "in plugin tongs's code");
        CHECK(english(crash_message("in_plugin_change", {"tongs", "bellows", "x"}, "")) ==
              "in plugin tongs's change (also bellows, x)");
        CHECK(english(crash_message("plugin_on_stack", {"y"}, "")) == "plugin y on the stack");
        CHECK(english(crash_message("no_plugin", {}, "")) == "no plugin involved");
        CHECK(english(crash_message("later_level", {"z"}, "something new")) == "something new");
        CHECK(crash_message("in_plugin_code", {"tongs"}, "in plugin tongs's code").code == "crash_in_code");
        CHECK(crash_message("in_plugin_code", {"tongs"}, "in X") == Message("in X"));  // other words: as they are
    });

    run("serialize and parse: codes, arguments, nesting, literals", [] {
        const Message m("from_mod", {"Mod X", Message("local_mod", {}), Message("copy_failed", {"error 5"})});
        CHECK(parse(serialize(m)) == m);
        CHECK(parse(serialize(Message("safe mode"))) == Message("safe mode"));
        CHECK(parse("an English reason from an older core") == Message("an English reason from an older core"));
        CHECK(parse("").empty());
        // Control characters in a literal cannot break the frame.
        CHECK(english(parse(serialize(Message("cannot_read", {std::string("a\x1F") + "b\x03"})))) == "cannot read ab");
    });

    run("language: EU4's settings.txt", [] {
        CHECK(language_of_settings("gui_scale=1.0\nlanguage=\"l_french\"\n") == Language::French);
        CHECK(language_of_settings("\xEF\xBB\xBFlanguage=\"l_german\"\r\nx=1\r\n") == Language::German);
        CHECK(language_of_settings("  language = \"l_spanish\"") == Language::Spanish);
        CHECK(language_of_settings("language=\"l_english\"") == Language::English);
        CHECK(!language_of_settings("language=\"l_russian\""));  // EU4 or a mod may have more; Anvil speaks English
        CHECK(!language_of_settings("languages=\"l_french\"\nother=1"));
        CHECK(!language_of_settings(""));
        CHECK(language_named("de") == Language::German && language_named(" fr ") == Language::French && language_named("PL") == Language::Polish);
        CHECK(!language_named("l_polish") && !language_named("pl-PL"));
        CHECK(language_named("zh-Hans") == Language::SimplifiedChinese && language_named("ZH_HANT") == Language::TraditionalChinese);
        CHECK(language_named("ja") == Language::Japanese && language_named("ko") == Language::Korean && !language_named("zh"));
        for (const Language l : kLanguages) CHECK(language_named(language_code(l)) == l);
        CHECK(!is_cjk(Language::English) && !is_cjk(Language::Spanish) && is_cjk(Language::Japanese) &&
              is_cjk(Language::TraditionalChinese));
    });

    run("language: Steam's and Windows' Chinese, Japanese and Korean", [] {
        CHECK(cjk_language_of_steam("schinese") == Language::SimplifiedChinese);
        CHECK(cjk_language_of_steam("tchinese") == Language::TraditionalChinese);
        CHECK(cjk_language_of_steam("japanese") == Language::Japanese);
        CHECK(cjk_language_of_steam("koreana") == Language::Korean);
        CHECK(!cjk_language_of_steam("english") && !cjk_language_of_steam("polish") && !cjk_language_of_steam(""));
        CHECK(cjk_language_of_locale("zh-CN") == Language::SimplifiedChinese);
        CHECK(cjk_language_of_locale("zh-SG") == Language::SimplifiedChinese);
        CHECK(cjk_language_of_locale("zh-Hans-HK") == Language::SimplifiedChinese);
        CHECK(cjk_language_of_locale("zh-TW") == Language::TraditionalChinese);
        CHECK(cjk_language_of_locale("zh-HK") == Language::TraditionalChinese);
        CHECK(cjk_language_of_locale("zh-MO") == Language::TraditionalChinese);
        CHECK(cjk_language_of_locale("zh-Hant-TW") == Language::TraditionalChinese);
        CHECK(cjk_language_of_locale("ja-JP") == Language::Japanese && cjk_language_of_locale("ko-KR") == Language::Korean);
        CHECK(!cjk_language_of_locale("pl-PL") && !cjk_language_of_locale("en-US") && !cjk_language_of_locale("jav") &&
              !cjk_language_of_locale(""));
    });

    run("language: Steam's Polish", [] {
        CHECK(language_of_steam("polish") == Language::Polish && language_of_steam(" polish ") == Language::Polish);
        CHECK(!language_of_steam("english") && !language_of_steam("schinese") && !language_of_steam(""));
        CHECK(!is_cjk(Language::Polish));
    });

    run("language: the options of the overlay's setting", [&] {
        const anvil::SettingsFileResult overlay = anvil::load_settings(root / "plugins" / "anvil-overlay" / "settings.toml");
        CHECK(overlay.file.has_value());
        if (overlay.file) {
            const anvil::Setting* setting = nullptr;
            for (const anvil::Setting& s : overlay.file->settings) if (s.name == "language") setting = &s;
            CHECK(setting != nullptr && setting->options.size() == std::size(kLanguageSettingOptions));
            for (size_t i = 0; setting != nullptr && i < setting->options.size() && i < std::size(kLanguageSettingOptions); ++i) {
                CHECK(setting->options[i].name == kLanguageSettingOptions[i]);
            }
        }
        CHECK(!language_of_setting("game") && !language_of_setting("klingon") && !language_of_setting("zh-Hans"));
        CHECK(language_of_setting("zh_hans") == Language::SimplifiedChinese && language_of_setting("ko") == Language::Korean);
        for (const Language l : kLanguages) {
            bool offered = false;
            for (const std::string_view option : kLanguageSettingOptions) offered = offered || language_of_setting(option) == l;
            CHECK(offered);
        }
    });

    run("choose_language: ANVIL_LANGUAGE, then settings.txt, then English", [] {
        const fs::path dir = fs::temp_directory_path() / ("anvil-i18n-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(dir);
        SetEnvironmentVariableA("ANVIL_LANGUAGE", nullptr);
        // Without the system's languages: Steam on the machine may be Polish.
        CHECK(choose_language(dir, false, {}).language == Language::English && choose_language(dir, false, {}).source == "default");
        std::ofstream(dir / "settings.txt") << "language=\"l_spanish\"\n";
        CHECK(choose_language(dir).language == Language::Spanish && choose_language(dir).source == "settings.txt");
        SetEnvironmentVariableA("ANVIL_LANGUAGE", "de");
        CHECK(choose_language(dir).language == Language::German && choose_language(dir).source == "ANVIL_LANGUAGE");
        SetEnvironmentVariableA("ANVIL_LANGUAGE", "klingon");
        CHECK(choose_language(dir).language == Language::Spanish);
        SetEnvironmentVariableA("ANVIL_LANGUAGE", "ja");
        CHECK(choose_language(dir).language == Language::Japanese && choose_language(dir).source == "ANVIL_LANGUAGE");
        SetEnvironmentVariableA("ANVIL_LANGUAGE", nullptr);

        // Chinese, Japanese and Korean only with Mei: a translation mod
        // replaces EU4's English, so settings.txt says English.
        const SystemLanguages chinese_steam{"schinese", "en-US"};
        const SystemLanguages korean_windows{"english", "ko-KR"};
        CHECK(choose_language(dir, true, chinese_steam).language == Language::Spanish);  // EU4's own language first
        std::ofstream(dir / "settings.txt") << "language=\"l_english\"\n";
        CHECK(choose_language(dir, false, chinese_steam).language == Language::English);
        CHECK(choose_language(dir, false, chinese_steam).source == "settings.txt");
        CHECK(choose_language(dir, true, chinese_steam).language == Language::SimplifiedChinese);
        CHECK(choose_language(dir, true, chinese_steam).source == "Steam");
        CHECK(choose_language(dir, true, korean_windows).language == Language::Korean);
        CHECK(choose_language(dir, true, korean_windows).source == "Windows");
        CHECK(choose_language(dir, true, {"tchinese", "ja-JP"}).language == Language::TraditionalChinese);
        CHECK(choose_language(dir, true, {"english", "pl-PL"}).language == Language::English);
        CHECK(choose_language(dir, true, {"english", "pl-PL"}).source == "settings.txt");
        // Polish from Steam, with or without Mei: EU4 has no Polish, so settings.txt says English.
        CHECK(choose_language(dir, false, {"polish", "en-US"}).language == Language::Polish);
        CHECK(choose_language(dir, false, {"polish", "en-US"}).source == "Steam");
        CHECK(choose_language(dir, true, {"polish", "ja-JP"}).language == Language::Japanese);  // Mei's language first
        std::ofstream(dir / "settings.txt") << "language=\"l_german\"\n";
        CHECK(choose_language(dir, false, {"polish", "pl-PL"}).language == Language::German);  // EU4's own language first
        std::ofstream(dir / "settings.txt") << "language=\"l_english\"\n";
        // The Manager follows Steam and Windows without Mei (lab#93): it
        // installs Anvil before Mei is there.
        CHECK(choose_manager_language(dir, chinese_steam).language == Language::SimplifiedChinese);
        CHECK(choose_manager_language(dir, chinese_steam).source == "Steam");
        CHECK(choose_manager_language(dir, {"english", "zh-TW"}).language == Language::TraditionalChinese);
        CHECK(choose_manager_language(dir, {"english", "zh-TW"}).source == "Windows");
        CHECK(choose_manager_language(dir, {"english", "en-US"}).language == Language::English);
        CHECK(choose_manager_language(dir, {"polish", "pl-PL"}).language == Language::Polish);
        std::ofstream(dir / "settings.txt") << "language=\"l_french\"\n";
        CHECK(choose_manager_language(dir, chinese_steam).language == Language::French);  // EU4's own language first
        SetEnvironmentVariableA("ANVIL_LANGUAGE", "en");
        CHECK(choose_manager_language(dir, chinese_steam).language == Language::English);
        SetEnvironmentVariableA("ANVIL_LANGUAGE", nullptr);
        fs::remove(dir / "settings.txt");
        CHECK(choose_language(dir, true, {"", "ja-JP"}).language == Language::Japanese);
        SetEnvironmentVariableA("ANVIL_LANGUAGE", "de");
        CHECK(choose_language(dir, true, chinese_steam).language == Language::German);
        SetEnvironmentVariableA("ANVIL_LANGUAGE", nullptr);
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    });

    run("tr and text: the player's language, English as the fallback", [] {
        set_language(Language::English);
        CHECK(std::string(tr("Cancel")) == "Cancel");
        CHECK(text(Message("disabled", {})) == "disabled in anvil.toml");
        for (const Language l : kLanguages) {
            if (l == Language::English) continue;
            set_language(l);
            CHECK(std::string(tr("Cancel")) != "Cancel");
            CHECK(std::string(tr("a text no catalog has")) == "a text no catalog has");
            const std::string reason = text(Message("from_mod", {"Mod X", Message("local_mod", {}), Message("safe_mode", {})}));
            CHECK(reason.find("Mod X") != std::string::npos);
            CHECK(reason.find(tr("local mod")) != std::string::npos && reason.find(tr("safe mode")) != std::string::npos);
            CHECK(trf("{0} plugins were not loaded", {"3"}).find('3') != std::string::npos);
            CHECK(text(Message("unknown_code", {"a"})) == "unknown_code a");
            CHECK(text(Message("literal stays English")) == "literal stays English");
        }
        set_language(Language::English);
    });

    const std::set<std::string> keys = catalog_keys(root);
    run("the sources mark their texts", [&] {
        CHECK(keys.size() > 300);
        CHECK(keys.contains("Disable at next start") && keys.contains("Install a plugin from a zip...") &&
              keys.contains("{0} plugins were not loaded"));
    });

    run("CJK fonts: in the resources, readable, without the Reserved Font Name", [] {
        for (const Language l : kLanguages) {
            const FontData font = cjk_font(l);
            CHECK(is_cjk(l) == (font.size > 0));
            if (!is_cjk(l)) continue;
            const TrueType ttf(font);
            CHECK(ttf.ok);
            // OFL: a Modified Version of Source Han Sans may not be called "Source".
            for (const int id : {1, 4, 6}) {
                const std::string name = ttf.name(id);
                if (name.empty() || name.find("Source") != std::string::npos) {
                    std::printf("  %s: font name %d is '%s'\n", name_of(l), id, name.c_str());
                    CHECK(false);
                }
            }
            CHECK(!catalog_characters(l).empty());
            for (const uint32_t c : catalog_characters(l)) CHECK(c > 0x17F && c <= 0xFFFF);
        }
        CHECK(catalog_characters(Language::English).empty());
    });

    for (const Language l : kLanguages) {
        if (l == Language::English) continue;
        run((std::string("catalog ") + name_of(l) + ": complete, same placeholders, characters the fonts have").c_str(), [&] {
            const TrueType cjk(cjk_font(l));
            const Catalog c = catalog(l);
            std::map<std::string, std::string> entries;
            for (size_t i = 0; i < c.size; ++i) {
                const std::string english = c.entries[i].english;
                const std::string translated = c.entries[i].translated;
                if (!entries.emplace(english, translated).second) {
                    std::printf("  %s: twice: %s\n", name_of(l), english.c_str());
                    CHECK(false);
                }
                if (!keys.contains(english)) {
                    std::printf("  %s: not used in the sources: %s\n", name_of(l), english.c_str());
                    CHECK(false);
                }
                if (translated.empty()) {
                    std::printf("  %s: empty: %s\n", name_of(l), english.c_str());
                    CHECK(false);
                }
                if (placeholders(english) != placeholders(translated)) {
                    std::printf("  %s: placeholders differ: %s\n", name_of(l), english.c_str());
                    CHECK(false);
                }
                for (const uint32_t cp : code_points(translated)) {
                    if (!in_fonts(cp) && !cjk.has(cp)) {
                        std::printf("  %s: U+%04X is not in the fonts: %s\n", name_of(l), cp, translated.c_str());
                        CHECK(false);
                        break;
                    }
                }
            }
            for (const std::string& key : keys) {
                if (!entries.contains(key)) {
                    std::printf("  %s: missing: %s\n", name_of(l), key.c_str());
                    CHECK(false);
                }
            }
        });
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
