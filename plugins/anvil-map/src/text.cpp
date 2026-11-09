#include "text.hpp"

#include <array>
#include <cstdint>

namespace anvil_map {

namespace {

// Decodes one character at text[i]; 0 bytes for an invalid one.
std::size_t decode(std::string_view text, std::size_t i, char32_t& out) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(text[i + k]); };
    const unsigned char c = b(0);
    const std::size_t left = text.size() - i;
    if (c < 0x80) {
        out = c;
        return 1;
    }
    const auto cont = [&](std::size_t k) { return k < left && (b(k) & 0xC0) == 0x80; };
    if (c >= 0xC2 && c <= 0xDF && cont(1)) {
        out = (static_cast<char32_t>(c & 0x1F) << 6) | (b(1) & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF && cont(1) && cont(2)) {
        const char32_t v =
            (static_cast<char32_t>(c & 0x0F) << 12) | (static_cast<char32_t>(b(1) & 0x3F) << 6) | (b(2) & 0x3F);
        if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        out = v;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4 && cont(1) && cont(2) && cont(3)) {
        const char32_t v = (static_cast<char32_t>(c & 0x07) << 18) | (static_cast<char32_t>(b(1) & 0x3F) << 12) |
                           (static_cast<char32_t>(b(2) & 0x3F) << 6) | (b(3) & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return 0;
        out = v;
        return 4;
    }
    return 0;
}

// Windows-1252 0x80..0x9F; 0 where the code page has no character.
constexpr std::array<char16_t, 32> kHigh = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

}  // namespace

std::string_view utf8_prefix(std::string_view text, std::size_t max) {
    if (max > text.size()) max = text.size();
    std::size_t i = 0;
    while (i < text.size()) {
        char32_t c = 0;
        const std::size_t n = decode(text, i, c);
        if (n == 0 || i + n > max) break;
        i += n;
    }
    return text.substr(0, i);
}

bool valid_utf8(std::string_view text) { return utf8_prefix(text).size() == text.size(); }

unsigned char windows_1252(char32_t c) {
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) return static_cast<unsigned char>(c);
    for (std::size_t k = 0; k < kHigh.size(); ++k) {
        if (kHigh[k] != 0 && kHigh[k] == c) return static_cast<unsigned char>(0x80 + k);
    }
    return 0;
}

std::string game_text(std::string_view utf8, bool utf8_game) {
    const std::string_view valid = utf8_prefix(utf8);
    std::string out;
    out.reserve(valid.size());
    std::size_t i = 0;
    while (i < valid.size()) {
        char32_t c = 0;
        i += decode(valid, i, c);
        if (c < 0x20 && c != '\n') continue;
        if (c == 0x7F) continue;
        // The game starts a colour with U+00A7 and the next character, and it
        // has no escape for the sign. A plugin's text is literal, so the sign
        // becomes '?', like any character the game cannot show.
        if (c == 0xA7) c = '?';
        if (utf8_game) {
            append_utf8(out, c);
        } else {
            const unsigned char b = windows_1252(c);
            out += b != 0 ? static_cast<char>(b) : '?';
        }
    }
    return out;
}

}  // namespace anvil_map
