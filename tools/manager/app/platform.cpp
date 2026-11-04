#include "platform.hpp"

#include "core/crash_history.hpp"
#include "core/log.hpp"
#include "files.hpp"
#include "game.hpp"
#include "i18n/translate.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <toml++/toml.hpp>

#include <cstring>
#include <sstream>
#include <vector>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

constexpr const wchar_t* kCopyPrefix = L"AnvilManager-remove-";

fs::path settings_file() {
    PWSTR local = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) out = fs::path(local) / "Anvil" / "manager.toml";
    CoTaskMemFree(local);
    return out;
}

std::string utf8(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

template <typename Interface>
struct ComPtr {
    Interface* p = nullptr;
    ~ComPtr() {
        if (p != nullptr) p->Release();
    }
    Interface** out() { return &p; }
    Interface* operator->() const { return p; }
};

std::optional<fs::path> run_dialog(IFileDialog* dialog, HWND owner) {
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.out()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path out(path);
    CoTaskMemFree(path);
    return out;
}

struct Internet {
    HINTERNET handle = nullptr;
    ~Internet() {
        if (handle != nullptr) WinHttpCloseHandle(handle);
    }
};

// One HTTPS GET of `host` + `path`, redirects followed as WinHTTP does
// (never from https to http). The body when the answer is 200 and at most
// `limit` bytes; nullopt with `error` (naming `name`) otherwise.
std::optional<std::string> https_get(const wchar_t* host, const wchar_t* path, const wchar_t* headers, size_t limit,
                                     const std::string& name, std::string& error) {
    const std::wstring agent = L"AnvilManager/" + widen(ANVIL_VERSION);
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
        error = i18n::trf("cannot reach {0} (error {1})", {name, std::to_string(GetLastError())});
        return std::nullopt;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = i18n::trf("{0} answered {1}", {name, std::to_string(status)});
        return std::nullopt;
    }
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            error = i18n::trf("cannot reach {0} (error {1})", {name, std::to_string(GetLastError())});
            return std::nullopt;
        }
        if (available == 0) break;
        const size_t at = body.size();
        if (at + available > limit) {
            error = i18n::trf("{0} sent more than expected", {name});
            return std::nullopt;
        }
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, body.data() + at, available, &read)) {
            error = i18n::trf("cannot reach {0} (error {1})", {name, std::to_string(GetLastError())});
            return std::nullopt;
        }
        body.resize(at + read);
    }
    return body;
}

}  // namespace

fs::path own_exe_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0) return {};
        if (size < path.size()) {
            path.resize(size);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::optional<Package> load_embedded_package(std::string& error) {
    const HRSRC resource = FindResourceW(nullptr, L"ANVIL_PACKAGE", MAKEINTRESOURCEW(10));  // RT_RCDATA
    const HGLOBAL loaded = resource != nullptr ? LoadResource(nullptr, resource) : nullptr;
    const auto* data = static_cast<const uint8_t*>(loaded != nullptr ? LockResource(loaded) : nullptr);
    if (data == nullptr) {
        error = i18n::tr("AnvilManager.exe carries no Anvil package");
        return std::nullopt;
    }
    auto zip = ZipArchive::open(std::vector<uint8_t>(data, data + SizeofResource(nullptr, resource)), error);
    if (!zip) return std::nullopt;
    return load_package(std::move(*zip), ANVIL_VERSION, error);
}

ManagerSettings load_manager_settings() {
    ManagerSettings settings;
    const fs::path file = settings_file();
    const auto text = file.empty() ? std::nullopt : read_text_file(file, 1 << 20);
    if (!text) return settings;
    try {
        const toml::table table = toml::parse(*text);
        if (const auto folder = table["game_folder"].value<std::string>()) {
            settings.game_folder = fs::path(std::u8string(folder->begin(), folder->end()));
        }
        settings.check_releases = table["check_releases"].value_or(false);
        settings.asked_releases = table["asked_releases"].value_or(false);
        settings.benchmark_first_arm = table["benchmark_first_arm"].value_or(std::string());
    } catch (const toml::parse_error&) {
        // A broken file gives the defaults; the next save writes a good one.
    }
    return settings;
}

bool save_manager_settings(const ManagerSettings& settings, std::string& error) {
    const fs::path file = settings_file();
    if (file.empty()) {
        error = "no local application data folder";
        return false;
    }
    toml::table table;
    table.insert("game_folder", utf8(settings.game_folder));
    table.insert("check_releases", settings.check_releases);
    table.insert("asked_releases", settings.asked_releases);
    if (!settings.benchmark_first_arm.empty()) table.insert("benchmark_first_arm", settings.benchmark_first_arm);
    std::ostringstream text;
    text << "# Anvil Manager's settings. The game folder it manages, whether it asks GitHub for new releases and\n"
            "# the first arm of the last start-up benchmark series.\n"
         << table << "\n";
    return write_file_atomic(file, text.str(), error);
}

std::optional<fs::path> pick_folder(HWND owner, const fs::path& start) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.out())))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(widen(i18n::tr("Choose the Europa Universalis IV folder (the one with eu4.exe)")).c_str());
    if (!start.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(folder.out())))) {
            dialog->SetFolder(folder.p);
        }
    }
    return run_dialog(dialog.p, owner);
}

std::optional<fs::path> pick_zip(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.out())))) {
        return std::nullopt;
    }
    const std::wstring filter = widen(i18n::tr("Plugin zip (*.zip)"));
    const COMDLG_FILTERSPEC filters[] = {{filter.c_str(), L"*.zip"}};
    dialog->SetFileTypes(1, filters);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(widen(i18n::tr("Install a plugin from a zip")).c_str());
    return run_dialog(dialog.p, owner);
}

