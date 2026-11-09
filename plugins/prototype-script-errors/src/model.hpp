// PROTOTYPE (eu4-engine-lab#11), throwaway: never merged. What the script-error
// panel knows about error.log: messages grouped as anvil-log-throttle groups
// them (source line + text), their counts with the throttle's reports added,
// a kind, a name to search for, and the file and line when the game names one.
// No Anvil here, so the dump tool runs it without the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace script_errors {

enum class Kind { script, target, files, gui, load, loc, gfx, other };
inline constexpr int kKinds = 8;
const char* kind_label(Kind kind);

// A layer of the playset: the game folder or a mod.
struct Root {
    std::string name;
    std::filesystem::path path;
};

// A line of a mod or game file that names the token.
struct Hit {
    std::string layer;
    std::string rel;  // game-relative path
    std::filesystem::path abs;
    int line = 0;
    std::string text;
    bool defines = false;  // save_(global_)event_target_as
};

struct Message {
    std::string source;  // the game's C++ file and line, "triggerimplementation.cpp:17083"
    std::string text;    // the message after "[source]: "
    Kind kind = Kind::other;
    std::string token;   // a name to search for in the mod
    std::string detail;  // what the game says about the token
    std::string file;    // the game-relative file the game names
    int line = 0;        // its line; 0 when the game names none
    std::filesystem::path resolved;  // that file in the playset, when found
    std::string resolved_layer;
    bool resolve_tried = false;
    std::uint64_t lines = 0;    // lines written as they were
    std::uint64_t repeats = 0;  // repeats anvil-log-throttle reported
    bool throttled = false;     // the throttle counts it
    std::size_t order = 0;      // first seen
    std::uint64_t seen = 0;     // count when the player marked everything seen
    // "Find in mod files"
    int search = 0;  // 0 not asked, 1 running, 2 done
    std::vector<Hit> hits;
    std::size_t hit_total = 0;
    std::size_t hit_files = 0;

    std::uint64_t count() const { return lines + repeats; }
    bool is_new() const { return count() > seen; }
};

class Log {
public:
    // Bytes of the log as the game writes them (Windows-1252), in order.
    void feed(std::string_view bytes);
    void reset();

    std::vector<Message>& messages() { return messages_; }
    const std::vector<Message>& messages() const { return messages_; }
    std::uint64_t lines() const { return lines_; }
    std::uint64_t notices() const { return notices_; }
    std::uint64_t reports() const { return reports_; }

private:
    void line(std::string_view raw);

    std::string carry_;
    std::unordered_map<std::string, std::size_t> index_;
    std::vector<Message> messages_;
    std::uint64_t lines_ = 0;
    std::uint64_t notices_ = 0;
    std::uint64_t reports_ = 0;
};

// The game folder, then the enabled mods of dlc_load.json in its order.
std::vector<Root> playset_roots(const std::filesystem::path& dlc_load, const std::filesystem::path& user_dir,
                                const std::filesystem::path& game_dir);
// The file the message names, in the last layer that has it.
void resolve(Message& message, const std::vector<Root>& roots);
// Every use of the token in the playset's script folders.
void search(const std::vector<Root>& roots, const Message& message, std::vector<Hit>& hits, std::size_t& total,
            std::size_t& files);

std::string grouped(std::uint64_t n);  // 760 090
std::string short_label(const Message& message);
std::string utf8_from_1252(std::string_view text);
std::filesystem::path path_from_utf8(std::string_view text);
std::string utf8_from_path(const std::filesystem::path& path);
std::string vscode_uri(const std::filesystem::path& file, int line);

}  // namespace script_errors
