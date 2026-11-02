// The player's language in the overlay and the Manager (issue #158, design
// §15). Texts are looked up by their English words in the catalogs
// (i18n/catalog_*.cpp); a text without a translation stays English. anvil.log
// and `anvil check` never go through here.
#pragma once

#include "i18n/message.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

namespace anvil::i18n {

// The language tr() speaks; English until it is set. Set it before the
// first frame, and again only when the player's language changes.
void set_language(Language language);
Language language();

// The text in language(), or in `language`; `english` itself when the
// catalog has no translation. The result lives as long as the process.
const char* tr(const char* english);
const char* tr(Language language, const char* english);

// tr(), then {0}, {1}... replaced by `args` (i18n::format).
std::string trf(const char* english, std::initializer_list<std::string_view> args);

// Marks an English text for the catalogs where it is translated later (an
// array of texts, a ternary): the tests find every tr("..."), trf("...") and
// N_("...") in the sources and check that each catalog translates it.
constexpr const char* N_(const char* english) { return english; }

// A message in language(), or in `language`. An unknown code gives its English form.
std::string text(const Message& message);
std::string text(Language language, const Message& message);

// One language's translations: English words to the language's words.
struct Translation {
    const char* english;
    const char* translated;
};
struct Catalog {
    const Translation* entries;
    size_t size;
};
Catalog catalog(Language language);  // empty for English

}  // namespace anvil::i18n
