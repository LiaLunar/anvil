// The table of the messages of the throttled logs with their counts, for the
// service anvil-log-throttle.messages (include/anvil/anvil_log_throttle.h)
// and the script errors panel. Apart from the Throttle, which forgets: a
// message stays here until the process ends. Kept only from keep() on, so a
// game that never asks for it pays nothing. Thread-safe.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace log_throttle {

// A message is its log, the game's source file and line, and the text. The
// strings never change and never move once the message is in the table.
struct TallyMessage {
    std::string log;     // the log's file name, "error.log"
    std::string source;  // the game's C++ source file name, "triggerimplementation.cpp"
    int source_line = 0;
    std::string text;    // UTF-8, without the line end
};

// A message and its counts at the time of the copy.
struct TallyRow {
    std::uint64_t index = 0;    // position in the table, in the order first seen
    std::uint64_t version = 0;  // the table's version at its last line
    std::uint64_t written = 0;  // lines of the message that reached the log
    std::uint64_t counted = 0;  // lines the throttle counted instead
    const TallyMessage* message = nullptr;
};

struct TallyTotals {
    bool keeping = false;
    std::uint64_t version = 0;   // grows with every line the table takes
    std::uint64_t since_ms = 0;  // when keeping started
    std::uint64_t messages = 0;
    std::uint64_t written = 0;
    std::uint64_t counted = 0;
    std::uint64_t untracked = 0;  // lines of new messages once the table is full
};

class Tally {
public:
    explicit Tally(std::size_t max_messages = 32768) : max_messages_(max_messages) {}

    // Starts the table at `now_ms`; later calls change nothing. Lines before
    // it are not in the table.
    void keep(std::uint64_t now_ms);
    bool keeping() const { return keeping_.load(std::memory_order_acquire); }

    // A line of a throttled log: `source` as the game passes it (a path; the
    // table keeps its file name), `text` as the game wrote it, `counted` when
    // the throttle counted it instead of writing it. Nothing while not kept.
    void add(std::string_view log, std::string_view source, int line, std::string_view text, bool counted);

    TallyTotals totals() const;

    // The messages from position `first` on whose version is above
    // `changed_after`, at most `capacity`, appended to `out`; returns the
    // position after the last one looked at (the table's size when it got
    // to the end).
    std::uint64_t read(std::uint64_t first, std::uint64_t changed_after, std::size_t capacity,
                       std::vector<TallyRow>& out) const;

private:
    struct Counts {
        std::uint64_t written = 0;
        std::uint64_t counted = 0;
        std::uint64_t version = 0;
        std::string raw;  // the game's bytes when they are not the UTF-8 text
    };

    std::size_t max_messages_;
    std::atomic<bool> keeping_{false};
    mutable std::mutex mutex_;
    std::deque<TallyMessage> messages_;  // a deque: its elements never move
    std::vector<Counts> counts_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;
    TallyTotals totals_;
};

// The game writes its logs in Windows-1252, or UTF-8 with a mod such as Mei:
// valid UTF-8 stays as it is, anything else is read as Windows-1252.
std::string utf8_text(std::string_view bytes);

// The file name of a path with / or \.
std::string_view file_name(std::string_view path);

// The text without the line break it may end with.
std::string_view without_line_end(std::string_view text);

}  // namespace log_throttle
