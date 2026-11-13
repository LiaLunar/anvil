// anvil-log-throttle (plugins/anvil-log-throttle): the throttling on its own,
// and the plugin end to end. The DLL loads through the host table against a
// synthetic image with the file sink class of eu4.exe 1.37.5 (its vtable and
// the declared bytes of symbols.toml at their RVAs); lines written through
// that vtable reach a stand-in of the game's writer as the plugin decides.
// Usage: anvil_log_throttle_tests <folder with one folder per plugin>
#include "plugin_set_fixture.hpp"

#include "../plugins/anvil-log-throttle/src/errors_model.hpp"
#include "../plugins/anvil-log-throttle/src/playset.hpp"
#include "../plugins/anvil-log-throttle/src/tally.hpp"
#include "../plugins/anvil-log-throttle/src/texts.hpp"
#include "../plugins/anvil-log-throttle/src/throttle.hpp"

#include <anvil/anvil_log_throttle.h>
#include <anvil/anvil_plugin.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace anvil;
namespace lt = log_throttle;
namespace fs = std::filesystem;

constexpr const char* kBuild = "eu4-1.37.5-steam-win64";

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// ---- the throttle on its own ----------------------------------------------------

lt::Verdict record(lt::Throttle& t, const void* sink, std::string_view text, std::uint64_t now,
                   std::vector<lt::Line>& out, const lt::Config& config = {}, int line = 7256, int channel = 0x10003) {
    return t.record(sink, channel, "C:\\src\\triggerimplementation.cpp", line, text, now, config, out);
}

