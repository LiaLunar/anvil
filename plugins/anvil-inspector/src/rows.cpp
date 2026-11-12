#include "rows.hpp"

#include <cstdlib>

namespace anvil_inspector {

std::uint64_t make_id(Action action, std::uint32_t key) {
    return (static_cast<std::uint64_t>(action) << 32) | key;
}

Action action_of(std::uint64_t id) {
    const auto a = static_cast<std::uint32_t>(id >> 32);
    return a == 1 ? Action::toggle : a == 2 ? Action::refresh : Action::none;
}

std::uint32_t heading_key(Role role, Part part) {
    return static_cast<std::uint32_t>(role) * 16 + static_cast<std::uint32_t>(part);
}

bool apply_click(View& view, std::uint64_t id) {
    if (action_of(id) != Action::toggle) return false;
    const auto key = static_cast<std::uint32_t>(id);
    if (!view.closed.erase(key)) view.closed.insert(key);
    return true;
}

std::string milli_text(std::int32_t value) {
    const std::int64_t v = value;
    const std::int64_t whole = std::llabs(v) / 1000, part = std::llabs(v) % 1000;
    std::string out = (v < 0 ? "-" : "") + std::to_string(whole);
    if (part != 0) {
        std::string digits = std::to_string(part + 1000).substr(1);
        while (!digits.empty() && digits.back() == '0') digits.pop_back();
        out += "." + digits;
    }
    return out;
}

std::string date_text(const AnvilGameDate& d) {
    return std::to_string(d.year) + "." + std::to_string(d.month) + "." + std::to_string(d.day);
}

namespace {

class Builder {
public:
    Builder(const View& view, const Texts& t) : view_(view), t_(t) {}

    void text(std::string label, std::string value = {}) {
        if (out_.size() >= kMaxRows) return;
        const bool has = !value.empty();
        out_.push_back({kText, std::move(label), std::move(value), has, 0});
    }
    void button(std::string label, std::uint64_t id) {
        if (out_.size() >= kMaxRows) return;
        out_.push_back({kButton, std::move(label), {}, false, id});
    }
    void separator() {
        if (out_.size() >= kMaxRows) return;
        out_.push_back({kSeparator, {}, {}, false, 0});
    }
    // A heading that folds; true when it is open.
    bool heading(const std::string& indent, const std::string& label, Role role, Part part) {
        const std::uint32_t key = heading_key(role, part);
        const bool open = view_.closed.count(key) == 0;
        button(indent + (open ? "v " : "> ") + label, make_id(Action::toggle, key));
        return open;
    }
    // Why a list or a scope has nothing to show; empty when it was read.
    std::string failure(AnvilResult r) const {
        switch (r) {
        case ANVIL_OK: return {};
        case ANVIL_ERROR_UNSUPPORTED: return t_("Needs a newer anvil-game.");
        case ANVIL_ERROR_NOT_DECLARED: return t_("Not available on this game build.");
        default: return t_.f("Could not be read ({0}).", {std::to_string(static_cast<int>(r))});
        }
    }
    template <class T, class Each>
    void list(const std::string& indent, const char* title, const List<T>& l, Role role, Part part, Each each) {
        const std::string count = l.result == ANVIL_OK ? std::to_string(l.items.size()) : "-";
        if (!heading(indent, t_.f(title, {count}), role, part)) return;
        const std::string in = indent + "    ";
        if (l.result != ANVIL_OK) {
            text(in + failure(l.result));
            return;
        }
        const std::size_t shown = l.items.size() < kItemsShown ? l.items.size() : kItemsShown;
        for (std::size_t i = 0; i < shown; ++i) each(in, l.items[i]);
        if (l.items.size() > shown) text(in + t_.f("{0} more", {std::to_string(l.items.size() - shown)}));
    }

    void flags(const std::string& indent, const List<Flag>& l, Role role, Part part) {
        list(indent, N_("Flags ({0})"), l, role, part,
             [&](const std::string& in, const Flag& f) { text(in + f.name, t_.f("set {0}", {date_text(f.date)})); });
    }

