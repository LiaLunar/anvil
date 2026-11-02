// Messages for the player by code (issue #158, design §15): the core says why
// a plugin is skipped or failed as a code with arguments, writes the English
// words to anvil.log and `anvil check`, and the overlay and the Manager show
// the same message in the player's language (i18n/translate.hpp). Nothing
// here translates; the catalogs are not linked into version.dll.
#pragma once

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::i18n {

// A code with arguments, or a literal text shown as it is (an error of the
// system, a plugin.toml diagnostic): those stay English in every language.
struct Message {
    std::string code;  // "" for a literal
    std::string text;  // the literal
    std::vector<Message> args;

    Message() = default;
    Message(std::string literal) : text(std::move(literal)) {}  // NOLINT: a literal converts implicitly
    Message(const char* literal) : text(literal) {}              // NOLINT
    Message(std::string code_, std::initializer_list<Message> args_) : code(std::move(code_)), args(args_) {}

    bool empty() const { return code.empty() && text.empty(); }
    bool operator==(const Message&) const = default;
};

// The English pattern of a code ("requires plugin '{0}' ({1}), which is not
// loaded"); nullptr for a code this Anvil does not know.
const char* pattern_of(std::string_view code);

// Every code with its English pattern, for the tests and the catalogs.
struct Pattern {
    const char* code;
    const char* english;
};
const std::vector<Pattern>& patterns();

// {0}..{9} in `pattern` replaced by args; "{{" is "{". A placeholder without
// an argument stays as it is.
std::string format(std::string_view pattern, const std::vector<std::string>& args);

// The English words: what anvil.log, `anvil check` and the reports say. An
// unknown code gives the code and its arguments.
std::string english(const Message& message);

// For anvil.control: one string that carries the code and arguments
// (ASCII 0x02 code, 0x1F before each argument, 0x03; a literal as it is).
std::string serialize(const Message& message);
Message parse(std::string_view text);

// How a crash report names its plugins (crash_history.hpp's level and
// plugins): "in plugin x's code (also y)". `summary`, the report's English
// words, as a literal when the level is unknown or the words differ.
Message crash_message(std::string_view level, const std::vector<std::string>& plugins, std::string_view summary);

// ---- the player's language -------------------------------------------------

enum class Language { English, French, German, Spanish };
inline constexpr Language kLanguages[] = {Language::English, Language::French, Language::German, Language::Spanish};

const char* language_code(Language language);  // "en", "fr", "de", "es"
// "fr", "de", "es", "en" or EU4's names ("l_french"); nullopt for others.
std::optional<Language> language_named(std::string_view name);

// The `language` of EU4's settings.txt ("l_german"); nullopt when the text has
// none or names a language Anvil does not speak (Anvil then speaks English).
std::optional<Language> language_of_settings(std::string_view settings_txt);

struct LanguageChoice {
    Language language = Language::English;
    std::string source;  // "ANVIL_LANGUAGE", "settings.txt" or "default"
};
// The player's language: ANVIL_LANGUAGE (en, fr, de, es) when set, otherwise
// settings.txt in EU4's user folder (only read), otherwise English.
LanguageChoice choose_language(const std::filesystem::path& user_dir);

}  // namespace anvil::i18n