void open_url(std::string_view url) {
    if (!url.starts_with("https://")) return;
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_folder(const fs::path& folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return;
    ShellExecuteW(nullptr, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool copy_to_clipboard(HWND owner, std::string_view text) {
    const std::wstring wide = widen(text);
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool ok = false;
    if (const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
        if (void* locked = GlobalLock(memory)) {
            std::memcpy(locked, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

bool launch_game(const fs::path& gog_program) {
    const HINSTANCE started = gog_program.empty()
                                  ? ShellExecuteW(nullptr, L"open", kSteamLaunchUrl, nullptr, nullptr, SW_SHOWNORMAL)
                                  : ShellExecuteW(nullptr, L"open", gog_program.c_str(), nullptr,
                                                  gog_program.parent_path().c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(started) > 32;
}

bool restart_elevated(const fs::path& game_dir) {
    const fs::path exe = own_exe_path();
    // A folder ending in a backslash ("D:\") would escape the closing quote.
    std::wstring folder = game_dir.wstring();
    if (!folder.empty() && folder.back() == L'\\') folder += L'\\';
    const std::wstring parameters = L"--game \"" + folder + L"\"";
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

bool remove_after_exit(const fs::path& exe) {
    wchar_t temp[MAX_PATH + 1] = {};
    if (GetTempPathW(MAX_PATH + 1, temp) == 0) return false;
    const fs::path copy = fs::path(temp) / (std::wstring(kCopyPrefix) + std::to_wstring(GetCurrentProcessId()) + L".exe");
    if (!CopyFileW(own_exe_path().c_str(), copy.c_str(), FALSE)) return false;
    std::wstring command = L"\"" + copy.wstring() + L"\" --remove-after-exit \"" + exe.wstring() + L"\" " +
                           std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, temp, &startup, &process)) {
        DeleteFileW(copy.c_str());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int run_remove_after_exit(const fs::path& exe, DWORD pid) {
    if (_wcsicmp(exe.filename().c_str(), L"AnvilManager.exe") != 0) return 2;
    std::error_code ec;
    if (!fs::is_regular_file(exe.parent_path() / "eu4.exe", ec)) return 2;
    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(process, 60000);
        CloseHandle(process);
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (DeleteFileW(exe.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
        Sleep(250);
    }
    return 1;
}

void remove_stale_copies() {
    wchar_t temp[MAX_PATH + 1] = {};
    if (GetTempPathW(MAX_PATH + 1, temp) == 0) return;
    std::error_code ec;
    for (fs::directory_iterator it(temp, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.starts_with(kCopyPrefix) && name.ends_with(L".exe")) DeleteFileW(it->path().c_str());  // fails while running
    }
}

ReleaseCheck fetch_latest_release() {
    ReleaseCheck out;
    const auto body = https_get(kReleaseApiHost, kReleaseApiPath,
                                L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", 1u << 20,
                                "GitHub", out.error);
    if (!body) return out;
    out.release = parse_release(*body);
    if (!out.release) out.error = i18n::tr("GitHub's reply is not a release of Anvil");
    return out;
}

std::optional<Catalog> load_embedded_catalog(std::string& error) {
    const HRSRC resource = FindResourceW(nullptr, L"ANVIL_CATALOG", MAKEINTRESOURCEW(10));  // RT_RCDATA
    const HGLOBAL loaded = resource != nullptr ? LoadResource(nullptr, resource) : nullptr;
    const auto* data = static_cast<const char*>(loaded != nullptr ? LockResource(loaded) : nullptr);
    if (data == nullptr) {
        error = "AnvilManager.exe carries no plugin catalog";
        return std::nullopt;
    }
    return parse_catalog(std::string_view(data, SizeofResource(nullptr, resource)), error);
}

CatalogFetch fetch_catalog() {
    CatalogFetch out;
    const auto body = https_get(kCatalogHost, kCatalogPath, nullptr, kMaxCatalogBytes, "GitHub", out.error);
    if (!body) return out;
    out.catalog = parse_catalog(*body, out.error);
    return out;
}

ZipDownload download_pinned_zip(const CatalogPlugin& plugin) {
    ZipDownload out;
    const auto [host, path] = split_https_url(plugin.zip.url);
    if (host.empty()) {
        out.error = "not an https link";
        return out;
    }
    const auto body = https_get(host.c_str(), path.c_str(), nullptr, plugin.zip.size, "GitHub", out.error);
    if (!body) return out;
    std::vector<uint8_t> bytes(body->begin(), body->end());
    out.error = check_pinned_zip(plugin, bytes);
    if (out.error.empty()) out.bytes = std::move(bytes);
    return out;
}

SendOutcome post_report(std::string body) {
    SendOutcome out;
    const std::wstring agent = L"AnvilManager/" + widen(ANVIL_VERSION);
    Internet session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0)};
    Internet connection{session.handle != nullptr ? WinHttpConnect(session.handle, kReceiverHost, INTERNET_DEFAULT_HTTPS_PORT, 0)
                                                  : nullptr};
    Internet request{connection.handle != nullptr
                         ? WinHttpOpenRequest(connection.handle, L"POST", kReceiverPath, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                         : nullptr};
    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (request.handle == nullptr ||
        !WinHttpSendRequest(request.handle, headers, static_cast<DWORD>(-1L), body.data(), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        out.error = i18n::trf("cannot reach bench.happydwarf.dev (error {0})", {std::to_string(GetLastError())});
        return out;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available) || available == 0) break;
        const size_t at = out.body.size();
        if (at + available > (64u << 10)) break;
        out.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, out.body.data() + at, available, &read)) break;
        out.body.resize(at + read);
    }
    return out;
}

}  // namespace anvil::manager
