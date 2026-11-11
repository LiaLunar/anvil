#include "errors_model.hpp"

#include <algorithm>
#include <cctype>

namespace script_errors {

namespace {

constexpr std::size_t kMaxRows = 1000;    // anvil-overlay.panel takes 1024
constexpr std::size_t kItemsShown = 30;   // of an open group
constexpr std::size_t kFilesShown = 40;   // of a group of missing files
constexpr std::size_t kMaxText = 1500;    // of a row's text; the panel takes 4096 bytes
constexpr std::uint64_t kGroupBit = 1ull << 55;

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Where `key` starts in `s`, without case; npos when it is not there.
std::size_t find_nocase(std::string_view s, std::string_view key) {
    if (key.size() > s.size()) return std::string_view::npos;
    for (std::size_t i = 0; i + key.size() <= s.size(); ++i) {
        std::size_t k = 0;
        while (k < key.size() && lower(s[i + k]) == key[k]) ++k;
        if (k == key.size()) return i;
    }
    return std::string_view::npos;
}

std::string quoted_after(std::string_view text, std::string_view key) {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) return {};
    const std::size_t from = at + key.size();
    const std::size_t end = text.find('"', from);
    return end == std::string_view::npos ? std::string{} : std::string(text.substr(from, end - from));
}

// "file: interface/x.gui line: 12", 'File: "interface/x.gui", ... near line: 12'.
void file_and_line(Message& m) {
    const std::string_view t = m.text;
    const std::size_t at = find_nocase(t, "file:");
    if (at == std::string_view::npos) return;
    std::size_t i = at + 5;
    while (i < t.size() && t[i] == ' ') ++i;
    std::size_t end;
    if (i < t.size() && t[i] == '"') {
        ++i;
        end = t.find('"', i);
        if (end == std::string_view::npos) return;
    } else {
        end = i;
        while (end < t.size() && t[end] != ' ' && t[end] != ',' && t[end] != '\t') ++end;
    }
    if (end == i) return;
    m.file = std::string(t.substr(i, end - i));
    // "line:" after the file, or before it ("near line: 12 file: x.gui").
    std::size_t l = find_nocase(t.substr(end), "line:");
    l = l != std::string_view::npos ? end + l : find_nocase(t, "line:");
    if (l == std::string_view::npos) return;
    std::size_t d = l + 5;
    while (d < t.size() && t[d] == ' ') ++d;
    int line = 0;
    while (d < t.size() && t[d] >= '0' && t[d] <= '9' && line < 100'000'000) line = line * 10 + (t[d++] - '0');
    m.line = line;
}

std::uint64_t fnv(std::string_view s) {
    std::uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

std::string file_line(const Message& m) { return m.line > 0 ? m.file + ":" + std::to_string(m.line) : m.file; }

}  // namespace

std::string grouped(std::uint64_t n) {
    const std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ' ';
        out += digits[i];
    }
    return out;
}

std::string cut(std::string_view text, std::size_t max) {
    if (text.size() <= max) return std::string(text);
    std::size_t end = max > 3 ? max - 3 : 0;
    // Not inside a UTF-8 sequence.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end)) + "...";
}

void classify(Message& m) {
    const std::string_view t = m.text;
    const std::string_view src = m.source;
    file_and_line(m);
    if (starts_with(t, "Could not open file: ") || starts_with(t, "Invalid file when updating checksum: ")) {
        // The start-up noise: one line per file, with the file's path.
        m.kind = Kind::files;
        const std::size_t colon = t.find(": ");
        m.group = std::string(t.substr(0, colon));
        std::string_view path = t.substr(colon + 2);
        const std::size_t comma = path.find(", error:");
        if (comma != std::string_view::npos) path = path.substr(0, comma);
        m.file = std::string(path);
        m.line = 0;
    } else if (m.line > 0) {
        m.kind = Kind::located;
    } else if (starts_with(t, "Script error!")) {
        m.kind = Kind::script;
        m.token = quoted_after(t, "Script Object Token: \"");
    } else if (starts_with(t, "Undefined event target")) {
        m.kind = Kind::target;
        const std::size_t c = t.rfind(": ");
        if (c != std::string_view::npos) m.token = std::string(t.substr(c + 2));
        while (!m.token.empty() && m.token.back() == ' ') m.token.pop_back();
        m.tooltip = contains(t, "tooltip");
        m.searchable = !m.token.empty();
    } else if (starts_with(t, "Unknown country '")) {
        m.kind = Kind::script;
        const std::size_t end = t.find('\'', 17);
        if (end != std::string_view::npos) m.token = std::string(t.substr(17, end - 17));
        m.searchable = !m.token.empty();
    } else if (contains(src, "windowtype") || contains(src, "guitype") || contains(src, "spritetype") ||
               m.file.ends_with(".gui") || m.file.ends_with(".gfx")) {
        m.kind = Kind::gui;
    } else if (contains(src, "localize") || contains(src, "ck2text") || contains(t, "ocaliz") || contains(t, "ocalis")) {
        m.kind = Kind::loc;
    } else if (contains(src, "bitmapfont") || contains(src, "texture") || contains(t, "Texture ") ||
               contains(t, "building entity")) {
        m.kind = Kind::gfx;
    }
}

