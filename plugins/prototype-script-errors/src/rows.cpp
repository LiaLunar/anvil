// PROTOTYPE (eu4-engine-lab#11), throwaway. See rows.hpp.
#include "rows.hpp"

#include <algorithm>
#include <map>

namespace script_errors {

namespace {

constexpr std::size_t kMaxRows = 1000;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

struct Out {
    std::vector<Row> rows;
    void text(std::string label, std::string value = {}, bool has_value = true) {
        if (rows.size() >= kMaxRows) return;
        const bool shown = has_value && !value.empty();
        rows.push_back({kText, std::move(label), std::move(value), shown, 0});
    }
    void button(std::string label, std::uint64_t id) {
        if (rows.size() >= kMaxRows) return;
        rows.push_back({kButton, std::move(label), {}, false, id});
    }
    void separator() {
        if (rows.size() >= kMaxRows) return;
        rows.push_back({kSeparator, {}, {}, false, 0});
    }
};

std::string cut(const std::string& s, std::size_t max) { return s.size() <= max ? s : s.substr(0, max - 3) + "..."; }

std::string file_line(const Message& m) {
    return m.line > 0 ? m.file + ":" + std::to_string(m.line) : m.file;
}

std::uint32_t name_id(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return (h & 0x7FFFFFFF) | 0x1000;  // apart from the kinds' ids
}

std::vector<std::size_t> by_count(const Log& log, const View& view, auto&& keep) {
    std::vector<std::size_t> out;
    const auto& ms = log.messages();
    for (std::size_t i = 0; i < ms.size(); ++i) {
        if (view.new_only && !ms[i].is_new()) continue;
        if (keep(ms[i])) out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [&](std::size_t a, std::size_t b) { return ms[a].count() > ms[b].count(); });
    return out;
}

void details(Out& out, const Log& log, std::size_t index, const std::string& indent) {
    const Message& m = log.messages()[index];
    out.text(indent + "Message", cut(m.text, 400));
    out.text(indent + "Game code", "[" + m.source + "] (the game's C++ source, not the mod's)");
    std::string count = grouped(m.count());
    if (m.throttled) count += " (" + grouped(m.lines) + " written, " + grouped(m.repeats) + " counted by anvil-log-throttle)";
    out.text(indent + "Count", count);
    if (m.file.empty()) {
        out.text(indent + "File", "the game names no file or line");
    } else if (!m.resolved.empty()) {
        out.text(indent + "File", file_line(m) + " in " + m.resolved_layer);
        out.button(indent + "Open " + cut(file_line(m), 48) + " in VS Code", make_id(Action::open, static_cast<std::uint32_t>(index)));
        out.button(indent + "Copy the path and line", make_id(Action::copy, static_cast<std::uint32_t>(index)));
    } else {
        out.text(indent + "File", file_line(m) + ": no layer of the playset has it");
    }
    if (!m.token.empty()) {
        if (m.search == 0) {
            out.button(indent + "Find '" + cut(m.token, 32) + "' in the playset's script", make_id(Action::find, static_cast<std::uint32_t>(index)));
        } else if (m.search == 1) {
            out.text(indent + "Searching events, common, decisions, missions...");
        } else {
            out.text(indent + "Found", grouped(m.hit_total) + " uses in " + grouped(m.hit_files) + " files" +
                                           (m.hit_total > m.hits.size() ? " (first ones below)" : ""));
            for (std::size_t h = 0; h < m.hits.size(); ++h) {
                const Hit& hit = m.hits[h];
                out.button(indent + "  " + (hit.defines ? "saved at " : "used at ") + cut(hit.rel, 44) + ":" + std::to_string(hit.line),
                           make_id(Action::open_hit, static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(h)));
            }
        }
    }
}

void message_row(Out& out, const Log& log, const View& view, std::size_t i, const std::string& indent) {
    const Message& m = log.messages()[i];
    const bool selected = view.selected == i;
    std::string label = indent + (selected ? "v " : "> ") + (m.is_new() && view.variant != 2 ? "* " : "") +
                        grouped(m.count()) + "  " + short_label(m);
    out.button(label, make_id(Action::select, static_cast<std::uint32_t>(i)));
    if (selected) details(out, log, i, indent + "    ");
}

// A: one ranked list, worst first.
void top(Out& out, const Log& log, const View& view) {
    const auto order = by_count(log, view, [](const Message&) { return true; });
    const std::size_t shown = std::min<std::size_t>(order.size(), 40);
    for (std::size_t k = 0; k < shown; ++k) message_row(out, log, view, order[k], "");
    if (order.size() > shown) out.text(grouped(order.size() - shown) + " more messages, fewer occurrences");
}

// B: a heading per kind with totals; a kind opens to its messages.
void kinds(Out& out, const Log& log, const View& view) {
    for (int k = 0; k < kKinds; ++k) {
        const Kind kind = static_cast<Kind>(k);
        const auto order = by_count(log, view, [&](const Message& m) { return m.kind == kind; });
        if (order.empty()) continue;
        std::uint64_t total = 0;
        std::size_t fresh = 0;
        for (std::size_t i : order) {
            total += log.messages()[i].count();
            if (log.messages()[i].is_new()) ++fresh;
        }
        const bool open = view.open.count(static_cast<std::uint32_t>(k)) != 0;
        out.button(std::string(open ? "v " : "> ") + kind_label(kind) + ": " + grouped(order.size()) + " messages, " +
                       grouped(total) + (fresh ? ", " + grouped(fresh) + " new" : ""),
                   make_id(Action::toggle, static_cast<std::uint32_t>(k)));
        if (!open) continue;
        const std::size_t shown = std::min<std::size_t>(order.size(), 30);
        for (std::size_t n = 0; n < shown; ++n) message_row(out, log, view, order[n], "    ");
        if (order.size() > shown) out.text("    " + grouped(order.size() - shown) + " more");
    }
}

// C: the file first. Files with lines, then files the game could not open,
// then everything the game names no file for, by its name in the script.
void files(Out& out, const Log& log, const View& view) {
    const auto& ms = log.messages();
    std::map<std::string, std::vector<std::size_t>> with_line;
    std::vector<std::size_t> missing;
    std::vector<std::size_t> nameless;
    for (std::size_t i = 0; i < ms.size(); ++i) {
        if (view.new_only && !ms[i].is_new()) continue;
        if (ms[i].line > 0) {
            with_line[ms[i].file].push_back(i);
        } else if (!ms[i].file.empty()) {
            missing.push_back(i);
        } else {
            nameless.push_back(i);
        }
    }
    out.text("Files with a line (" + grouped(with_line.size()) + ")");
    for (auto& [file, list] : with_line) {
        const std::uint32_t id = name_id(file);
        const bool open = view.open.count(id) != 0;
        const Message& first = ms[list.front()];
        std::uint64_t total = 0;
        for (std::size_t i : list) total += ms[i].count();
        out.button(std::string(open ? "v " : "> ") + cut(file, 52) + "  (" + grouped(list.size()) + " lines, " + grouped(total) + ")",
                   make_id(Action::toggle, id));
        if (!open) continue;
        out.text("    Layer", first.resolved.empty() ? "no layer of the playset has it" : first.resolved_layer);
        std::sort(list.begin(), list.end(), [&](std::size_t a, std::size_t b) { return ms[a].line < ms[b].line; });
        for (std::size_t i : list) {
            out.text("    line " + std::to_string(ms[i].line), cut(ms[i].text, 200));
            if (!ms[i].resolved.empty()) out.button("    Open line " + std::to_string(ms[i].line), make_id(Action::open, static_cast<std::uint32_t>(i)));
        }
    }
    out.separator();
    {
        const std::uint32_t id = name_id("#missing");
        const bool open = view.open.count(id) != 0;
        out.button(std::string(open ? "v " : "> ") + "Files the game could not open (" + grouped(missing.size()) + ")",
                   make_id(Action::toggle, id));
        if (open) {
            std::sort(missing.begin(), missing.end(), [&](std::size_t a, std::size_t b) { return ms[a].file < ms[b].file; });
            std::string last;
            std::size_t files_shown = 0, files_total = 0;
            for (std::size_t i : missing) {
                if (ms[i].file == last) continue;
                last = ms[i].file;
                ++files_total;
                if (files_shown >= 40) continue;
                ++files_shown;
                out.text("    " + cut(ms[i].file, 60), ms[i].resolved.empty() ? "in no layer" : "in " + ms[i].resolved_layer);
            }
            if (files_total > files_shown) out.text("    " + grouped(files_total - files_shown) + " more files");
        }
    }
    out.separator();
    out.text("No file named by the game (" + grouped(nameless.size()) + ")", "by the name it reports");
    std::stable_sort(nameless.begin(), nameless.end(), [&](std::size_t a, std::size_t b) { return ms[a].count() > ms[b].count(); });
    const std::size_t shown = std::min<std::size_t>(nameless.size(), 30);
    for (std::size_t n = 0; n < shown; ++n) message_row(out, log, view, nameless[n], "    ");
    if (nameless.size() > shown) out.text("    " + grouped(nameless.size() - shown) + " more");
}

}  // namespace

const char* variant_name(int variant) {
    switch (variant) {
        case 0: return "A: worst first";
        case 1: return "B: by kind";
        default: return "C: by file";
    }
}

bool apply_view_click(View& view, Log& log, std::uint64_t id) {
    switch (action_of(id)) {
        case Action::next:
            view.variant = (view.variant + 1) % kVariants;
            view.note.clear();
            return true;
        case Action::mark:
            for (Message& m : log.messages()) m.seen = m.count();
            view.note = "everything so far marked seen";
            return true;
        case Action::new_only:
            view.new_only = !view.new_only;
            return true;
        case Action::select: {
            const std::size_t i = first_of(id);
            view.selected = view.selected == i ? kNone : i;
            return true;
        }
        case Action::toggle: {
            const std::uint32_t g = first_of(id);
            if (!view.open.erase(g)) view.open.insert(g);
            return true;
        }
        default:
            return false;
    }
}

std::vector<Row> build_rows(const Log& log, const View& view) {
    Out out;
    std::uint64_t total = 0;
    std::size_t throttled = 0;
    std::size_t fresh = 0;
    for (const Message& m : log.messages()) {
        total += m.count();
        if (m.throttled) ++throttled;
        if (m.is_new()) ++fresh;
    }
    out.text("PROTOTYPE, variant " + std::string(variant_name(view.variant)) + " (of 3)");
    out.button("Next variant >", make_id(Action::next, 0));
    out.text("Reads", view.source);
    out.text("error.log", grouped(log.lines()) + " lines: " + grouped(log.messages().size()) + " messages, " +
                              grouped(total) + " occurrences");
    out.text("Throttle", grouped(throttled) + " messages counted; counts arrive with its reports, every 60 s");
    out.button(view.new_only ? "Show all messages" : "Show only new (" + grouped(fresh) + ")", make_id(Action::new_only, 0));
    out.button("Mark all seen", make_id(Action::mark, 0));
    if (!view.note.empty()) out.text("Last", view.note);
    out.separator();
    switch (view.variant) {
        case 0: top(out, log, view); break;
        case 1: kinds(out, log, view); break;
        default: files(out, log, view); break;
    }
    return std::move(out.rows);
}

}  // namespace script_errors
