#pragma once

#include "manifest.hpp"
#include "settings.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// A plugin translation (design §5.4, ADR 0010): translations/<code>.toml in
// the plugin folder holds one language's words for the texts the player sees
// of the plugin in the overlay and the Manager: its description and its
// settings' labels, descriptions, groups and option labels. They are looked
// up by their English words, as the catalogs of Anvil's own texts are, so a
// changed English text is shown in English until its translation follows.
// The loader never skips a plugin for its translations.

inline constexpr const char* kTranslationsFolder = "translations";

// The codes a translation file may have, as ANVIL_LANGUAGE writes them:
// fr, de, es, zh-Hans, zh-Hant, ja, ko, pl. English has none: it is the source.
const std::vector<std::string>& translation_codes();

enum class TranslationStatus { Draft, Checked };

struct PluginTranslation {
    TranslationStatus status = TranslationStatus::Draft;
    // English words to this language's words.
    std::map<std::string, std::string, std::less<>> text;
    // The line of each entry of [text], for anvil check.
    std::map<std::string, uint32_t, std::less<>> lines;

    // The translation of `english`, or `english` itself without one.
    std::string_view translate(std::string_view english) const;
};

struct PluginTranslationResult {
    std::optional<PluginTranslation> translation;
    std::string error;  // set when translation is empty and the file exists
    uint32_t line = 0;  // the line the error is about; 0 = the whole file
};

// Parses one translation file:
//
//     status = "draft"            # or "checked": a native speaker checked it
//     [text]
//     "Panel key" = "Touche du panneau"
//
// Unknown keys, a missing status, values that are not strings, an empty
// translation and a NUL are errors.
PluginTranslationResult parse_plugin_translation(std::string_view text);

// <folder>/translations/<code>.toml. A missing file gives neither a
// translation nor an error; so does English ("en") and an unknown code.
PluginTranslationResult load_plugin_translation(const std::filesystem::path& folder, std::string_view code);

// The English texts of the plugin a translation may translate, each once, in
// the order the player meets them: the manifest's description, then per
// setting its label, description, group and option labels.
std::vector<std::string> plugin_texts(const Manifest& manifest, const SettingsFile* settings);

// What anvil check says about translations/ (design §16). `file` is the path
// below `folder` ("translations/fr.toml"); `line` is 1-based or 0.
struct TranslationProblem {
    std::string file;
    uint32_t line = 0;
    std::string message;
    bool error = true;  // false: a warning (a missing translation)
};

// Every file in translations/ must be <code>.toml for a known code and parse,
// and each of its keys must be a text of the plugin (`texts`): errors. A text
// without a translation, or a language without a file, is a warning, or an
// error when `complete` (anvil check --complete-translations); a plugin
// without a translations/ folder gets one such problem for the folder. A
// plugin with no texts needs no translations.
std::vector<TranslationProblem> check_plugin_translations(const std::filesystem::path& folder,
                                                          const std::vector<std::string>& texts, bool complete);

}  // namespace anvil