void throttle_tests() {
    const int a = 0, b = 0;  // two sinks
    run("log throttle: the first lines are written, then a notice, then counts", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 3;
        std::vector<lt::Line> out;
        for (int i = 0; i < 3; ++i) CHECK(record(t, &a, "Unknown culture group x", 100, out, c) == lt::Verdict::write);
        CHECK(out.empty());
        CHECK(record(t, &a, "Unknown culture group x", 100, out, c) == lt::Verdict::count);
        CHECK(out.size() == 1 && out[0].notice && out[0].sink == &a && out[0].channel == 0x10003 &&
              out[0].line == 7256 && out[0].file == "C:\\src\\triggerimplementation.cpp");
        CHECK(!out.empty() && out[0].text ==
                                  "[anvil-log-throttle] logged 3 times; further repeats are counted and reported "
                                  "every 60 s: Unknown culture group x");
        CHECK(!out.empty() && out[0].text.substr(out[0].message_at) == "Unknown culture group x");
        out.clear();
        for (int i = 0; i < 5; ++i) CHECK(record(t, &a, "Unknown culture group x", 200, out, c) == lt::Verdict::count);
        CHECK(out.empty());
        const lt::SinkStats s = t.stats(&a);
        CHECK(s.lines == 9 && s.written == 3 && s.counted == 6 && s.messages == 1);
    });

    run("log throttle: a message is the sink, channel, source line and text", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 1;
        std::vector<lt::Line> out;
        CHECK(record(t, &a, "m", 0, out, c) == lt::Verdict::write);
        CHECK(record(t, &b, "m", 0, out, c) == lt::Verdict::write);
        CHECK(record(t, &a, "m", 0, out, c, 1) == lt::Verdict::write);
        CHECK(record(t, &a, "m", 0, out, c, 7256, 0x301) == lt::Verdict::write);
        CHECK(record(t, &a, "n", 0, out, c) == lt::Verdict::write);
        CHECK(out.empty() && t.tracked() == 5);
        CHECK(record(t, &a, "m", 0, out, c) == lt::Verdict::count && out.size() == 1);
    });

    run("log throttle: a count is reported once it is due, at the next line of any sink", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 1;
        c.report_ms = 10'000;
        std::vector<lt::Line> out;
        record(t, &a, "spam", 1'000, out, c);
        record(t, &a, "spam", 1'000, out, c);  // the notice
        for (int i = 0; i < 4; ++i) record(t, &a, "spam", 2'000 + i, out, c);
        out.clear();
        // Not due yet.
        CHECK(record(t, &b, "other 1", 10'999, out, c) == lt::Verdict::write && out.empty());
        // Due, but the due counts are looked for at most once a second.
        CHECK(record(t, &b, "other 2", 11'500, out, c) == lt::Verdict::write && out.empty());
        // Reported before the line of the other sink.
        CHECK(record(t, &b, "other 3", 12'000, out, c) == lt::Verdict::write);
        CHECK(out.size() == 1 && !out[0].notice && out[0].sink == &a &&
              out[0].text == "[anvil-log-throttle] repeated 5 more times in the last 11 s: spam" &&
              out[0].text.substr(out[0].message_at) == "spam");
        out.clear();
        // The next report counts from the last one.
        record(t, &a, "spam", 15'000, out, c);
        CHECK(out.empty());
        record(t, &b, "other 4", 21'999, out, c);
        CHECK(out.empty());
        record(t, &b, "other 5", 23'000, out, c);
        CHECK(out.size() == 1 && out[0].text == "[anvil-log-throttle] repeated 1 more times in the last 11 s: spam");
        out.clear();
        // Nothing new: no report.
        record(t, &b, "other 6", 40'000, out, c);
        CHECK(out.empty());
    });

    run("log throttle: a closing sink reports its counts and is forgotten", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 2;
        std::vector<lt::Line> out;
        for (int i = 0; i < 10; ++i) record(t, &a, "spam", 1'000, out, c);
        for (int i = 0; i < 4; ++i) record(t, &b, "spam", 1'000, out, c);
        out.clear();
        const lt::SinkStats s = t.close(&a, 1'400, out);
        CHECK(s.lines == 10 && s.written == 2 && s.counted == 8 && s.messages == 1);
        CHECK(out.size() == 1 && out[0].sink == &a &&
              out[0].text == "[anvil-log-throttle] repeated 8 more times in the last 1 s: spam");
        CHECK(t.tracked() == 1 && t.stats(&a).lines == 0);
        // The address may come back as another sink: it starts from 0.
        out.clear();
        CHECK(record(t, &a, "spam", 2'000, out, c) == lt::Verdict::write && out.empty());
        // A sink without counts reports nothing.
        out.clear();
        lt::Throttle quiet;
        record(quiet, &a, "once", 0, out, c);
        CHECK(quiet.close(&a, 10, out).lines == 1 && out.empty());
    });

    run("log throttle: full_log writes every line; earlier counts are still reported", [&] {
        lt::Throttle t;
        lt::Config c;
        c.repeats = 1;
        c.report_ms = 1'000;
        std::vector<lt::Line> out;
        record(t, &a, "spam", 0, out, c);
        record(t, &a, "spam", 0, out, c);
        record(t, &a, "spam", 0, out, c);
        out.clear();
        c.full_log = true;
        CHECK(record(t, &a, "spam", 500, out, c) == lt::Verdict::write && out.empty());
        CHECK(record(t, &a, "spam", 1'500, out, c) == lt::Verdict::write);
        CHECK(out.size() == 1 && out[0].text == "[anvil-log-throttle] repeated 2 more times in the last 2 s: spam");
        CHECK(t.stats(&a).written == 3);
    });

    run("log throttle: a full table forgets what is not counted, else writes untracked", [&] {
        lt::Throttle t(2);
        lt::Config c;
        c.repeats = 1;
        std::vector<lt::Line> out;
        record(t, &a, "one", 0, out, c);
        record(t, &a, "two", 0, out, c);
        CHECK(t.tracked() == 2);
        // Full: "one" and "two" are forgotten and "three" is tracked.
        record(t, &a, "three", 0, out, c);
        CHECK(t.tracked() == 1);
        CHECK(record(t, &a, "three", 0, out, c) == lt::Verdict::count && out.size() == 1);
        record(t, &a, "four", 0, out, c);
        CHECK(record(t, &a, "four", 0, out, c) == lt::Verdict::count && t.tracked() == 2);
        // Every tracked message is counted: a new one is written, every time.
        out.clear();
        for (int i = 0; i < 5; ++i) CHECK(record(t, &a, "five", 0, out, c) == lt::Verdict::write);
        CHECK(out.empty() && t.tracked() == 2);
    });

    run("log throttle: the logs setting", [] {
        const lt::LogNames d = lt::LogNames::parse("error.log");
        CHECK(!d.all && d.names == std::vector<std::string>{"error.log"});
        CHECK(d.matches("logs/error.log") && d.matches("C:\\Users\\x\\logs\\ERROR.LOG") && d.matches("error.log"));
        CHECK(!d.matches("logs/error_old.log") && !d.matches("logs/setup_error.log") && !d.matches(""));
        const lt::LogNames two = lt::LogNames::parse(" Error.log ,game.log,, ");
        CHECK(two.names == (std::vector<std::string>{"error.log", "game.log"}) && two.matches("logs/game.log"));
        const lt::LogNames all = lt::LogNames::parse("error.log, *");
        CHECK(all.all && all.matches("logs/anything.log"));
        const lt::LogNames none = lt::LogNames::parse(" , ");
        CHECK(!none.all && none.names.empty() && !none.matches("logs/error.log"));
    });
}


// ---- the table of messages and the script errors panel ---------------------------

namespace se = script_errors;

void tally_tests() {
    run("tally: nothing before keep, then every line by message, with written and counted", [] {
        lt::Tally t;
        t.add("error.log", "C:\\src\\a.cpp", 5, "lost", false);
        CHECK(!t.keeping() && t.totals().messages == 0 && t.totals().version == 0);
        t.keep(1'000);
        t.keep(2'000);
        CHECK(t.keeping() && t.totals().since_ms == 1'000);
        for (int i = 0; i < 3; ++i) t.add("error.log", "C:\\src\\a.cpp", 5, "one\r\n", false);
        for (int i = 0; i < 4; ++i) t.add("error.log", "C:\\src\\a.cpp", 5, "one", true);
        t.add("error.log", "C:\\src\\a.cpp", 6, "one", false);
        t.add("game.log", "C:\\src\\a.cpp", 5, "one", false);
        const lt::TallyTotals totals = t.totals();
        CHECK(totals.messages == 3 && totals.written == 5 && totals.counted == 4 && totals.version == 9 &&
              totals.untracked == 0);
        std::vector<lt::TallyRow> rows;
        CHECK(t.read(0, 0, 100, rows) == 3 && rows.size() == 3);
        CHECK(rows[0].index == 0 && rows[0].written == 3 && rows[0].counted == 4 && rows[0].version == 7 &&
              rows[0].message->text == "one" && rows[0].message->source == "a.cpp" &&
              rows[0].message->source_line == 5 && rows[0].message->log == "error.log");
        CHECK(rows[1].message->source_line == 6 && rows[2].message->log == "game.log");
        // Only what changed after a version; from a position; at most a capacity.
        rows.clear();
        CHECK(t.read(0, 7, 100, rows) == 3 && rows.size() == 2 && rows[0].index == 1);
        rows.clear();
        CHECK(t.read(1, 0, 1, rows) == 2 && rows.size() == 1 && rows[0].index == 1);
        rows.clear();
        CHECK(t.read(3, 0, 100, rows) == 3 && rows.empty());
        // The strings stay where they are as the table grows.
        std::vector<lt::TallyRow> before;
        t.read(0, 0, 1, before);
        for (int i = 0; i < 2000; ++i) t.add("error.log", "a.cpp", 1, "m" + std::to_string(i), false);
        std::vector<lt::TallyRow> after;
        t.read(0, 0, 1, after);
        CHECK(before[0].message == after[0].message && after[0].message->text == "one");
    });

    run("tally: a full table counts the lines of new messages as untracked", [] {
        lt::Tally t(2);
        t.keep(0);
        t.add("error.log", "a.cpp", 1, "one", false);
        t.add("error.log", "a.cpp", 1, "two", false);
        t.add("error.log", "a.cpp", 1, "three", false);
        t.add("error.log", "a.cpp", 1, "one", true);
        const lt::TallyTotals totals = t.totals();
        CHECK(totals.messages == 2 && totals.untracked == 1 && totals.written == 3 && totals.counted == 1);
    });

    run("tally: Windows-1252 becomes UTF-8, UTF-8 stays", [] {
        CHECK(lt::utf8_text("plain") == "plain");
        CHECK(lt::utf8_text("Caf\xE9 \x80 \x9C") == "Caf\xC3\xA9 \xE2\x82\xAC \xC5\x93");
        CHECK(lt::utf8_text("\xE5\x8C\x97\xE4\xBA\xAC") == "\xE5\x8C\x97\xE4\xBA\xAC");
        // An overlong form is not UTF-8.
        CHECK(lt::utf8_text("\xC0\xAF") == "\xC3\x80\xC2\xAF");
        lt::Tally t;
        t.keep(0);
        t.add("error.log", "a.cpp", 1, "Kh\xE9mer", false);
        t.add("error.log", "a.cpp", 1, "Kh\xE9mer", true);
        std::vector<lt::TallyRow> rows;
        t.read(0, 0, 10, rows);
        CHECK(rows.size() == 1 && rows[0].message->text == "Kh\xC3\xA9mer" && rows[0].written == 1 && rows[0].counted == 1);
    });
}

se::Message classified(std::string text, std::string source = "triggerimplementation.cpp") {
    se::Message m;
    m.text = std::move(text);
    m.source = std::move(source);
    se::classify(m);
    return m;
}

// The texts a source file hands to t(...), t.f(...) and N_(...).
std::set<std::string> panel_texts(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    std::set<std::string> out;
    for (const std::string call : {"t(\"", "t.f(\"", "N_(\"", "texts()(\"", "texts().f(\""}) {
        for (std::size_t at = s.find(call); at != std::string::npos; at = s.find(call, at + 1)) {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_' || s[at - 1] == '.')) {
                if (!(call[0] == 't' && s[at - 1] == ')')) continue;
            }
            const std::size_t from = at + call.size();
            const std::size_t end = s.find('"', from);
            out.insert(s.substr(from, end - from));
        }
    }
    return out;
}

void panel_tests(const fs::path& source_dir) {
    run("panel: what the game's text says", [] {
        se::Message m = classified(
            "Script error! Script Object Token: \"has_country_flag\", Error: Trigger condition set on the wrong scope: province");
        CHECK(m.kind == se::Kind::script && m.token == "has_country_flag" && !m.searchable && m.file.empty());
        m = classified("Undefined event target: destination_province", "gamestate.cpp");
        CHECK(m.kind == se::Kind::target && m.token == "destination_province" && m.searchable && !m.tooltip);
        m = classified("Undefined event target in tooltip: destination_province", "gamestate.cpp");
        CHECK(m.kind == se::Kind::target && m.token == "destination_province" && m.tooltip);
        CHECK(se::item_key(classified("Undefined event target: destination_province")) == se::item_key(m));
        m = classified("Unknown country 'Z99' in a tag trigger");
        CHECK(m.kind == se::Kind::script && m.token == "Z99" && m.searchable);
        m = classified("Could not open file: history/provinces/1 - Stockholm.txt, error: not found");
        CHECK(m.kind == se::Kind::files && m.group == "Could not open file" && m.file == "history/provinces/1 - Stockholm.txt");
        m = classified("Invalid file when updating checksum: history/provinces/2.txt");
        CHECK(m.kind == se::Kind::files && m.group == "Invalid file when updating checksum");
        m = classified("Unexpected token: foo, near line: 4640 file: interface/countrydecisionsview.gui", "guitype.cpp");
        CHECK(m.kind == se::Kind::located && m.file == "interface/countrydecisionsview.gui" && m.line == 4640);
        m = classified("Error: \"Unexpected token\" in file: \"interface/settings.gui\" near line: 2640", "pdx_parser.cpp");
        CHECK(m.kind == se::Kind::located && m.file == "interface/settings.gui" && m.line == 2640);
        m = classified("Failed to read sprite file: interface/x.gfx", "spritetype.cpp");
        CHECK(m.kind == se::Kind::gui && m.line == 0);
        m = classified("Missing localization key: \"abc\"", "localizer.cpp");
        CHECK(m.kind == se::Kind::loc);
        m = classified("Something else");
        CHECK(m.kind == se::Kind::other && m.token.empty());
    });

    run("panel: a heading per kind, file and line first, missing files folded and last", [] {
        se::Model model;
        model.update(0, "a.cpp", 1, "Could not open file: history/a.txt", 1, 0);
        model.update(1, "a.cpp", 1, "Could not open file: history/b.txt", 1, 0);
        model.update(2, "gamestate.cpp", 7653, "Undefined event target: dest", 10, 2'468'990);
        model.update(3, "gamestate.cpp", 7654, "Undefined event target in tooltip: dest", 3, 0);
        model.update(4, "guitype.cpp", 9, "bad, near line: 12 file: interface/x.gui", 1, 0);
        model.update(5, "t.cpp", 1, "Script error! Script Object Token: \"development\", Error: x", 2, 0);
        se::View view;
        const se::Texts en;
        std::vector<se::Row> rows = model.rows(view, {}, en);
        std::vector<std::string> buttons;
        for (const se::Row& r : rows) {
            if (r.kind == se::kButton) buttons.push_back(r.label);
        }
        CHECK(rows[0].label == "Messages" && rows[0].value == "6, 2 469 008 times");
        CHECK(buttons.size() == 7);
        if (buttons.size() != 7) for (const std::string& b : buttons) std::printf("    button: %s\n", b.c_str());
        CHECK(buttons.size() == 7 && buttons[0] == "Show only new (6)" && buttons[1] == "Mark all seen" &&
              buttons[2] == "v With a file and line: 1 messages, 1 times, 1 new" &&
              buttons[3].starts_with("    > * 1  interface/x.gui:12: ") &&
              buttons[4] == "> Script: triggers and effects: 1 messages, 2 times, 1 new" &&
              buttons[5] == "> Undefined event targets: 2 messages, 2 469 003 times, 1 new" &&
              buttons[6] == "> Files the game could not open or check: 2 messages, 2 times, 1 new");
        // Open the event targets: one item for both messages of the target.
        CHECK(model.apply_click(view, se::make_id(se::Action::toggle, static_cast<int>(se::Kind::target))));
        rows = model.rows(view, {}, en);
        std::uint64_t target = 0;
        for (const se::Row& r : rows) {
            if (r.label == "    > * 2 469 003  event_target:dest") target = se::arg_of(r.id);
        }
        CHECK(target != 0 && model.item(target).size() == 2);
        CHECK(model.apply_click(view, se::make_id(se::Action::select, target)));
        rows = model.rows(view, {}, en);
        bool count = false, find = false;
        for (const se::Row& r : rows) {
            if (r.label.ends_with("Count") && r.value == "2 469 000 in script, 3 in tooltips") count = true;
            if (r.label.ends_with("Find 'dest' in the playset's script") && se::action_of(r.id) == se::Action::find) find = true;
        }
        CHECK(count && find);
        // The missing files: one row per kind of line.
        CHECK(model.apply_click(view, se::make_id(se::Action::toggle, static_cast<int>(se::Kind::files))));
        rows = model.rows(view, {}, en);
        bool folded = false;
        for (const se::Row& r : rows) {
            if (r.label == "    > * 2  Could not open file: 2 files") folded = true;
        }
        CHECK(folded);
        // Mark all seen, then only new: nothing; a new line of one message brings it back.
        CHECK(model.apply_click(view, se::make_id(se::Action::mark, 0)));
        CHECK(model.apply_click(view, se::make_id(se::Action::new_only, 0)));
        rows = model.rows(view, {}, en);
        int headings = 0;
        for (const se::Row& r : rows) {
            if (se::action_of(r.id) == se::Action::toggle) ++headings;
        }
        CHECK(headings == 0);
        model.update(5, "t.cpp", 1, "Script error! Script Object Token: \"development\", Error: x", 3, 0);
        rows = model.rows(view, {}, en);
        headings = 0;
        for (const se::Row& r : rows) {
            if (se::action_of(r.id) == se::Action::toggle) ++headings;
        }
        CHECK(headings == 1 && !model.apply_click(view, se::make_id(se::Action::open, 0)));
        // Not from the start, and a full table: said in the header.
        rows = model.rows(view, {7, false}, en);
        CHECK(rows[1].label == "Counted since the panel was turned on: earlier lines are not in it." &&
              rows[2].label == "7 lines of further messages are not counted: the table is full.");
    });

    run("panel: rows stay within what anvil-overlay.panel takes", [] {
        se::Model model;
        for (int i = 0; i < 5000; ++i) {
            model.update(static_cast<std::uint64_t>(i), "t.cpp", 1, std::string(5000, 'x') + std::to_string(i), 1, 0);
        }
        se::View view;
        view.open = {0, 1, 2, 3, 4, 5, 6, 7};
        view.selected = 1;
        const std::vector<se::Row> rows = model.rows(view, {}, se::Texts{se::Lang::pl});
        CHECK(rows.size() <= 1024);
        for (const se::Row& r : rows) CHECK(r.label.size() <= 4096 && r.value.size() <= 4096);
        CHECK(se::cut("\xC5\x82\xC5\x82\xC5\x82", 5) == "\xC5\x82..." && se::cut("a\xC5\x82\xC5\x82\xC5\x82", 5) == "a..." && se::cut("abcdef", 5) == "ab...");
    });

    run("panel: every text has every language, with the same placeholders", [&] {
        std::size_t count = 0;
        const se::TextEntry* entries = se::text_entries(count);
        std::set<std::string> known;
        const auto holes = [](std::string_view s) {
            std::set<std::string> out;
            for (std::size_t i = 0; i + 2 < s.size(); ++i) {
                if (s[i] == '{' && s[i + 2] == '}' && s[i + 1] >= '0' && s[i + 1] <= '9') out.insert(std::string(s.substr(i, 3)));
            }
            return out;
        };
        for (std::size_t i = 0; i < count; ++i) {
            known.insert(entries[i].english);
            for (const char* tr : entries[i].translated) {
                if (tr == nullptr || *tr == '\0' || holes(tr) != holes(entries[i].english)) {
                    std::printf("  translation of '%s': %s\n", entries[i].english, tr == nullptr ? "(none)" : tr);
                    CHECK(false);
                }
            }
        }
        std::set<std::string> used;
        for (const char* file : {"errors_model.cpp", "panel.cpp"}) {
            for (const std::string& text : panel_texts(source_dir / "plugins" / "anvil-log-throttle" / "src" / file)) used.insert(text);
        }
        for (const std::string& text : used) {
            if (!known.count(text)) std::printf("  not in texts.cpp: %s\n", text.c_str());
            CHECK(known.count(text) == 1);
        }
        for (const std::string& text : known) {
            if (!used.count(text)) std::printf("  not used: %s\n", text.c_str());
            CHECK(used.count(text) == 1);
        }
        CHECK(std::string(se::Texts{se::Lang::pl}("Mark all seen")) == "Oznacz wszystko jako przejrzane" &&
              se::Texts{se::Lang::de}.f("{0} more", {"3"}) == "3 weitere" &&
              std::string(se::Texts{se::Lang::en}("Mark all seen")) == "Mark all seen" &&
              se::lang_of("zh-Hant") == se::Lang::zh_hant && se::lang_of("xx") == se::Lang::en);
    });

    run("panel: the playset's layers, a file in the last layer, and Find", [] {
        const fs::path root = fs::temp_directory_path() / ("anvil-panel-tests-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(root, ec);
        const auto put = [](const fs::path& p, const std::string& text) {
            fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << text;
        };
        put(root / "game" / "events" / "a.txt", "x = { save_event_target_as = dest }\n# dest in a comment\n");
        put(root / "game" / "interface" / "x.gui", "a");
        put(root / "user" / "mod" / "m.mod", "name=\"My mod\"\npath=\"mod/m\"\n");
        put(root / "user" / "mod" / "m" / "interface" / "x.gui", "b");
        put(root / "user" / "mod" / "m" / "common" / "b.txt", "\n\nevent_target:dest = { }\ndestination = 1\n");
        put(root / "user" / "dlc_load.json", "{\"enabled_mods\":[\"mod/m.mod\"],\"disabled_dlcs\":[]}");
        const std::vector<se::Layer> layers = se::playset_layers(root / "user" / "dlc_load.json", root / "user", root / "game");
        CHECK(layers.size() == 2 && layers[1].name == "My mod");
        std::string path, layer;
        CHECK(se::resolve(layers, "interface/x.gui", path, layer) && layer == "My mod" && path.ends_with("x.gui"));
        CHECK(!se::resolve(layers, "interface/none.gui", path, layer));
        const se::Search found = se::search(layers, "dest");
        CHECK(found.total == 2 && found.files == 2 && found.hits.size() == 2 && found.hits[0].defines &&
              found.hits[0].rel == "events/a.txt" && found.hits[1].rel == "common/b.txt" && found.hits[1].line == 3);
        CHECK(se::vscode_uri("C:\\Program Files (x86)\\a b.txt", 12) == "vscode://file/C:/Program%20Files%20%28x86%29/a%20b.txt:12");
        fs::remove_all(root, ec);
    });
}

// ---- the plugin end to end ------------------------------------------------------

// An MSVC std::string as eu4.exe lays it out.
struct GameString {
    union {
        char buffer[16];
        const char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;

    std::string text() const { return std::string(capacity > 15 ? pointer : buffer, size); }
};
static_assert(sizeof(GameString) == 32);

struct OwnedString {
    std::string text;
    GameString game{};
    explicit OwnedString(std::string t) : text(std::move(t)) {
        game.size = text.size();
        if (text.size() < 16) {
            std::memcpy(game.buffer, text.c_str(), text.size() + 1);
            game.capacity = 15;
        } else {
            game.pointer = text.c_str();
            game.capacity = text.size();
        }
    }
};

// A file sink as the game lays it out: the vtable, its file object at +0x20,
// whose path is at +0x08.
struct FakeFile {
    void* handle = nullptr;
    GameString path{};
};
struct FakeSink {
    const void* vtable = nullptr;
    std::uint8_t rest[0x18] = {};
    FakeFile* file = nullptr;
    std::uint8_t more[0x10] = {};
};
static_assert(offsetof(FakeSink, file) == 0x20 && offsetof(FakeFile, path) == 0x08);

using WriteFn = void(void* sink, int channel, const GameString* file, int line, const GameString* message);
using DestroyFn = void*(void* sink, unsigned flags);

// What the stand-ins of the game's writer and destructor got.
struct Written {
    const void* sink;
    int channel;
    std::string file;
    int line;
    std::string text;
};
std::vector<Written> g_written;
std::vector<const void*> g_destroyed;

void fake_write(void* sink, int channel, const GameString* file, int line, const GameString* message) {
    g_written.push_back({sink, channel, file->text(), line, message->text()});
}
void* fake_destroy(void* sink, unsigned) {
    g_destroyed.push_back(sink);
    return sink;
}

// eu4.exe's size and sections, reserved; pages are committed only where the
// symbols of anvil-log-throttle lie:
//   .text  0x1562000, 0x1563000  the writer and destructor (jumps to the stand-ins), the router's path read
//   .rdata 0x1D94000             the file sink's vtable 0x1D94BA8
struct FakeEu4 {
    static constexpr uint32_t kSize = 0x25E0000;
    uint8_t* base = nullptr;

    FakeEu4() {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
        commit(0x0, PAGE_READWRITE);
        commit(0x1562000, PAGE_EXECUTE_READWRITE);
        commit(0x1563000, PAGE_EXECUTE_READWRITE);
        commit(0x1D94000, PAGE_READWRITE);
        write_headers();
        jump(0x1562B90, reinterpret_cast<uintptr_t>(&fake_destroy));
        jump(0x1562BE0, reinterpret_cast<uintptr_t>(&fake_write));
        put(0x15635B9, {0x48, 0x8B, 0x57, 0x20, 0x48, 0x83, 0xC2, 0x08, 0x48, 0x83, 0x7A, 0x18, 0x10, 0x72, 0x03, 0x48,
                        0x8B, 0x12});
        put_pointer(0x1D94BA8, address(0x1562B90));
        put_pointer(0x1D94BB0, address(0x1562BE0));
        DWORD old = 0;
        VirtualProtect(base + 0x1D94000, 0x1000, PAGE_READONLY, &old);
    }
    ~FakeEu4() { VirtualFree(base, 0, MEM_RELEASE); }

    void commit(uint32_t page, DWORD protect) { VirtualAlloc(base + page, 0x1000, MEM_COMMIT, protect); }
    uintptr_t address(uint32_t rva) const { return reinterpret_cast<uintptr_t>(base) + rva; }
    const void* vtable() const { return base + 0x1D94BA8; }
    void put(uint32_t rva, std::initializer_list<uint8_t> bytes) {
        std::memcpy(base + rva, bytes.begin(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), base + rva, bytes.size());
    }
    void put_pointer(uint32_t rva, uintptr_t value) { std::memcpy(base + rva, &value, sizeof(value)); }
    // mov rax, target; jmp rax
    void jump(uint32_t rva, uintptr_t target) {
        put(rva, {0x48, 0xB8});
        put_pointer(rva + 2, target);
        put(rva + 10, {0xFF, 0xE0});
    }

    void write_headers() {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        std::memcpy(base, &dos, sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 3;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.ImageBase = reinterpret_cast<uintptr_t>(base);
        nt.OptionalHeader.SectionAlignment = 0x1000;
        nt.OptionalHeader.FileAlignment = 0x200;
        nt.OptionalHeader.SizeOfImage = kSize;
        nt.OptionalHeader.SizeOfHeaders = 0x400;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        std::memcpy(base + 0x80, &nt, sizeof(nt));
        struct Layout {
            const char* name;
            uint32_t rva, size, flags;
        };
        const Layout sections[] = {
            {".text", 0x1000, 0x1B64D78, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ},
            {".rdata", 0x1B66000, 0x44999A, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ},
            {".data", 0x1FB0000, 0x4B4FB4, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE},
        };
        uint32_t at = 0x80 + sizeof(IMAGE_NT_HEADERS64);
        for (const auto& l : sections) {
            IMAGE_SECTION_HEADER s{};
            std::memcpy(s.Name, l.name, std::strlen(l.name));
            s.VirtualAddress = l.rva;
            s.Misc.VirtualSize = l.size;
            s.Characteristics = l.flags;
            std::memcpy(base + at, &s, sizeof(s));
            at += sizeof(s);
        }
    }
};

// A sink of the fake game, with the path the game gives it.
struct Sink {
    FakeFile file;
    FakeSink sink;
    OwnedString path;
    Sink(const FakeEu4& image, std::string p) : path(std::move(p)) {
        file.path = path.game;
        sink.vtable = image.vtable();
        sink.file = &file;
    }
};

// A line as the game writes it: through the sink's vtable.
void write(const Sink& s, const std::string& text, int line = 7256, int channel = 0x10003) {
    const OwnedString file("C:\\mnt\\gsg\\eu4\\eu4\\eu4\\source\\triggerimplementation.cpp");
    const OwnedString message(text);
    auto* const* slots = static_cast<void* const*>(s.sink.vtable);
    reinterpret_cast<WriteFn*>(slots[1])(const_cast<FakeSink*>(&s.sink), channel, &file.game, line, &message.game);
}
void destroy(const Sink& s) {
    auto* const* slots = static_cast<void* const*>(s.sink.vtable);
    reinterpret_cast<DestroyFn*>(slots[0])(const_cast<FakeSink*>(&s.sink), 1);
}

std::vector<std::string> texts() {
    std::vector<std::string> out;
    for (const Written& w : g_written) out.push_back(w.text);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: anvil_log_throttle_tests <plugins folder>\n");
        return 2;
    }
    const fs::path plugins_dir = fs::absolute(argv[1]);
    throttle_tests();
    tally_tests();
    panel_tests(argc > 2 ? fs::path(argv[2]) : fs::path(ANVIL_SOURCE_DIR));

    // As in the game, the image and the host outlive the plugin.
    FakeEu4& image = *new FakeEu4;
    TestStart& start = *new TestStart(image.address(0), FakeEu4::kSize, kBuild);
    ChangeRegistry& changes = start.changes;
    ServiceBus& bus = start.bus;
    std::vector<std::string>& log = start.log;
    const fs::path data = fs::temp_directory_path() / ("anvil-log-throttle-tests-" + std::to_string(GetCurrentProcessId()));
    AnvilPlugin* plugin = nullptr;

    run("log throttle plugin: symbols verify and it swaps the file sink's write and destructor", [&] {
        const auto dir = plugins_dir / "anvil-log-throttle";
        const auto parsed = load_manifest(dir);
        CHECK(parsed.manifest && parsed.manifest->phase == Phase::Early && !parsed.manifest->affects_gameplay);
        if (!parsed.manifest) return;
        anvil::ResolvedPlugin* resolved = start.add(dir);
        CHECK(resolved != nullptr && resolved->symbols.size() == 3);
        if (resolved == nullptr) return;
        resolved->data_dir = data;
        // Three lines per message, as an environment override.
        start.environment = [](const std::string& name) -> std::optional<std::string> {
            if (name == "ANVIL_ANVIL_LOG_THROTTLE_REPEATS") return "3";
            return std::nullopt;
        };
        start.make();
        AnvilPlugin* p = start.load("anvil-log-throttle");
        CHECK(p != nullptr && changes.count(p->owner) == 2);
        if (p == nullptr) return;
        auto* const* slots = static_cast<void* const*>(image.vtable());
        CHECK(reinterpret_cast<uintptr_t>(slots[0]) != image.address(0x1562B90) &&
              reinterpret_cast<uintptr_t>(slots[1]) != image.address(0x1562BE0));
        CHECK(contains(log, "anvil-log-throttle: throttling error.log after 3 lines per message, reports every 60 s"));
        plugin = p;
    });
    if (plugin == nullptr) {
        for (const auto& l : log) std::printf("    log: %s\n", l.c_str());
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }

    Sink& game_log = *new Sink(image, "logs/game.log");
    Sink& error_log = *new Sink(image, "C:/Users/player/Documents/Paradox Interactive/Europa Universalis IV/logs/error.log");
    const std::string spam = "Script error! Script Object Token: \"x\", Error: Unknown culture group y. Root Country: RUS";

    run("log throttle plugin: other logs are written as they come", [&] {
        g_written.clear();
        for (int i = 0; i < 20; ++i) write(game_log, "Monthly tick");
        CHECK(g_written.size() == 20 && g_written[0].sink == &game_log.sink && g_written[0].text == "Monthly tick" &&
              g_written[0].line == 7256 && g_written[0].channel == 0x10003 &&
              g_written[0].file == "C:\\mnt\\gsg\\eu4\\eu4\\eu4\\source\\triggerimplementation.cpp");
    });

    run("log throttle plugin: error.log keeps three lines of a message, then a notice, then counts", [&] {
        g_written.clear();
        for (int i = 0; i < 10; ++i) {
            write(error_log, spam);
            if (i == 5) write(error_log, "short", 12);
        }
        const std::string notice =
            "[anvil-log-throttle] logged 3 times; further repeats are counted and reported every 60 s: " + spam;
        CHECK((texts() == std::vector<std::string>{spam, spam, spam, notice, "short"}));
        CHECK(g_written.size() == 5 && g_written[3].sink == &error_log.sink && g_written[3].line == 7256 &&
              g_written[3].channel == 0x10003 && g_written[4].line == 12);
        CHECK(contains(log, "anvil-log-throttle: error.log: counting the repeats of [triggerimplementation.cpp:7256]: " +
                                spam));
        // CALL_STACK, which the game's writer drops, passes untouched however often it comes.
        g_written.clear();
        for (int i = 0; i < 5; ++i) write(error_log, "CALL_STACK");
        CHECK((texts() == std::vector<std::string>(5, "CALL_STACK")));
    });

    run("log throttle plugin: anvil.log names a message without the line break it ends with", [&] {
        Sink& localization = *new Sink(image, "logs/error.log");
        const std::string message =
            "Error in localization value! Key: \"MODIFIER_ALLOW_CLIENT_STATES\", Error: \"Couldn't find Latin1 character\"";
        g_written.clear();
        for (int i = 0; i < 4; ++i) write(localization, message + "\r\n", 1063);
        // The game's lines and the notice keep it: only the bytes of anvil.log change.
        CHECK(g_written.size() == 4 && g_written[3].text.ends_with(message + "\r\n"));
        CHECK(contains(log, "anvil-log-throttle: error.log: counting the repeats of [triggerimplementation.cpp:1063]: " +
                                message));
        // Closed here, so that its count is not reported in a later test.
        destroy(localization);
    });

    run("log throttle plugin: full_log writes every line", [&] {
        g_written.clear();
        CHECK(plugin->record->settings().set_text(0, "true").applied);
        for (int i = 0; i < 4; ++i) write(error_log, spam);
        CHECK(g_written.size() == 4);
        CHECK(plugin->record->settings().set_text(0, "false").applied);
        g_written.clear();
        write(error_log, spam);
        CHECK(g_written.empty());
    });

    run("log throttle plugin: the closing log gets the counts, then the game's destructor runs", [&] {
        g_written.clear();
        g_destroyed.clear();
        destroy(error_log);
        CHECK(g_written.size() == 1 && g_written[0].sink == &error_log.sink && g_written[0].line == 7256 &&
              g_written[0].text.starts_with("[anvil-log-throttle] repeated 8 more times in the last ") &&
              g_written[0].text.ends_with(" s: " + spam));
        CHECK(g_destroyed == std::vector<const void*>{&error_log.sink});
        CHECK(contains(log, "anvil-log-throttle: error.log: 16 lines, 8 written, 8 counted (repeating messages: 1)"));
        // Another log closes without a word.
        g_written.clear();
        destroy(game_log);
        CHECK(g_written.empty() && g_destroyed.size() == 2);
    });

    run("log throttle plugin: a count is reported when it is due", [&] {
        CHECK(plugin->record->settings().set_text(2, "1").applied);
        Sink& fresh = *new Sink(image, "logs/error.log");
        g_written.clear();
        for (int i = 0; i < 6; ++i) write(fresh, spam);
        CHECK(g_written.size() == 4);
        Sleep(1100);
        write(fresh, "after a second");
        CHECK(g_written.size() == 6 && g_written[4].text.starts_with("[anvil-log-throttle] repeated 3 more times in the last ") &&
              g_written[5].text == "after a second");
    });

    run("log throttle plugin: anvil-log-throttle.messages gives the messages with their counts as they come", [&] {
        const auto* svc = static_cast<const AnvilLogMessagesService*>(
            bus.get_service(ANVIL_LOG_MESSAGES_SERVICE, ANVIL_LOG_MESSAGES_SERVICE_VERSION));
        CHECK(svc != nullptr && svc->size == sizeof(AnvilLogMessagesService));
        if (svc == nullptr) return;
        AnvilLogTotals totals{};
        totals.size = sizeof(totals);
        // The panel is off by default: nothing is kept until keep().
        CHECK(svc->totals(&totals) == ANVIL_OK && totals.keeping == 0 && totals.messages == 0);
        AnvilLogTotals small{};
        small.size = 8;
        CHECK(svc->totals(&small) == ANVIL_ERROR_INVALID_ARGUMENT && svc->totals(nullptr) == ANVIL_ERROR_INVALID_ARGUMENT);
        CHECK(svc->keep() == ANVIL_OK);
        CHECK(plugin->record->settings().set_text(2, "60").applied);
        Sink& log2 = *new Sink(image, "logs/error.log");
        g_written.clear();
        for (int i = 0; i < 25; ++i) write(log2, spam);
        write(log2, "Caf\xE9", 3);
        CHECK(g_written.size() == 5);
        CHECK(svc->totals(&totals) == ANVIL_OK && totals.keeping == 1 && totals.messages == 2 && totals.written == 4 &&
              totals.counted == 22 && totals.version == 26);
        AnvilLogMessage out[4]{};
        std::uint32_t count = 0;
        std::uint64_t next = 0;
        CHECK(svc->read(0, 0, out, 4, sizeof(AnvilLogMessage), &count, &next) == ANVIL_OK && count == 2 && next == 2);
        CHECK(out[0].index == 0 && out[0].written == 3 && out[0].counted == 22 && std::string(out[0].text) == spam &&
              std::string(out[0].source) == "triggerimplementation.cpp" && out[0].source_line == 7256 &&
              std::string(out[0].log) == "error.log");
        CHECK(std::string(out[1].text) == "Caf\xC3\xA9" && out[1].source_line == 3 && out[1].version == 26);
        // Only what changed since a version; a NULL out with no room; bad arguments.
        CHECK(svc->read(0, 25, out, 4, sizeof(AnvilLogMessage), &count, &next) == ANVIL_OK && count == 1 && out[0].index == 1);
        CHECK(svc->read(0, 0, nullptr, 0, sizeof(AnvilLogMessage), &count, &next) == ANVIL_OK && count == 0 && next == 0);
        CHECK(svc->read(0, 0, out, 4, 8, &count, &next) == ANVIL_ERROR_INVALID_ARGUMENT &&
              svc->read(0, 0, nullptr, 4, sizeof(AnvilLogMessage), &count, &next) == ANVIL_ERROR_INVALID_ARGUMENT);
        // After the log closes, its counts are what error.log says: lines plus the repeats of the reports.
        destroy(log2);
        std::uint64_t in_log = 0;
        for (const Written& w : g_written) {
            const std::string_view t = w.text;
            const std::string_view report = "[anvil-log-throttle] repeated ";
            if (t == spam) ++in_log;
            if (t.starts_with(report) && t.ends_with(spam)) in_log += std::stoull(std::string(t.substr(report.size())));
        }
        CHECK(in_log == 25);
        CHECK(svc->read(0, 0, out, 4, sizeof(AnvilLogMessage), &count, &next) == ANVIL_OK && out[0].written + out[0].counted == 25);
    });

    std::error_code ec;
    fs::remove_all(data, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
