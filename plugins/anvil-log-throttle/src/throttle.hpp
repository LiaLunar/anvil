// What anvil-log-throttle does with a line of a log, apart from the game
// (anvil#172): which lines are written, which are only counted, and the
// notices and reports it writes instead. Thread-safe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace log_throttle {

struct Config {
    std::uint64_t repeats = 10;        // lines of a message written before its repeats are counted
    std::uint64_t report_ms = 60'000;  // a count is reported once it is this old, at the next line of any log
    bool full_log = false;             // every line is written; counts so far are still reported
};

// A line the plugin writes through the game's writer, to the sink it names.
struct Line {
    const void* sink = nullptr;
    int channel = 0;
    std::string file;  // the source file and line of the game's line
    int line = 0;
    std::string text;
    bool notice = false;  // the first line of a message that is now counted; else a report
    std::size_t message_at = 0;  // where the game's text starts in `text`
};

enum class Verdict {
    write,  // the game's line goes to the log as it is
    count,  // the game's line is only counted
};

// The lines of one sink since it was first seen.
struct SinkStats {
    std::uint64_t lines = 0;     // every line of the sink
    std::uint64_t written = 0;   // written as they were
    std::uint64_t counted = 0;   // counted instead (a notice stands in for the first of each message)
    std::uint64_t messages = 0;  // messages whose repeats were counted
};

// A message is the sink, the channel, the source file and line, and the text.
// The first `repeats` lines of a message are written; the next one is
// replaced by a notice, and from then on its lines are counted. A count is
// reported when it is `report_ms` old, at the next line of any sink, and when
// its sink closes. At most `max_messages` messages are tracked: when the table
// is full, the messages that are not counted yet are forgotten (they start
// again from 0), and when every tracked message is counted, new ones are
// written untracked. A line is never counted under a message whose text
// differs, so no message is lost to a hash collision.
class Throttle {
public:
    explicit Throttle(std::size_t max_messages = 16384) : max_messages_(max_messages) {}

    // A line of a sink. The lines to write first (reports that are due, and
    // the notice of a message that is counted from this line on) are appended
    // to `out`, in order; the caller writes them, then the game's line when
    // the verdict is `write`.
    Verdict record(const void* sink, int channel, std::string_view file, int line, std::string_view text,
                   std::uint64_t now_ms, const Config& config, std::vector<Line>& out);

    // The sink closes: the reports of its counts are appended to `out`, its
    // messages are forgotten (the game may reuse its address) and its stats
    // are returned.
    SinkStats close(const void* sink, std::uint64_t now_ms, std::vector<Line>& out);

    SinkStats stats(const void* sink) const;
    std::size_t tracked() const;

private:
    struct Counted {
        Line line;  // the message, as the game wrote it
        std::uint64_t pending = 0;
        std::uint64_t since_ms = 0;  // of the notice or the last report
    };
    struct Entry {
        const void* sink = nullptr;
        std::uint64_t seen = 0;
        std::unique_ptr<Counted> counted;
    };

    void report_due(std::uint64_t now_ms, std::uint64_t report_ms, std::vector<Line>& out);
    void forget_uncounted();

    std::size_t max_messages_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::vector<Counted*> counted_;  // every counted message, for the reports
    std::unordered_map<const void*, SinkStats> sinks_;
    std::uint64_t next_scan_ms_ = 0;
};

// The texts the plugin writes; the game's writer puts its own
// "[<file>:<line>]: " before them.
std::string notice_text(std::uint64_t repeats, std::uint64_t report_ms, std::string_view text);
std::string report_text(std::uint64_t count, std::uint64_t elapsed_ms, std::string_view text);

// The `logs` setting: file names separated by commas, compared without case,
// or "*" for every log.
struct LogNames {
    bool all = false;
    std::vector<std::string> names;  // lower case

    static LogNames parse(std::string_view setting);
    // Whether the log at `path` (any folder, / or \) is one of them.
    bool matches(std::string_view path) const;
};

}  // namespace log_throttle
