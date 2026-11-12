#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anvil {

// The player's anvil.toml with `id` added to (`listed`) or removed from one
// of its lists of plugin ids: disabled_plugins (design §12, §15: "disable at
// the next start"; the Manager enables plugins again), allow_ironman or
// allow_multiplayer (§13, plugin ids or "<plugin>.<module>"), or
// enabled_modules / disabled_modules ("<plugin>.<module>" only, ADR 0011).
// Everything else stays as it was, comments and
// layout included: the id is inserted into the existing list, or a
// `<key> = ["<id>"]` line is added before the first table (or at the end); a
// removed value takes its comma with it, and its line when it stood alone. A
// text that already says so is returned unchanged. nullopt with `error` when
// `key` is not one of those lists or `id` not a plugin id, when the text does
// not parse or the key is not a list, or when the edited text would not read
// back as the same file with the id added or removed.
std::optional<std::string> with_plugin_listed(std::string_view text, std::string_view key, std::string_view id,
                                              bool listed, std::string& error);
// with_plugin_listed(text, "disabled_plugins", id, true, error).
std::optional<std::string> with_disabled_plugin(std::string_view text, std::string_view id, std::string& error);

// The same on the file: reads it (a missing file is an empty one), writes it
// through a temporary file in the same folder and replaces the old one in one
// step. False with `error` on failure; the file is then untouched. A missing
// file stays missing when there is nothing to remove.
bool set_plugin_listed(const std::filesystem::path& file, std::string_view key, std::string_view id, bool listed,
                       std::string& error);
bool add_disabled_plugin(const std::filesystem::path& file, std::string_view id, std::string& error);

}  // namespace anvil
