#include "benchmark_setup.hpp"

#include "core/game_build.hpp"
#include "core/log.hpp"
#include "core/manifest.hpp"
#include "core/playset.hpp"
#include "core/safe_mode.hpp"
#include "core/semver.hpp"
#include "files.hpp"
#include "i18n/translate.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

bool is_standard(const std::string& id) { return id == "anvil-game" || id == "anvil-overlay"; }

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

bool process_running(const wchar_t* name) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        found = _wcsicmp(entry.szExeFile, name) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

unsigned read_unfinished(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    unsigned value = 0;
    char c = 0;
    while (in.get(c) && c >= '0' && c <= '9' && value < 1000) value = value * 10 + static_cast<unsigned>(c - '0');
    return value;
}

// A product name from Windows for the report: printable, at most 128 bytes.
std::string clean_text(std::string text) {
    std::erase_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (text.size() > 128) {
        size_t cut = 128;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;  // not inside a UTF-8 sequence
        text.resize(cut);
    }
    return text;
}

std::string cpu_name() {
    wchar_t name[256] = {};
    DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) {
        return "unknown";
    }
    const std::string text = clean_text(narrow(name));
    return text.empty() ? "unknown" : text;
}

int windows_build() {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get = ntdll ? reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return get != nullptr && get(&info) == 0 ? static_cast<int>(info.dwBuildNumber) : 0;
}

int ram_gb() {
    ULONGLONG kb = 0;
    if (!GetPhysicallyInstalledSystemMemory(&kb)) {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) kb = status.ullTotalPhys / 1024;
    }
    const ULONGLONG gb = (kb + 512 * 1024) / (1024 * 1024);
    return static_cast<int>(std::clamp<ULONGLONG>(gb, 1, 4096));
}

// Security Center's antivirus products (root\SecurityCenter2, readable
// without elevation): their display names only.
std::vector<std::string> antivirus_names() {
    std::vector<std::string> names;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWbemLocator* locator = nullptr;
    IWbemServices* services = nullptr;
    IEnumWbemClassObject* results = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                   reinterpret_cast<void**>(&locator)))) {
        BSTR space = SysAllocString(L"ROOT\\SecurityCenter2");
        if (SUCCEEDED(locator->ConnectServer(space, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services))) {
            CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            BSTR language = SysAllocString(L"WQL");
            BSTR query = SysAllocString(L"SELECT displayName FROM AntiVirusProduct");
            services->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &results);
            SysFreeString(language);
            SysFreeString(query);
        }
        SysFreeString(space);
    }
    while (results != nullptr && names.size() < 8) {
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        if (FAILED(results->Next(5000, 1, &object, &returned)) || returned == 0) break;
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(object->Get(L"displayName", 0, &value, nullptr, nullptr)) && value.vt == VT_BSTR) {
            const std::string name = clean_text(narrow(value.bstrVal));
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
        VariantClear(&value);
        object->Release();
    }
    if (results != nullptr) results->Release();
    if (services != nullptr) services->Release();
    if (locator != nullptr) locator->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return names;
}

uint64_t round_to(uint64_t value, uint64_t step) { return (value + step / 2) / step * step; }

}  // namespace

std::vector<ComparablePlugin> comparable_plugins(const cli::GameReport& report) {
    std::vector<ComparablePlugin> out;
    for (const cli::LoadedPlugin& loaded : report.load_order) {
        if (is_standard(loaded.id)) continue;
        ComparablePlugin p;
        p.id = loaded.id;
        p.version = loaded.version;
        if (loaded.mod.empty()) {
            for (const cli::GamePlugin& g : report.plugins) {
                if (g.folder == loaded.folder && g.report.manifest) p.dll = g.report.manifest->folder / widen(g.report.manifest->entry);
            }
        } else {
            for (const cli::GameModPlugin& m : report.mod_plugins) {
                if (m.id != loaded.id || m.mod != loaded.mod || !m.report.manifest) continue;
                p.source = m.workshop_id.empty() ? "local" : "workshop";
                p.workshop_item = m.workshop_id;
                p.update_pending = m.state == "update-pending";
                const fs::path folder = m.loads_from.empty() ? fs::path(widen(m.folder)) : fs::path(widen(m.loads_from));
                p.dll = folder / widen(m.report.manifest->entry);
            }
        }
        if (!p.dll.empty()) out.push_back(std::move(p));
    }
    return out;
}