std::uint64_t item_key(const Message& m) {
    if (m.kind == Kind::target && !m.token.empty()) return (fnv("target|" + m.token) & (kGroupBit - 1)) | kGroupBit;
    if (m.kind == Kind::files) return (fnv("files|" + m.group) & (kGroupBit - 1)) | kGroupBit;
    return m.index + 1;
}

void Model::update(std::uint64_t index, std::string_view source, int source_line, std::string_view text,
                   std::uint64_t written, std::uint64_t counted) {
    auto it = by_index_.find(index);
    if (it == by_index_.end()) {
        Message& m = messages_.emplace_back();
        m.index = index;
        m.source = std::string(source);
        m.source_line = source_line;
        m.text = std::string(text);
        classify(m);
        it = by_index_.emplace(index, static_cast<std::uint32_t>(messages_.size() - 1)).first;
    }
    Message& m = messages_[it->second];
    m.written = written;
    m.counted = counted;
}

Message* Model::message(std::uint64_t index) {
    const auto it = by_index_.find(index);
    return it == by_index_.end() ? nullptr : &messages_[it->second];
}

void Model::mark_seen() {
    for (Message& m : messages_) m.seen = m.count();
}

std::vector<Message*> Model::item(std::uint64_t key) {
    std::vector<Message*> out;
    if (key == 0) return out;
    if ((key & kGroupBit) == 0) {
        if (Message* m = message(key - 1)) out.push_back(m);
        return out;
    }
    for (Message& m : messages_) {
        if (item_key(m) == key) out.push_back(&m);
    }
    return out;
}

bool Model::apply_click(View& view, std::uint64_t id) {
    switch (action_of(id)) {
    case Action::new_only:
        view.new_only = !view.new_only;
        return true;
    case Action::mark:
        mark_seen();
        view.note = "mark";
        return true;
    case Action::toggle: {
        const int kind = static_cast<int>(arg_of(id));
        if (!view.open.erase(kind)) view.open.insert(kind);
        return true;
    }
    case Action::select:
        view.selected = view.selected == arg_of(id) ? 0 : arg_of(id);
        return true;
    default:
        return false;
    }
}

std::vector<Model::Item> Model::items_of(Kind kind, bool new_only) const {
    std::vector<Item> out;
    std::unordered_map<std::uint64_t, std::size_t> at;
    for (std::uint32_t i = 0; i < messages_.size(); ++i) {
        const Message& m = messages_[i];
        if (m.kind != kind || (new_only && !m.is_new())) continue;
        const std::uint64_t key = item_key(m);
        auto [it, fresh] = at.try_emplace(key, out.size());
        if (fresh) out.push_back({key, {}, 0, false});
        Item& item = out[it->second];
        item.messages.push_back(i);
        item.count += m.count();
        item.fresh = item.fresh || m.is_new();
    }
    std::stable_sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.count > b.count; });
    return out;
}

std::vector<Row> Model::rows(const View& view, const Totals& totals, const Texts& t) const {
    std::vector<Row> out;
    const auto text = [&out](std::string label, std::string value = {}) {
        if (out.size() >= kMaxRows) return;
        const bool has = !value.empty();
        out.push_back({kText, std::move(label), std::move(value), has, 0});
    };
    const auto button = [&out](std::string label, std::uint64_t id) {
        if (out.size() >= kMaxRows) return;
        out.push_back({kButton, std::move(label), {}, false, id});
    };
    std::uint64_t total = 0;
    std::size_t fresh = 0;
    for (const Message& m : messages_) {
        total += m.count();
        if (m.is_new()) ++fresh;
    }
    text(t("Messages"), t.f("{0}, {1} times", {grouped(messages_.size()), grouped(total)}));
    if (!totals.from_start) text(t("Counted since the panel was turned on: earlier lines are not in it."));
    if (totals.untracked != 0) {
        text(t.f("{0} lines of further messages are not counted: the table is full.", {grouped(totals.untracked)}));
    }
    button(view.new_only ? std::string(t("Show every message")) : t.f("Show only new ({0})", {grouped(fresh)}),
           make_id(Action::new_only, 0));
    button(t("Mark all seen"), make_id(Action::mark, 0));
    if (view.note == "mark") {
        text(t("Everything so far is marked seen."));
    } else if (!view.note.empty()) {
        text(view.note);
    }
    out.push_back({kSeparator, {}, {}, false, 0});
    if (messages_.empty()) text(t("No message in the log yet."));

    static const char* const kLabels[kKinds] = {
        N_("With a file and line"),    N_("Script: triggers and effects"), N_("Undefined event targets"),
        N_("Localisation"),            N_("Interface (.gui, .gfx)"),        N_("Graphics and fonts"),
        N_("Other"),                   N_("Files the game could not open or check"),
    };
    for (int k = 0; k < kKinds; ++k) {
        const Kind kind = static_cast<Kind>(k);
        const std::vector<Item> items = items_of(kind, view.new_only);
        if (items.empty()) continue;
        std::size_t messages = 0;
        std::uint64_t count = 0;
        std::size_t news = 0;
        for (const Item& item : items) {
            messages += item.messages.size();
            count += item.count;
            if (item.fresh) ++news;
        }
        const bool open = view.open.count(k) != 0;
        const std::string head =
            news != 0 ? t.f("{0}: {1} messages, {2} times, {3} new",
                            {t(kLabels[k]), grouped(messages), grouped(count), grouped(news)})
                      : t.f("{0}: {1} messages, {2} times", {t(kLabels[k]), grouped(messages), grouped(count)});
        button(std::string(open ? "v " : "> ") + head, make_id(Action::toggle, static_cast<std::uint64_t>(k)));
        if (!open) continue;
        const std::size_t shown = std::min(items.size(), kItemsShown);
        for (std::size_t n = 0; n < shown; ++n) {
            const Item& item = items[n];
            const Message& first = messages_[item.messages.front()];
            std::string label;
            switch (kind) {
            case Kind::target: label = "event_target:" + first.token; break;
            case Kind::files: label = t.f("{0}: {1} files", {first.group, grouped(item.messages.size())}); break;
            case Kind::located: label = file_line(first) + ": " + first.text; break;
            case Kind::script:
                label = first.token.empty() || first.searchable ? first.text : first.token + ": " + first.text;
                break;
            default: label = first.text; break;
            }
            const bool selected = view.selected == item.key;
            button("    " + std::string(selected ? "v " : "> ") + (item.fresh ? "* " : "") + grouped(item.count) + "  " +
                       cut(label, 90),
                   make_id(Action::select, item.key));
            if (selected) details(out, item, t);
        }
        if (items.size() > shown) text("    " + t.f("{0} more", {grouped(items.size() - shown)}));
    }
    return out;
}

