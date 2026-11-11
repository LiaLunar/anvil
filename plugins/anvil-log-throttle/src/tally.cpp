#include "tally.hpp"

#include <algorithm>

namespace log_throttle {

namespace {

std::uint64_t key_of(std::string_view log, std::string_view source, int line, std::string_view text) {
    std::uint64_t h = 14695981039346656037ull;
    const auto add = [&h](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const std::string_view part : {log, source}) {
        const std::size_t size = part.size();
        add(&size, sizeof(size));
        add(part.data(), part.size());
    }
    add(&line, sizeof(line));
    add(text.data(), text.size());
    return h;
}

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c >> 5) == 0x6) {
            n = 2;
            cp = c & 0x1F;
        } else if ((c >> 4) == 0xE) {
            n = 3;
            cp = c & 0x0F;
        } else if ((c >> 3) == 0x1E) {
            n = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b >> 6) != 0x2) return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        // Overlong forms, surrogates and values past Unicode are not UTF-8.
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n;
    }
    return true;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Windows-1252 0x80..0x9F; the five bytes it leaves undefined keep their C1 value.
constexpr std::uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

}  // namespace

std::string_view file_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view without_line_end(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

std::string utf8_text(std::string_view bytes) {
    if (valid_utf8(bytes)) return std::string(bytes);
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 4);
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out += ch;
        } else if (c < 0xA0) {
            append_utf8(out, kCp1252High[c - 0x80]);
        } else {
            append_utf8(out, c);
        }
    }
    return out;
}

void Tally::keep(std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (totals_.keeping) return;
    totals_.keeping = true;
    totals_.since_ms = now_ms;
    keeping_.store(true, std::memory_order_release);
}

void Tally::add(std::string_view log, std::string_view source, int line, std::string_view text, bool counted) {
    if (!keeping()) return;
    source = file_name(source);
    text = without_line_end(text);
    const std::uint64_t key = key_of(log, source, line, text);
    std::lock_guard lock(mutex_);
    ++totals_.version;
    if (counted) {
        ++totals_.counted;
    } else {
        ++totals_.written;
    }
    auto it = index_.find(key);
    if (it == index_.end()) {
        if (messages_.size() >= max_messages_) {
            ++totals_.untracked;
            return;
        }
        TallyMessage& m = messages_.emplace_back();
        m.log = std::string(log);
        m.source = std::string(source);
        m.source_line = line;
        m.text = utf8_text(text);
        Counts& fresh = counts_.emplace_back();
        // The game's bytes, when they differ from the UTF-8 text, for the
        // comparison of a later line without a conversion.
        if (m.text != text) fresh.raw = std::string(text);
        it = index_.emplace(key, static_cast<std::uint32_t>(messages_.size() - 1)).first;
        ++totals_.messages;
    } else {
        const TallyMessage& m = messages_[it->second];
        const std::string& bytes = counts_[it->second].raw.empty() ? m.text : counts_[it->second].raw;
        // A hash collision: the line is not counted under another message.
        if (m.source_line != line || m.log != log || m.source != source || bytes != text) {
            ++totals_.untracked;
            return;
        }
    }
    Counts& c = counts_[it->second];
    if (counted) {
        ++c.counted;
    } else {
        ++c.written;
    }
    c.version = totals_.version;
}

TallyTotals Tally::totals() const {
    std::lock_guard lock(mutex_);
    return totals_;
}

std::uint64_t Tally::read(std::uint64_t first, std::uint64_t changed_after, std::size_t capacity,
                          std::vector<TallyRow>& out) const {
    std::lock_guard lock(mutex_);
    std::uint64_t i = first;
    std::size_t taken = 0;
    for (; i < counts_.size() && taken < capacity; ++i) {
        const Counts& c = counts_[i];
        if (c.version <= changed_after) continue;
        out.push_back({i, c.version, c.written, c.counted, &messages_[i]});
        ++taken;
    }
    return i;
}

}  // namespace log_throttle