std::vector<ReportPluginSource> plugin_sources(const cli::GameReport& report) {
    std::vector<ReportPluginSource> out;
    std::vector<ComparablePlugin> all = comparable_plugins(report);
    // The standard plugins load from anvil/plugins too.
    for (const cli::GamePlugin& g : report.plugins) {
        if (g.report.manifest && is_standard(g.report.manifest->id) && g.status == "loaded") {
            ComparablePlugin p;
            p.id = g.report.manifest->id;
            p.dll = g.report.manifest->folder / widen(g.report.manifest->entry);
            all.insert(all.begin(), std::move(p));
        }
    }
    for (const ComparablePlugin& p : all) {
        const std::string sha = lower(sha256_file(p.dll));
        if (sha.empty()) continue;
        out.push_back(ReportPluginSource{p.id, p.source, p.workshop_item, sha});
    }
    return out;
}

bool steam_running() { return process_running(L"steam.exe"); }
bool any_eu4_running() { return process_running(L"eu4.exe"); }

SetupFacts read_setup_facts(const fs::path& game_dir, const cli::GameReport& report, const std::string& installed_anvil,
                            const std::string& plugin) {
    SetupFacts f;
    f.steam_running = steam_running();
    f.eu4_running = any_eu4_running();
    f.unfinished_starts = read_unfinished(game_dir / "anvil" / "data" / "anvil" / "unfinished-starts.txt");
    SYSTEM_POWER_STATUS power{};
    f.on_battery = GetSystemPowerStatus(&power) && power.ACLineStatus == 0;
    f.installed_anvil = installed_anvil;
    f.anvil_game_loads = std::any_of(report.load_order.begin(), report.load_order.end(),
                                     [](const cli::LoadedPlugin& p) { return p.id == "anvil-game"; });
    f.plugin = plugin;
    for (ComparablePlugin& p : comparable_plugins(report)) {
        if (p.id == plugin) f.comparable = std::move(p);
    }
    return f;
}

std::vector<SetupCheck> setup_checks(const SetupFacts& f) {
    std::vector<SetupCheck> out;
    const auto installed = Version::parse(f.installed_anvil);
    const bool new_enough = installed && !(*installed < *Version::parse(kBenchmarkAnvil));
    using i18n::tr;
    using i18n::trf;
    out.push_back({f.installed_anvil.empty() ? std::string(tr("Anvil is not installed")) : trf("Anvil {0} is installed", {f.installed_anvil}),
                   new_enough, true,
                   trf("Install this Manager's Anvil on the Anvil tab (the benchmark needs Anvil {0} or newer).", {kBenchmarkAnvil})});
    out.push_back({tr("anvil-game loads"), f.anvil_game_loads, true, tr("Repair Anvil on the Anvil tab.")});
    out.push_back({f.steam_running ? tr("Steam is running") : tr("Steam is not running"), f.steam_running, true,
                   tr("Start Steam and sign in: the game starts with Steam, as you usually play.")});
    out.push_back({f.eu4_running ? tr("EU4 is running") : tr("EU4 is not running"), !f.eu4_running, true, tr("Close the game.")});
    const bool safe = f.unfinished_starts >= kSafeModeAfter;
    out.push_back({safe ? tr("The next start would be in safe mode") : tr("The next start is not in safe mode"), !safe, true,
                   tr("Start EU4 once with Play EU4 and reach the menu: that start is the safe one, the next is normal.")});
    const std::string id = f.plugin.empty() ? std::string(tr("The compared plugin")) : f.plugin;
    out.push_back({f.comparable ? trf("{0} {1} is enabled and loads", {id, f.comparable->version}) : trf("{0} does not load", {id}),
                   f.comparable.has_value(), true, tr("Enable it on the Plugins tab, or approve it on the Mods tab.")});
    const bool pending = f.comparable && f.comparable->update_pending;
    out.push_back({pending ? trf("A Workshop update of {0} waits for approval", {id}) : std::string(tr("No Workshop update waits for approval")),
                   !pending, true, tr("Approve or keep the update on the Mods tab first.")});
    out.push_back({f.on_battery ? tr("Running on battery: starts may be slower") : tr("Plugged in"), !f.on_battery, false,
                   tr("Plug the computer in for times like your usual ones.")});
    return out;
}

bool checks_pass(const std::vector<SetupCheck>& checks) {
    return std::all_of(checks.begin(), checks.end(), [](const SetupCheck& c) { return c.ok || !c.blocks; });
}

SeriesEnvironment read_environment(const fs::path& user_dir, const ComparablePlugin& plugin) {
    SeriesEnvironment e;
    e.steam_running = steam_running();
    std::error_code ec;
    if (fs::is_regular_file(user_dir / "dlc_load.json", ec)) e.dlc_load_sha256 = sha256_file(user_dir / "dlc_load.json");
    const ManifestResult manifest = load_manifest(plugin.dll.parent_path());
    if (manifest.manifest) e.plugin_version = manifest.manifest->version.to_string();
    e.plugin_sha256 = sha256_file(plugin.dll);
    return e;
}