void Model::details(std::vector<Row>& out, const Item& item, const Texts& t) const {
    const std::string in = "        ";
    const auto text = [&out](std::string label, std::string value = {}) {
        if (out.size() >= kMaxRows) return;
        const bool has = !value.empty();
        out.push_back({kText, std::move(label), std::move(value), has, 0});
    };
    const auto button = [&out](std::string label, std::uint64_t id) {
        if (out.size() >= kMaxRows) return;
        out.push_back({kButton, std::move(label), {}, false, id});
    };
    const Message& m = messages_[item.messages.front()];
    if (m.kind == Kind::files) {
        std::vector<const Message*> files;
        for (const std::uint32_t i : item.messages) files.push_back(&messages_[i]);
        std::sort(files.begin(), files.end(), [](const Message* a, const Message* b) { return a->file < b->file; });
        const std::size_t shown = std::min(files.size(), kFilesShown);
        for (std::size_t i = 0; i < shown; ++i) text(in + cut(files[i]->file, 120));
        if (files.size() > shown) text(in + t.f("{0} more files", {grouped(files.size() - shown)}));
        return;
    }
    if (m.kind == Kind::target) {
        std::uint64_t script = 0, tooltip = 0;
        for (const std::uint32_t i : item.messages) (messages_[i].tooltip ? tooltip : script) += messages_[i].count();
        text(in + t("Event target"), m.token);
        text(in + t("Count"), t.f("{0} in script, {1} in tooltips", {grouped(script), grouped(tooltip)}));
    } else {
        text(in + t("Message"), cut(m.text, kMaxText));
        text(in + t("Count"), m.counted != 0 ? t.f("{0}: {1} written, {2} counted by the throttle",
                                                    {grouped(m.count()), grouped(m.written), grouped(m.counted)})
                                              : grouped(m.count()));
    }
    text(in + t("Game code"),
         t.f("[{0}:{1}], the game's C++ code, not a mod's file", {m.source, std::to_string(m.source_line)}));
    if (m.kind != Kind::target) {
        if (m.file.empty()) {
            text(in + t("File"), t("The game names no file or line."));
        } else if (!m.resolved.empty()) {
            text(in + t("File"), t.f("{0} in {1}", {file_line(m), m.layer}));
            button(in + t("Open in VS Code"), make_id(Action::open, 0));
            button(in + t("Copy the path and line"), make_id(Action::copy, 0));
        } else {
            text(in + t("File"), t.f("{0}: no layer of the playset has it", {file_line(m)}));
        }
    }
    if (!m.searchable) return;
    if (m.search == 0) {
        button(in + t.f("Find '{0}' in the playset's script", {cut(m.token, 48)}), make_id(Action::find, 0));
    } else if (m.search == 1) {
        text(in + t("Searching events, common, decisions, missions and history/countries..."));
    } else {
        text(in + t("Found"), t.f("{0} uses in {1} files ({2} ms)",
                                  {grouped(m.hit_total), grouped(m.hit_files), std::to_string(m.search_ms)}));
        for (std::size_t h = 0; h < m.hits.size(); ++h) {
            const Hit& hit = m.hits[h];
            const std::string where = cut(hit.rel, 60) + ":" + std::to_string(hit.line);
            button(in + "  " + (hit.defines ? t.f("saved at {0}", {where}) : t.f("used at {0}", {where})),
                   make_id(Action::open_hit, h));
        }
    }
}

}  // namespace script_errors
