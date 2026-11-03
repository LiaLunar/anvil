#include "i18n/cjk_font.hpp"

#include "i18n/translate.hpp"

#include <windows.h>

#include <algorithm>
#include <set>

namespace anvil::i18n {

FontData cjk_font(Language language) {
    const wchar_t* name = language == Language::SimplifiedChinese    ? L"ANVIL_FONT_ZH_HANS"
                          : language == Language::TraditionalChinese ? L"ANVIL_FONT_ZH_HANT"
                          : language == Language::Japanese           ? L"ANVIL_FONT_JA"
                          : language == Language::Korean             ? L"ANVIL_FONT_KO"
                                                                     : nullptr;
    if (name == nullptr) return {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&cjk_font), &module)) {
        return {};
    }
    HRSRC resource = FindResourceW(module, name, MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (resource == nullptr) return {};
    HGLOBAL loaded = LoadResource(module, resource);
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(module, resource);
    if (data == nullptr || size == 0) return {};
    return {data, size};
}

std::vector<uint32_t> catalog_characters(Language language) {
    std::set<uint32_t> out;
    const Catalog c = catalog(language);
    for (size_t i = 0; i < c.size; ++i) {
        const std::string_view text = c.entries[i].translated;
        for (size_t at = 0; at < text.size();) {
            const auto b = static_cast<unsigned char>(text[at]);
            const size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
            if (at + n > text.size()) break;
            uint32_t cp = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
            for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[at + k]) & 0x3F);
            if (cp > 0x17F) out.insert(cp);
            at += n;
        }
    }
    return {out.begin(), out.end()};
}

std::optional<Language> language_of_setting(std::string_view option) {
    if (option == "game") return std::nullopt;
    const auto* known = std::find(std::begin(kLanguageSettingOptions), std::end(kLanguageSettingOptions), option);
    if (known == std::end(kLanguageSettingOptions)) return std::nullopt;
    return language_named(option);
}

}  // namespace anvil::i18n