std::string environment_change(const SeriesEnvironment& before, const SeriesEnvironment& now) {
    if (before.steam_running && !now.steam_running) return i18n::tr("Steam is no longer running");
    if (before.dlc_load_sha256 != now.dlc_load_sha256) return i18n::tr("the enabled mods changed");
    if (before.plugin_version != now.plugin_version) return i18n::tr("the compared plugin's version changed");
    if (before.plugin_sha256 != now.plugin_sha256) return i18n::tr("the compared plugin's files changed");
    return before == now ? "" : i18n::tr("Steam started");
}

std::string drive_kind(const fs::path& path) {
    wchar_t root[MAX_PATH] = {};
    wchar_t volume[MAX_PATH] = {};
    if (path.empty() || !GetVolumePathNameW(path.c_str(), root, MAX_PATH) ||
        !GetVolumeNameForVolumeMountPointW(root, volume, MAX_PATH)) {
        return "unknown";
    }
    std::wstring device = volume;
    if (!device.empty() && device.back() == L'\\') device.pop_back();  // the volume itself, not its root folder
    const HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "unknown";
    DWORD bytes = 0;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_DESCRIPTOR_HEADER header{};
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &header, sizeof(header), &bytes, nullptr) &&
        header.Size >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        std::vector<unsigned char> buffer(header.Size);
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(), header.Size, &bytes, nullptr)) {
            bus = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data())->BusType;
        }
    }
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    const bool seek_known = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &bytes,
                                            nullptr) != 0 && bytes >= sizeof(seek);
    CloseHandle(h);
    if (bus == BusTypeNvme) return "nvme";
    if (bus == BusTypeUsb || bus == BusTypeSd || bus == BusTypeMmc || bus == BusTypeVirtual || bus == BusTypeFileBackedVirtual) {
        return "other";
    }
    if (!seek_known) return bus == BusTypeUnknown ? "unknown" : "other";
    if (seek.IncursSeekPenalty) return "hdd";
    return bus == BusTypeSata || bus == BusTypeAta || bus == BusTypeSas || bus == BusTypeRAID ? "sata_ssd" : "other";
}

fs::path workshop_folder(const fs::path& game_dir, const fs::path& user_dir) {
    const Playset playset = read_playset(user_dir);
    for (const PlaysetMod& m : playset.mods) {
        if (!m.workshop_id.empty() && !m.folder.empty()) return m.folder.parent_path();
    }
    // <library>/steamapps/common/<game> -> <library>/steamapps/workshop/content/236850
    return game_dir.parent_path().parent_path() / "workshop" / "content" / "236850";
}

ReportSystem read_system(const fs::path& game_dir, const fs::path& user_dir) {
    ReportSystem s;
    s.cpu = cpu_name();
    s.logical_cores = std::max<int>(1, static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    s.ram_gb = ram_gb();
    s.drive_game = drive_kind(game_dir);
    s.drive_user = drive_kind(user_dir);
    std::error_code ec;
    fs::path workshop = workshop_folder(game_dir, user_dir);
    while (!workshop.empty() && !fs::exists(workshop, ec) && workshop.has_parent_path() && workshop.parent_path() != workshop) {
        workshop = workshop.parent_path();  // the drive is what counts; the folder may not exist yet
    }
    s.drive_workshop = drive_kind(workshop);
    s.windows_build = windows_build();
    s.antivirus = antivirus_names();
    return s;
}

ReportMods read_mods(const fs::path& user_dir) {
    ReportMods m;
    const Playset playset = read_playset(user_dir);
    uint64_t files = 0, bytes = 0;
    for (const PlaysetMod& mod : playset.mods) {
        ++m.enabled;
        if (mod.workshop_id.empty()) {
            ++m.local;
        } else if (std::find(m.workshop_ids.begin(), m.workshop_ids.end(), mod.workshop_id) == m.workshop_ids.end()) {
            m.workshop_ids.push_back(mod.workshop_id);
        }
        if (mod.folder.empty()) continue;
        std::vector<fs::path> found;
        list_files(mod.folder, found);
        std::error_code ec;
        for (const fs::path& f : found) {
            if (!fs::is_regular_file(fs::symlink_status(f, ec))) continue;  // a link is not followed
            const uintmax_t size = fs::file_size(f, ec);
            if (ec) continue;
            ++files;
            bytes += size;
        }
    }
    m.files = round_to(files, 100);
    m.size_mb = round_to(bytes / (1024 * 1024), 100);
    return m;
}

}  // namespace anvil::manager
