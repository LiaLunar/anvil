// The fonts of Chinese, Japanese and Korean (issue #158, design §15): for each,
// a subset of a Source Han Sans Regular face with the characters its catalog
// uses (src/i18n/fonts/README.md). The overlay and the Manager merge it into
// their Latin font; only the player's language's is loaded.
#pragma once

#include "i18n/message.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace anvil::i18n {

struct FontData {
    const void* data = nullptr;  // in the binary's resources: valid until the process ends
    size_t size = 0;
};

// The font of a CJK language from the resources of the binary this code is
// linked into (src/i18n/fonts/fonts.rc); empty for the other languages, or
// when the binary does not carry it.
FontData cjk_font(Language language);

// The characters beyond Latin Extended-A that a language's catalog uses,
// sorted: the glyphs the atlas needs from the CJK font.
std::vector<uint32_t> catalog_characters(Language language);

// The options of the overlay's `language` setting (plugins/anvil-overlay/
// settings.toml), in their order: "game" follows choose_language().
inline constexpr std::string_view kLanguageSettingOptions[] = {"game", "en", "fr", "de", "es",
                                                               "zh_hans", "zh_hant", "ja", "ko"};
// The language an option names; nullopt for "game" and unknown names.
std::optional<Language> language_of_setting(std::string_view option);

}  // namespace anvil::i18n
