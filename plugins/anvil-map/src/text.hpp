// anvil-map: text from plugins (UTF-8) as the game draws it.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace anvil_map {

// The longest prefix of `text` that is valid UTF-8 (no overlong forms, no
// surrogates, nothing above U+10FFFF) and at most `max` bytes, ending at a
// whole character.
std::string_view utf8_prefix(std::string_view text, std::size_t max = std::string_view::npos);

// Whether all of `text` is valid UTF-8.
bool valid_utf8(std::string_view text);

// `utf8` as the game should get it: the valid prefix unchanged when the game's
// text is UTF-8 (Mei's utf8_localization), else in Windows-1252 with '?' for
// a character it lacks. The game's colour code character (U+00A7) becomes '?'
// too, so the text stays literal; control characters other than '\n' are
// dropped.
std::string game_text(std::string_view utf8, bool utf8_game);

// The Windows-1252 byte of a code point, or 0 when it has none.
unsigned char windows_1252(char32_t c);

}  // namespace anvil_map
