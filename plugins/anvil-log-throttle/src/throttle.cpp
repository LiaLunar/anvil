#include "throttle.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace log_throttle {

namespace {

// FNV-1a, 64 bits: the key of a message in the table. Equal keys are checked
// against the stored text before a line is counted.
struct Hash {
    std::uint64_t value = 14695981039346656037ull;

    void add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value ^= bytes[i];
            value *= 1099511628211ull;
        }
    }
    template <class T> void add_value(const T& v) { add(&v, sizeof(v)); }
};

std::uint64_t key_of(const void* sink, int channel, std::string_view file, int line, std::string_view text) {
    Hash h;
    h.add_value(sink);
    h.add_value(channel);
    h.add_value(line);
    h.add_value(file.size());
    h.add(file.data(), file.size());
    h.add(text.data(), text.size());
    return h.value;
}

bool same_message(const Line& l, const void* sink, int channel, std::string_view file, int line,
                  std::string_view text) {
    return l.sink == sink && l.channel == channel && l.line == line && l.file == file && l.text == text;
}

std::uint64_t whole_seconds(std::uint64_t ms) { return std::max<std::uint64_t>(1, (ms + 999) / 1000); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

std::string notice_text(std::uint64_t repeats, std::uint64_t report_ms, std::string_view text) {
    return std::format("[anvil-log-throttle] logged {} times; further repeats are counted and reported every {} s: {}",
                       repeats, whole_seconds(report_ms), text);
}

std::string report_text(std::uint64_t count, std::uint64_t elapsed_ms, std::string_view text) {
    return std::format("[anvil-log-throttle] repeated {} more times in the last {} s: {}", count,
                       whole_seconds(elapsed_ms), text);
}

Verdict Throttle::record(const void* sink, int channel, std::string_view file, int line, std::string_view text,
                         std::uint64_t now_ms, const Config& config, std::vector<Line>& out) {
    std::lock_guard lock(mutex_);
    SinkStats& stats = sinks_[sink];
    ++stats.lines;
    // At most once a second: the counts that are due, of any sink.
    if (now_ms >= next_scan_ms_) {
        report_due(now_ms, config.report_ms, out);
        next_scan_ms_ = now_ms + 1000;
    }
    if (config.full_log) {
        ++stats.written;
        return Verdict::write;
    }
    const std::uint64_t key = key_of(sink, channel, file, line, text);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        if (entries_.size() >= max_messages_) forget_uncounted();
        if (entries_.size() >= max_messages_) {
            ++stats.written;
            return Verdict::write;
        }
        it = entries_.emplace(key, Entry{sink, 0, nullptr}).first;
    }
    Entry& e = it->second;
    if (e.counted) {
        if (!same_message(e.counted->line, sink, channel, file, line, text)) {
            ++stats.written;
            return Verdict::write;
        }
        ++e.counted->pending;
        ++stats.counted;
        return Verdict::count;
    }
    if (++e.seen <= config.repeats) {
        ++stats.written;
        return Verdict::write;
    }
    // This line is the first that is counted; a notice says so instead.
    auto counted = std::make_unique<Counted>();
    counted->line = Line{sink, channel, std::string(file), line, std::string(text), false};
    counted->pending = 1;
    counted->since_ms = now_ms;
    Line notice = counted->line;
    notice.text = notice_text(config.repeats, config.report_ms, text);
    notice.notice = true;
    notice.message_at = notice.text.size() - text.size();
    counted_.push_back(counted.get());
    e.counted = std::move(counted);
    ++stats.counted;
    ++stats.messages;
    out.push_back(std::move(notice));
    return Verdict::count;
}

void Throttle::report_due(std::uint64_t now_ms, std::uint64_t report_ms, std::vector<Line>& out) {
    for (Counted* c : counted_) {
        if (c->pending == 0 || now_ms - c->since_ms < report_ms) continue;
        Line report = c->line;
        report.text = report_text(c->pending, now_ms - c->since_ms, c->line.text);
        report.message_at = report.text.size() - c->line.text.size();
        out.push_back(std::move(report));
        c->pending = 0;
        c->since_ms = now_ms;
    }
}

void Throttle::forget_uncounted() {
    std::erase_if(entries_, [](const auto& item) { return !item.second.counted; });
}

SinkStats Throttle::close(const void* sink, std::uint64_t now_ms, std::vector<Line>& out) {
    std::lock_guard lock(mutex_);
    for (Counted* c : counted_) {
        if (c->line.sink != sink || c->pending == 0) continue;
        Line report = c->line;
        report.text = report_text(c->pending, now_ms - c->since_ms, c->line.text);
        report.message_at = report.text.size() - c->line.text.size();
        out.push_back(std::move(report));
        c->pending = 0;
        c->since_ms = now_ms;
    }
    std::erase_if(counted_, [sink](const Counted* c) { return c->line.sink == sink; });
    std::erase_if(entries_, [sink](const auto& item) { return item.second.sink == sink; });
    SinkStats stats;
    if (const auto it = sinks_.find(sink); it != sinks_.end()) {
        stats = it->second;
        sinks_.erase(it);
    }
    return stats;
}

SinkStats Throttle::stats(const void* sink) const {
    std::lock_guard lock(mutex_);
    const auto it = sinks_.find(sink);
    return it != sinks_.end() ? it->second : SinkStats{};
}

std::size_t Throttle::tracked() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

LogNames LogNames::parse(std::string_view setting) {
    LogNames out;
    while (!setting.empty()) {
        const std::size_t comma = setting.find(',');
        std::string_view item = setting.substr(0, comma);
        setting = comma == std::string_view::npos ? std::string_view() : setting.substr(comma + 1);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (item.empty()) continue;
        if (item == "*") {
            out.all = true;
            continue;
        }
        std::string name(item);
        std::transform(name.begin(), name.end(), name.begin(), lower);
        out.names.push_back(std::move(name));
    }
    return out;
}

bool LogNames::matches(std::string_view path) const {
    if (all) return true;
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return std::any_of(names.begin(), names.end(), [name](const std::string& n) {
        return n.size() == name.size() &&
               std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) { return a == lower(b); });
    });
}

}  // namespace log_throttle
