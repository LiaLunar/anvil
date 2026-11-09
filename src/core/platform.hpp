#pragma once

#include <windows.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace anvil {

// What anvil.log says about the platform at start-up, so a report from Linux
// (Proton, Wine) names the Wine and the Direct3D 9 implementation it ran on.
// Only read and written to the log: nothing here changes what Anvil does.

// The Wine this process runs under, from ntdll's wine_get_version,
// wine_get_build_id and wine_get_host_version; nullopt on Windows.
struct WineInfo {
    std::string version;       // "9.0"
    std::string build_id;      // "wine-9.0 (Proton-9.0-4)", may be empty
    std::string host_system;   // "Linux", may be empty
    std::string host_release;  // the kernel release, may be empty
};
std::optional<WineInfo> detect_wine();

// "Wine: 9.0 (build wine-9.0, host Linux 6.5.0)", or "Wine: not detected".
std::string wine_log_line(const std::optional<WineInfo>& wine);

// The strings of a version resource (VS_VERSIONINFO) and its fixed file
// version ("10.0.17763.1"); empty when the resource is damaged. Only the first
// string table is read.
struct VersionResource {
    std::map<std::string, std::string> strings;  // "CompanyName" -> "DXVK" (UTF-8)
    std::string file_version;
};
VersionResource parse_version_resource(const void* data, size_t size);
// The version resource of a loaded module (also one loaded as a data file).
VersionResource module_version_resource(HMODULE module);

// The DXVK version a DXVK build keeps next to its "DXVK: " log prefix
// ("v2.7.1", "v2.7.1-498-ga6764047"); nullopt when the bytes hold none.
// DXVK's version resource copies Microsoft's d3d9.dll version, so this
// string is the only place its own version is written.
std::optional<std::string> find_dxvk_version(std::string_view bytes);

// "d3d9.dll: DXVK v2.7.1 (version resource: DXVK 10.0.17763.1, system folder)",
// "d3d9.dll: Microsoft Corporation 10.0.26100.1 (system folder)" or
// "d3d9.dll: not loaded". The folder is named, not its path: "system folder",
// "game folder" or "other folder".
std::string d3d9_log_line(HMODULE d3d9, const std::filesystem::path& game_dir);

}  // namespace anvil
