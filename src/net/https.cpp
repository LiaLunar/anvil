#include "https.hpp"

#include "i18n/translate.hpp"

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace anvil::net {
namespace {

using i18n::N_;

struct Internet {
    HINTERNET handle = nullptr;
    ~Internet() {
        if (handle != nullptr) WinHttpCloseHandle(handle);
    }
};

// `error` in the player's language, `english` (when given) as written.
void fail(std::string& error, std::string* english, const char* pattern, const std::vector<std::string>& args) {
    error = i18n::format(i18n::tr(pattern), args);
    if (english != nullptr) *english = i18n::format(pattern, args);
}

}  // namespace

std::optional<std::string> https_get(const wchar_t* host, const wchar_t* path, const wchar_t* headers, size_t limit,
                                     const std::string& name, const std::wstring& agent, std::string& error,
                                     std::string* english_error) {
    Internet session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0)};
    Internet connection{session.handle != nullptr ? WinHttpConnect(session.handle, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr};
    Internet request{connection.handle != nullptr
                         ? WinHttpOpenRequest(connection.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                         : nullptr};
    if (request.handle == nullptr ||
        !WinHttpSendRequest(request.handle, headers, headers != nullptr ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(GetLastError())});
        return std::nullopt;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        fail(error, english_error, N_("{0} answered {1}"), {name, std::to_string(status)});
        return std::nullopt;
    }
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(GetLastError())});
            return std::nullopt;
        }
        if (available == 0) break;
        const size_t at = body.size();
        if (at + available > limit) {
            fail(error, english_error, N_("{0} sent more than expected"), {name});
            return std::nullopt;
        }
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, body.data() + at, available, &read)) {
            fail(error, english_error, N_("cannot reach {0} (error {1})"), {name, std::to_string(GetLastError())});
            return std::nullopt;
        }
        body.resize(at + read);
    }
    return body;
}

}  // namespace anvil::net