    void scope(const Scope& s) {
        std::string title;
        switch (s.role) {
        case Role::province:
            title = s.name.empty() ? t_.f("Province {0}", {std::to_string(s.province)})
                                   : t_.f("Province: {0} ({1})", {s.name, std::to_string(s.province)});
            break;
        case Role::owner: title = t_.f("Owner: {0}", {s.tag}); break;
        case Role::player: title = t_.f("Your country: {0}", {s.tag}); break;
        case Role::owner_and_player: title = t_.f("Owner and your country: {0}", {s.tag}); break;
        }
        separator();
        if (!heading("", title, s.role, Part::all)) return;
        if (s.result != ANVIL_OK) {
            text("    " + failure(s.result));
            return;
        }
        const std::string in = "    ";
        flags(in, s.flags, s.role, Part::flags);
        list(in, N_("Variables ({0})"), s.variables, s.role, Part::variables,
             [&](const std::string& at, const Variable& v) { text(at + v.name, milli_text(v.value)); });
        list(in, N_("Timed modifiers ({0})"), s.modifiers, s.role, Part::modifiers,
             [&](const std::string& at, const Timed& m) {
                 std::string label = m.key;
                 if (m.flags & ANVIL_GAME_TIMED_HIDDEN) label += " (" + std::string(t_("hidden")) + ")";
                 if (m.flags & ANVIL_GAME_TIMED_RULER) label += " (" + std::string(t_("ruler modifier")) + ")";
                 text(at + label, (m.flags & ANVIL_GAME_TIMED_NO_END) ? std::string(t_("no end date"))
                                                                      : t_.f("until {0}", {date_text(m.end)}));
             });
        if (s.role == Role::province) return;
        if (!heading(in, t_("Ruler, heir and consort"), s.role, Part::monarchs)) return;
        static const char* const kKinds[3] = {N_("Ruler"), N_("Heir"), N_("Consort")};
        for (int k = 0; k < 3; ++k) {
            const Monarch& m = s.monarchs[k];
            const std::string at = in + "    ";
            if (m.result == ANVIL_ERROR_NOT_FOUND) {
                text(at + t_(kKinds[k]), t_("none"));
                continue;
            }
            if (m.result != ANVIL_OK) {
                text(at + t_(kKinds[k]), failure(m.result));
                continue;
            }
            std::string who = m.name;
            if (!m.dynasty.empty()) who += (who.empty() ? "" : " ") + m.dynasty;
            who += " (" + std::to_string(m.adm) + "/" + std::to_string(m.dip) + "/" + std::to_string(m.mil) + ")";
            text(at + t_(kKinds[k]), who);
            if (m.flags.result == ANVIL_OK) {
                const std::size_t shown = m.flags.items.size() < kItemsShown ? m.flags.items.size() : kItemsShown;
                for (std::size_t i = 0; i < shown; ++i) {
                    text(at + "    " + m.flags.items[i].name, t_.f("set {0}", {date_text(m.flags.items[i].date)}));
                }
                if (m.flags.items.size() > shown) {
                    text(at + "    " + t_.f("{0} more", {std::to_string(m.flags.items.size() - shown)}));
                }
            }
        }
    }

    std::vector<Row> take() { return std::move(out_); }

private:
    const View& view_;
    const Texts& t_;
    std::vector<Row> out_;
};

}  // namespace

std::vector<Row> rows(const Snapshot& s, const View& view, const Texts& t) {
    Builder b(view, t);
    if (!s.session) {
        b.text(t("No game is running."));
        return b.take();
    }
    b.text(t("Date"), date_text(s.date));
    b.button(t("Refresh"), make_id(Action::refresh, 0));
    b.text(t("Read when the window opens, at a new date (at most once a second), at another province view and with Refresh."));
    if (s.view == ANVIL_ERROR_UNSUPPORTED) {
        b.text(t("The province view needs a newer anvil-game."));
    } else if (s.view != ANVIL_OK) {
        b.text(t("Open a province's view to see it and its owner here."));
    }
    for (const Scope& scope : s.scopes) b.scope(scope);
    return b.take();
}

}  // namespace anvil_inspector
