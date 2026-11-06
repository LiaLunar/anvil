#include "start_menu.hpp"

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <system_error>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

struct ShellLink {
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    HRESULT result = E_FAIL;

    ShellLink() {
        result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (SUCCEEDED(result)) result = link->QueryInterface(IID_PPV_ARGS(&file));
    }
    ~ShellLink() {
        if (file != nullptr) file->Release();
        if (link != nullptr) link->Release();
    }
    ShellLink(const ShellLink&) = delete;
    ShellLink& operator=(const ShellLink&) = delete;
};

}  // namespace

fs::path start_menu_link() {
    PWSTR programs = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        out = fs::path(programs) / L"Anvil Manager.lnk";
    }
    CoTaskMemFree(programs);
    return out;
}

fs::path link_target(const fs::path& link) {
    std::error_code ec;
    if (link.empty() || !fs::is_regular_file(link, ec)) return {};
    ShellLink shell;
    if (FAILED(shell.result) || FAILED(shell.file->Load(link.c_str(), STGM_READ))) return {};
    wchar_t target[MAX_PATH + 1] = {};
    if (FAILED(shell.link->GetPath(target, MAX_PATH + 1, nullptr, 0))) return {};
    return fs::path(target);
}

bool link_starts(const fs::path& link, const fs::path& exe) {
    const fs::path target = link_target(link);
    if (target.empty()) return false;
    // Windows may give the link's target long where the folder was given short
    // (8.3) or through a junction; `exe` may be gone after an uninstall.
    std::error_code ec_target, ec_exe;
    const fs::path a = fs::weakly_canonical(target, ec_target);
    const fs::path b = fs::weakly_canonical(exe, ec_exe);
    return !ec_target && !ec_exe && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool write_link(const fs::path& link, const fs::path& exe, std::string& error) {
    if (link.empty()) {
        error = std::system_category().message(ERROR_PATH_NOT_FOUND);
        return false;
    }
    ShellLink shell;
    HRESULT result = shell.result;
    if (SUCCEEDED(result)) result = shell.link->SetPath(exe.c_str());
    if (SUCCEEDED(result)) result = shell.link->SetWorkingDirectory(exe.parent_path().c_str());
    if (SUCCEEDED(result)) result = shell.file->Save(link.c_str(), TRUE);
    if (FAILED(result)) error = std::system_category().message(result);
    return SUCCEEDED(result);
}

void remove_link(const fs::path& link, const fs::path& exe) {
    if (!link_starts(link, exe)) return;
    std::error_code ec;
    fs::remove(link, ec);  // a shortcut left behind only points at nothing
}

}  // namespace anvil::manager
