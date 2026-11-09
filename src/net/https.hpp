#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace anvil::net {

// One HTTPS GET of `host` + `path` with WinHTTP, redirects followed as WinHTTP
// does (never from https to http), sent as `agent`. The body when the answer
// is 200 and at most `limit` bytes; nullopt with `error` (in the player's
// language, naming `name`) otherwise, and the same in English in
// `english_error` when it is given (for anvil.log). Blocks: call it on a
// worker thread.
std::optional<std::string> https_get(const wchar_t* host, const wchar_t* path, const wchar_t* headers, size_t limit,
                                     const std::string& name, const std::wstring& agent, std::string& error,
                                     std::string* english_error = nullptr);

}  // namespace anvil::net
