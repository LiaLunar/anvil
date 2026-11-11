// Plugin translations (design §5.4, ADR 0010): translations/<code>.toml, the
// texts a plugin has, the checks of anvil check and anvil.control's lookups.
#include "core/control.hpp"
#include "core/plugin_translation.hpp"
#include "core/settings.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

fs::path scratch(const char* name) {
    const auto dir = fs::temp_directory_path() / ("anvil_translation_tests_" + std::to_string(GetCurrentProcessId())) / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "translations");
    return dir;
}

void write(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

constexpr const char* kSettings = R"toml([setting.mode]
type = "enum"
values = [{ name = "off", label = "Off" }, { name = "on", label = "On" }]
default = "off"
label = "Mode"
description = "What the plugin does."
group = "General"

[setting.other]
type = "bool"
default = true
label = "Other"
group = "General"
)toml";

SettingsFile settings() { return *parse_settings_text(kSettings).file; }

size_t count(const std::vector<TranslationProblem>& problems, bool error) {
    size_t n = 0;
    for (const auto& p : problems) n += p.error == error ? 1 : 0;
    return n;
}

bool mentions(const std::vector<TranslationProblem>& problems, const std::string& file, const std::string& words) {
    for (const auto& p : problems) {
        if (p.file == file && p.message.find(words) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

void translation_tests() {
    run("translations: a file parses to its status and texts", [] {
        const auto r = parse_plugin_translation(
            "\xEF\xBB\xBF# a comment\nstatus = \"checked\"\n[text]\n\"Mode\" = \"Modus\"\n\"Off\" = \"Aus\"\n");
        CHECK(r.translation.has_value());
        if (!r.translation) return;
        CHECK(r.translation->status == TranslationStatus::Checked && r.translation->text.size() == 2);
        CHECK(r.translation->translate("Mode") == "Modus" && r.translation->translate("On") == "On");
        CHECK(r.translation->lines.at("Off") == 5);
        const auto draft = parse_plugin_translation("status = \"draft\"\n");
        CHECK(draft.translation && draft.translation->status == TranslationStatus::Draft && draft.translation->text.empty());
    });

    run("translations: what does not parse", [] {
        CHECK(parse_plugin_translation("[text]\n\"Mode\" = \"Modus\"\n").error.find("missing status") != std::string::npos);
        CHECK(parse_plugin_translation("status = \"done\"\n").line == 1);
        const auto unknown = parse_plugin_translation("status = \"draft\"\nlanguage = \"de\"\n");
        CHECK(unknown.error.find("unknown key 'language'") != std::string::npos && unknown.line == 2);
        CHECK(!parse_plugin_translation("status = \"draft\"\n[text]\n\"Mode\" = 1\n").translation);
        CHECK(parse_plugin_translation("status = \"draft\"\n[text]\n\"Mode\" = \"\"\n").error.find("empty") != std::string::npos);
        CHECK(!parse_plugin_translation("status = \"draft\"\n[text]\n\"Mode\" = \"a\\u0000b\"\n").translation);
        CHECK(!parse_plugin_translation("status = \"draft\"\ntext = \"x\"\n").translation);
        const auto broken = parse_plugin_translation("status = \"draft\"\n[text\n");
        CHECK(!broken.translation && broken.line == 2);
    });

    run("translations: the texts of a plugin, each once", [] {
        Manifest m;
        m.description = "A plugin.";
        const SettingsFile s = settings();
        const auto texts = plugin_texts(m, &s);
        const std::vector<std::string> expected = {"A plugin.", "Mode", "What the plugin does.", "General", "Off", "On", "Other"};
        CHECK(texts == expected);
        m.description.clear();
        CHECK(plugin_texts(m, nullptr).empty());
    });

    run("translations: loading by language code", [] {
        const fs::path dir = scratch("load");
        write(dir / "translations" / "de.toml", "status = \"draft\"\n[text]\n\"Mode\" = \"Modus\"\n");
        write(dir / "translations" / "fr.toml", "status = \"draft\"\n[text\n");
        const auto de = load_plugin_translation(dir, "de");
        CHECK(de.translation && de.translation->translate("Mode") == "Modus");
        const auto fr = load_plugin_translation(dir, "fr");
        CHECK(!fr.translation && !fr.error.empty());
        const auto es = load_plugin_translation(dir, "es");  // no file
        CHECK(!es.translation && es.error.empty());
        CHECK(!load_plugin_translation(dir, "en").translation && load_plugin_translation(dir, "en").error.empty());
        CHECK(!load_plugin_translation(dir, "../x").translation);
    });

    run("translations: anvil check's errors and warnings", [] {
        Manifest m;
        m.description = "A plugin.";
        const SettingsFile s = settings();
        const auto texts = plugin_texts(m, &s);

        const fs::path none = scratch("none");
        fs::remove_all(none / "translations");
        auto problems = check_plugin_translations(none, texts, false);
        CHECK(problems.size() == 1 && count(problems, false) == 1);
        CHECK(check_plugin_translations(none, texts, true).size() == 1 && count(check_plugin_translations(none, texts, true), true) == 1);
        CHECK(check_plugin_translations(none, {}, true).empty());

        const fs::path dir = scratch("check");
        std::string all = "status = \"draft\"\n[text]\n";
        for (const auto& t : texts) all += "\"" + t + "\" = \"x " + t + "\"\n";
        for (const auto& code : translation_codes()) write(dir / "translations" / (code + ".toml"), all);
        CHECK(check_plugin_translations(dir, texts, true).empty());

        write(dir / "translations" / "de.toml", all + "\"Old label\" = \"Alt\"\n");                 // stale
        write(dir / "translations" / "fr.toml", "status = \"draft\"\n[text]\n\"Mode\" = \"Mode\"\n");  // incomplete
        write(dir / "translations" / "pl.toml", "status = \"maybe\"\n");                            // broken
        write(dir / "translations" / "pt-BR.toml", all);                                            // unknown language
        write(dir / "translations" / "notes.txt", "");
        fs::remove(dir / "translations" / "ko.toml");                                                // missing
        problems = check_plugin_translations(dir, texts, false);
        CHECK(mentions(problems, "translations/de.toml", "\"Old label\" is not a text of this plugin"));
        CHECK(mentions(problems, "translations/fr.toml", "no translation of \"Off\""));
        CHECK(mentions(problems, "translations/pl.toml", "status must be"));
        CHECK(mentions(problems, "translations/pt-BR.toml", "not a language Anvil shows"));
        CHECK(mentions(problems, "translations/notes.txt", "not a language Anvil shows"));
        CHECK(mentions(problems, "translations/ko.toml", "missing"));
        // Errors: stale, broken, two unknown files; warnings: six texts of fr, ko.
        CHECK(count(problems, true) == 4 && count(problems, false) == 7);
        problems = check_plugin_translations(dir, texts, true);
        CHECK(count(problems, true) == 11 && count(problems, false) == 0);
    });

    run("translations: anvil.control's plugin_text and mod_plugin_text", [] {
        ControlSources sources;
        PluginTranslation t;
        t.text = {{"Mode", "Modus"}, {"A plugin.", "Ein Plugin."}};
        sources.translations.emplace("hello", t);
        sources.mod_translations.emplace(0u, t);
        sources.language = "de";
        Control control(sources);
        const AnvilControlService& svc = *control.service();
        CHECK(svc.size >= offsetof(AnvilControlService, mod_plugin_text) + sizeof(void*));
        const char* english = "Mode";
        CHECK(std::string(svc.plugin_text("hello", english)) == "Modus");
        CHECK(svc.plugin_text("other", english) == english);
        CHECK(svc.plugin_text("hello", "Other") == std::string("Other"));
        CHECK(svc.plugin_text("hello", nullptr) == nullptr && svc.plugin_text(nullptr, english) == english);
        CHECK(std::string(svc.mod_plugin_text(0, "A plugin.")) == "Ein Plugin.");
        CHECK(svc.mod_plugin_text(1, english) == english);
        // The result lives as long as the service: the same pointer every time.
        CHECK(svc.plugin_text("hello", english) == svc.plugin_text("hello", "Mode"));
    });
}
