// The script errors panel without Anvil (docs/script-errors-panel.md): the
// messages of the table with a kind, a name to search for and the file and
// line when the game names one; their groups; and the rows that
// anvil-overlay.panel v1 draws (text, button, separator). The layout follows
// the prototype of eu4-engine-lab#11: a heading per kind with totals, the
// messages with a file and line first, the start-up noise of missing files
// last and folded into one row per kind of line.
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "texts.hpp"

namespace script_errors {

// In the order the panel shows them.
enum class Kind : std::uint8_t { located, script, target, loc, gui, gfx, other, files };
inline constexpr int kKinds = 8;

// A line of a playset file that names the searched name.
struct Hit {
    std::string rel;  // the path in the game's folders, "events/x.txt"
    std::string abs;  // UTF-8
    int line = 0;
    bool defines = false;  // save_event_target_as, save_global_event_target_as
};

struct Message {
    std::uint64_t index = 0;  // in the table
    std::string source;       // the game's C++ source file
    int source_line = 0;
    std::string text;
    Kind kind = Kind::other;
    std::string token;   // a name to group or search by (event target, tag, trigger)
    bool searchable = false;
    bool tooltip = false;  // an event target of a tooltip
    std::string file;    // the game-relative file the game names
    int line = 0;        // 0: none
    std::string group;   // the kind of line of a missing file
    std::uint64_t written = 0;
    std::uint64_t counted = 0;
    std::uint64_t seen = 0;  // the count when everything was marked seen
    // Where the file is: tried once, when the panel first shows it.
    bool resolve_tried = false;
    std::string resolved;  // UTF-8 path; empty: in no layer
    std::string layer;
    // Find
    int search = 0;  // 0 not asked, 1 running, 2 done
    std::vector<Hit> hits;
    std::size_t hit_total = 0;
    std::size_t hit_files = 0;
    std::uint64_t search_ms = 0;

    std::uint64_t count() const { return written + counted; }
    bool is_new() const { return count() > seen; }
};

// What the game's text says: kind, names, file and line.
void classify(Message& message);

enum class Action : std::uint8_t { none, new_only, mark, toggle, select, open, copy, find, open_hit };

// A button's id: the action in the top byte, an item or group in the rest.
inline std::uint64_t make_id(Action a, std::uint64_t arg) { return (std::uint64_t(a) << 56) | (arg & 0x00FFFFFFFFFFFFFFull); }
inline Action action_of(std::uint64_t id) { return static_cast<Action>(id >> 56); }
inline std::uint64_t arg_of(std::uint64_t id) { return id & 0x00FFFFFFFFFFFFFFull; }

enum RowKind : std::uint32_t { kText = 1, kButton = 2, kSeparator = 3 };

struct Row {
    std::uint32_t kind = kText;
    std::string label;
    std::string value;
    bool has_value = false;
    std::uint64_t id = 0;
};

// What the player chose in the panel.
struct View {
    std::set<int> open{static_cast<int>(Kind::located)};  // open groups
    std::uint64_t selected = 0;                            // an item's key; 0 none
    bool new_only = false;
    std::string note;  // the outcome of the last click
};

struct Totals {
    std::uint64_t untracked = 0;
    bool from_start = true;  // the table started with the plugin
};

class Model {
public:
    // A message of the table, new or with new counts.
    void update(std::uint64_t index, std::string_view source, int source_line, std::string_view text,
                std::uint64_t written, std::uint64_t counted);
    std::size_t size() const { return messages_.size(); }
    Message* message(std::uint64_t index);
    void mark_seen();

    // The messages behind the item a key names (a button's argument).
    std::vector<Message*> item(std::uint64_t key);

    // Applies a click that only changes the view; false for the actions with
    // an effect outside it (open, copy, find, open_hit), which the caller does.
    bool apply_click(View& view, std::uint64_t id);

    std::vector<Row> rows(const View& view, const Totals& totals, const Texts& t) const;

private:
    struct Item {
        std::uint64_t key = 0;
        std::vector<std::uint32_t> messages;  // positions in messages_
        std::uint64_t count = 0;
        bool fresh = false;
    };
    std::vector<Item> items_of(Kind kind, bool new_only) const;
    void details(std::vector<Row>& out, const Item& item, const Texts& t) const;

    std::vector<Message> messages_;
    std::unordered_map<std::uint64_t, std::uint32_t> by_index_;
};

// The key of an item: of a message, or of the messages of one event target
// or of one kind of missing file.
std::uint64_t item_key(const Message& message);

std::string grouped(std::uint64_t n);  // 13 372 723
// At most `max` bytes, cut at a whole character, with "..." when cut.
std::string cut(std::string_view text, std::size_t max);

}  // namespace script_errors
