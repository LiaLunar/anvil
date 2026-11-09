#include "i18n/translate.hpp"

#include <atomic>
#include <unordered_map>
#include <vector>

namespace anvil::i18n {

extern const Translation kFrench[];
extern const size_t kFrenchSize;
extern const Translation kGerman[];
extern const size_t kGermanSize;
extern const Translation kSpanish[];
extern const size_t kSpanishSize;
extern const Translation kSimplifiedChinese[];
extern const size_t kSimplifiedChineseSize;
extern const Translation kTraditionalChinese[];
extern const size_t kTraditionalChineseSize;
extern const Translation kJapanese[];
extern const size_t kJapaneseSize;
extern const Translation kKorean[];
extern const size_t kKoreanSize;
extern const Translation kPolish[];
extern const size_t kPolishSize;

namespace {

std::atomic<Language> g_language{Language::English};

using Index = std::unordered_map<std::string_view, const char*>;

Index build(Language language) {
    Index index;
    const Catalog c = catalog(language);
    index.reserve(c.size);
    for (size_t i = 0; i < c.size; ++i) index.emplace(c.entries[i].english, c.entries[i].translated);
    return index;
}

// Built on the first lookup of a language: only the player's is ever built.
const Index& index_of(Language language) {
    static const Index none;
    switch (language) {
    case Language::French: { static const Index french = build(Language::French); return french; }
    case Language::German: { static const Index german = build(Language::German); return german; }
    case Language::Spanish: { static const Index spanish = build(Language::Spanish); return spanish; }
    case Language::SimplifiedChinese: { static const Index hans = build(Language::SimplifiedChinese); return hans; }
    case Language::TraditionalChinese: { static const Index hant = build(Language::TraditionalChinese); return hant; }
    case Language::Japanese: { static const Index japanese = build(Language::Japanese); return japanese; }
    case Language::Korean: { static const Index korean = build(Language::Korean); return korean; }
    case Language::Polish: { static const Index polish = build(Language::Polish); return polish; }
    case Language::English: break;
    }
    return none;
}

}  // namespace

Catalog catalog(Language language) {
    switch (language) {
    case Language::French: return {kFrench, kFrenchSize};
    case Language::German: return {kGerman, kGermanSize};
    case Language::Spanish: return {kSpanish, kSpanishSize};
    case Language::SimplifiedChinese: return {kSimplifiedChinese, kSimplifiedChineseSize};
    case Language::TraditionalChinese: return {kTraditionalChinese, kTraditionalChineseSize};
    case Language::Japanese: return {kJapanese, kJapaneseSize};
    case Language::Korean: return {kKorean, kKoreanSize};
    case Language::Polish: return {kPolish, kPolishSize};
    case Language::English: break;
    }
    return {nullptr, 0};
}

void set_language(Language language) { g_language.store(language, std::memory_order_relaxed); }

Language language() { return g_language.load(std::memory_order_relaxed); }

const char* tr(Language language, const char* english) {
    if (english == nullptr || language == Language::English) return english;
    const Index& index = index_of(language);
    const auto it = index.find(english);
    return it != index.end() && it->second[0] != '\0' ? it->second : english;
}

const char* tr(const char* english) { return tr(language(), english); }

std::string trf(const char* english, std::initializer_list<std::string_view> args) {
    return i18n::format(tr(english), std::vector<std::string>(args.begin(), args.end()));
}

std::string text(Language language, const Message& message) {
    if (message.code.empty()) return message.text;
    std::vector<std::string> args;
    for (const Message& a : message.args) args.push_back(text(language, a));
    const char* pattern = pattern_of(message.code);
    if (pattern == nullptr) return english(message);
    return i18n::format(tr(language, pattern), args);
}

std::string text(const Message& message) { return text(language(), message); }

}  // namespace anvil::i18n
