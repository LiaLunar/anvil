// anvil-log-throttle: the standard plugin that keeps a repeating line from
// filling the game's logs (anvil#172; design §4).
//
// Every log file of the game (logs/<name>.log) is a file sink of one class.
// The plugin swaps two slots of that class's vtable:
//   - the write (slot +8): a line of a log the `logs` setting names goes
//     through the Throttle (throttle.hpp), which says whether it is written
//     or counted and which notices and reports to write first;
//   - the deleting destructor (slot 0): before the game closes a log at exit,
//     the counts not reported yet are written to it.
// Everything else the game does with a line (the router's own count, its
// echo to a debugger) is untouched: only the bytes that reach the file differ.
// The writes come from any thread of the game.
#include <anvil/anvil.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "settings.gen.hpp"
#include "symbols.gen.hpp"

#include "throttle.hpp"

namespace {

// An MSVC std::string as eu4.exe lays it out (32 bytes): the text in place up
// to 15 characters, else a pointer to it; then the size and the capacity.
struct GameString {
    union {
        char buffer[16];
        const char* pointer;
    };
    std::uint64_t size;
    std::uint64_t capacity;

    std::string_view view() const { return {capacity > 15 ? pointer : buffer, size}; }
};
static_assert(sizeof(GameString) == 32);

// A view of `text` the game's writer can read (it only reads it). `text`
// must outlive it.
GameString game_string(const std::string& text) {
    GameString s;
    std::memset(&s, 0, sizeof(s));
    s.size = text.size();
    if (text.size() < 16) {
        std::memcpy(s.buffer, text.c_str(), text.size() + 1);
        s.capacity = 15;
    } else {
        s.pointer = text.c_str();
        s.capacity = text.size();
    }
    return s;
}

using WriteFn = void(void* sink, int channel, const GameString* file, int line, const GameString* message);
using DestroyFn = void*(void* sink, unsigned flags);

anvil::PointerSwap<WriteFn> g_write;
anvil::PointerSwap<DestroyFn> g_destroy;
// Kept for the process: the game may still write a line while the DLLs detach.
log_throttle::Throttle& g_throttle = *new log_throttle::Throttle;
const log_throttle::LogNames* g_logs = nullptr;  // set before the swaps
std::uint32_t g_file_offset = 0;  // sink+: the file object
std::uint32_t g_path_offset = 0;  // file object+: its path

std::atomic<std::int64_t> g_repeats{10};
std::atomic<std::int64_t> g_report_seconds{60};
std::atomic<bool> g_full_log{false};

// anvil.log names the first messages that are counted, not every one.
constexpr int kNamedMessages = 20;
std::atomic<int> g_named{0};

// The path of a sink's log, as the game's router reads it.
std::string_view log_path(const void* sink) {
    const auto* bytes = static_cast<const std::uint8_t*>(sink);
    const auto* file = *reinterpret_cast<const std::uint8_t* const*>(bytes + g_file_offset);
    if (file == nullptr) return {};
    return reinterpret_cast<const GameString*>(file + g_path_offset)->view();
}

std::string_view file_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// A game message may end with its own line break; anvil.log adds one.
std::string_view without_line_end(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

std::string shortened(std::string_view text) {
    constexpr std::size_t kMax = 300;
    return text.size() <= kMax ? std::string(text) : std::string(text.substr(0, kMax)) + "...";
}

void write_lines(const std::vector<log_throttle::Line>& lines) {
    WriteFn* const next = g_write.previous();
    const anvil::Host host;
    for (const log_throttle::Line& l : lines) {
        const GameString file = game_string(l.file);
        const GameString text = game_string(l.text);
        next(const_cast<void*>(l.sink), l.channel, &file, l.line, &text);
        if (!l.notice) continue;
        const int named = g_named.fetch_add(1, std::memory_order_relaxed);
        if (named < kNamedMessages) {
            host.info("{}: counting the repeats of [{}:{}]: {}", file_name(log_path(l.sink)), file_name(l.file), l.line,
                      shortened(without_line_end(std::string_view(l.text).substr(l.message_at))));
        } else if (named == kNamedMessages) {
            host.info("more messages are counted; anvil.log names no more of them");
        }
    }
}

void on_write(void* sink, int channel, const GameString* file, int line, const GameString* message) noexcept {
    WriteFn* const next = g_write.previous();
    // The game's writer drops the message CALL_STACK; it is not counted either.
    if (file == nullptr || message == nullptr || message->view() == "CALL_STACK" || !g_logs->matches(log_path(sink))) {
        next(sink, channel, file, line, message);
        return;
    }
    log_throttle::Config config;
    config.repeats = static_cast<std::uint64_t>(g_repeats.load(std::memory_order_relaxed));
    config.report_ms = static_cast<std::uint64_t>(g_report_seconds.load(std::memory_order_relaxed)) * 1000;
    config.full_log = g_full_log.load(std::memory_order_relaxed);
    std::vector<log_throttle::Line> out;
    log_throttle::Verdict verdict = log_throttle::Verdict::write;
    try {
        verdict = g_throttle.record(sink, channel, file->view(), line, message->view(), GetTickCount64(), config, out);
        write_lines(out);
    } catch (...) {
        // Only a failed allocation can throw: the game's line is written.
        verdict = log_throttle::Verdict::write;
    }
    if (verdict == log_throttle::Verdict::write) next(sink, channel, file, line, message);
}

void* on_destroy(void* sink, unsigned flags) noexcept {
    const std::string_view path = log_path(sink);
    if (g_logs->matches(path)) {
        try {
            std::vector<log_throttle::Line> out;
            const log_throttle::SinkStats stats = g_throttle.close(sink, GetTickCount64(), out);
            write_lines(out);
            if (stats.lines != 0) {
                const anvil::Host host;
                host.info("{}: {} lines, {} written, {} counted (repeating messages: {})", file_name(path), stats.lines,
                          stats.written, stats.counted, stats.messages);
            }
        } catch (...) {
            // The log closes without the last counts.
        }
    }
    return g_destroy.previous()(sink, flags);
}

struct LogThrottle : anvil::Plugin {
    void load(anvil::Host& host) override {
        g_file_offset = sym::channel_log_path.file;
        g_path_offset = sym::channel_log_path.path;
        const std::string logs = cfg::logs.get();
        g_logs = new log_throttle::LogNames(log_throttle::LogNames::parse(logs));
        g_repeats.store(cfg::repeats.get());
        g_report_seconds.store(cfg::report_seconds.get());
        g_full_log.store(cfg::full_log.get());
        host.subscribe_setting(cfg::repeats, [](std::int64_t v) { g_repeats.store(v); });
        host.subscribe_setting(cfg::report_seconds, [](std::int64_t v) { g_report_seconds.store(v); });
        host.subscribe_setting(cfg::full_log, [](bool on) { g_full_log.store(on); });
        if (!g_logs->all && g_logs->names.empty()) {
            host.info("no logs to throttle (logs = \"{}\"); nothing changed", logs);
            return;
        }
        host.swap(g_write, sym::file_sink_write_slot, &on_write);
        host.swap(g_destroy, sym::file_sink_destroy_slot, &on_destroy);
        host.info("throttling {} after {} lines per message, reports every {} s{}", g_logs->all ? "every log" : logs,
                  g_repeats.load(), g_report_seconds.load(), g_full_log.load() ? " (off: full_log)" : "");
    }
};

}  // namespace

ANVIL_PLUGIN(LogThrottle)
