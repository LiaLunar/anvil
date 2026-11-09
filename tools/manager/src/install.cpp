#include "install.hpp"

#include "core/game_build.hpp"
#include "core/log.hpp"
#include "files.hpp"
#include "game.hpp"
#include "i18n/translate.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

fs::path below(const fs::path& game_dir, std::string_view relative) {
    return game_dir / fs::path(widen(relative)).make_preferred();
}

std::string plugin_folder_of(std::string_view path) {
    constexpr std::string_view prefix = "anvil/plugins/";
    if (!path.starts_with(prefix)) return {};
    const size_t slash = path.find('/', prefix.size());
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(prefix.size(), slash - prefix.size()));
}

// A block of a version resource: WORD length, WORD value length, WORD type,
// a NUL-terminated UTF-16 key, padding to 4 bytes, the value, padding, then
// the child blocks up to `length`.
struct VersionBlock {
    const uint8_t* start = nullptr;
    size_t length = 0;
    std::wstring key;
    const uint8_t* value = nullptr;
    size_t value_length = 0;  // as the block says: bytes, or characters for text
    const uint8_t* children = nullptr;
};

size_t align4(size_t offset) { return (offset + 3) & ~size_t(3); }

std::optional<VersionBlock> read_block(const uint8_t* start, size_t available) {
    if (available < 6) return std::nullopt;
    VersionBlock block;
    block.start = start;
    uint16_t length = 0, value_length = 0, type = 0;
    std::memcpy(&length, start, 2);
    std::memcpy(&value_length, start + 2, 2);
    std::memcpy(&type, start + 4, 2);
    if (length < 6 || length > available) return std::nullopt;
    block.length = length;
    size_t at = 6;
    while (at + 1 < length) {
        wchar_t c = 0;
        std::memcpy(&c, start + at, 2);
        at += 2;
        if (c == 0) break;
        block.key += c;
    }
    at = align4(at);
    block.value = start + std::min<size_t>(at, length);
    block.value_length = value_length;
    const size_t value_bytes = type == 1 ? size_t(value_length) * 2 : value_length;
    block.children = start + std::min<size_t>(align4(at + value_bytes), length);
    return block;
}

// The string `name` in any StringTable of the resource.
std::string version_string(const uint8_t* data, size_t size, std::wstring_view name) {
    const auto root = read_block(data, size);
    if (!root || root->key != L"VS_VERSION_INFO") return {};
    const uint8_t* end = root->start + root->length;
    for (const uint8_t* at = root->children; at < end;) {
        const auto info = read_block(at, end - at);
        if (!info) break;
        if (info->key == L"StringFileInfo") {
            const uint8_t* info_end = info->start + info->length;
            for (const uint8_t* t = info->children; t < info_end;) {
                const auto table = read_block(t, info_end - t);
                if (!table) break;
                const uint8_t* table_end = table->start + table->length;
                for (const uint8_t* s = table->children; s < table_end;) {
                    const auto text = read_block(s, table_end - s);
                    if (!text) break;
                    if (text->key == name && text->value_length > 0) {
                        const size_t chars = std::min<size_t>(text->value_length, (table_end - text->value) / 2);
                        std::wstring value(chars, L'\0');
                        std::memcpy(value.data(), text->value, chars * 2);
                        while (!value.empty() && value.back() == L'\0') value.pop_back();
                        return narrow(value);
                    }
                    s = text->start + align4(text->length);
                }
                t = table->start + align4(table->length);
            }
        }
        at = info->start + align4(info->length);
    }
    return {};
}

bool is_dir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// A file, folder or link at `path`, without following a link.
bool present(const fs::path& path) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(path, ec));
}

std::string relative_text(const fs::path& path, const fs::path& base) {
    const auto relative = path.lexically_relative(base).generic_u8string();
    return std::string(relative.begin(), relative.end());
}

bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// `path` is `item` or below it (Windows' names: any case).
bool within(std::string_view path, std::string_view item) {
    return same_name(path, item) || (path.size() > item.size() && path[item.size()] == '/' &&
                                     same_name(path.substr(0, item.size()), item));
}

// The files of each legacy text patch, as their packages lay them out (lab#104:
// EU4DLL release 93, EU4 Unicode Patch v0.1.16); version.dll is apart.
constexpr const char* kEu4dllFiles[] = {".dist.v1.json",          "README.md",          "plugins/plugin64.dll",
                                        "plugins/plugin.ini",     "plugins/Plugin.dll", "plugins/dllautoupdater.exe",
                                        "plugins/autoupdate64.bat"};
constexpr const char* kUnicodePatchFiles[] = {"plugins/eu4_unicode_patch.dll", "plugins/eu4_unicode_patch",
                                              "plugins/eu4_unicode_patch.log", "gfx/fonts/eu4-unicode"};

const char* legacy_patch_id(LegacyPatch::Kind kind) {
    return kind == LegacyPatch::Kind::Eu4dll ? "eu4dll" : "eu4-unicode-patch";
}

std::vector<LegacyPatch> find_legacy_patches(const fs::path& game_dir) {
    std::vector<LegacyPatch> out;
    // EU4DLL: its plugin, its update list, or its 32-bit d3d9.dll, which the
    // 64-bit game never loads.
    const bool d3d9 = pe_machine(game_dir / "d3d9.dll") == uint16_t(IMAGE_FILE_MACHINE_I386);
    if (present(game_dir / "plugins" / "plugin64.dll") || present(game_dir / ".dist.v1.json") || d3d9) {
        LegacyPatch patch{LegacyPatch::Kind::Eu4dll, {}};
        if (d3d9) patch.files.push_back("d3d9.dll");
        for (const char* file : kEu4dllFiles) {
            if (present(below(game_dir, file))) patch.files.push_back(file);
        }
        out.push_back(std::move(patch));
    }
    if (present(game_dir / "plugins" / "eu4_unicode_patch.dll")) {
        LegacyPatch patch{LegacyPatch::Kind::UnicodePatch, {}};
        for (const char* file : kUnicodePatchFiles) {
            if (present(below(game_dir, file))) patch.files.push_back(file);
        }
        std::vector<std::string> logs;  // EU4UnicodePatch.<something>.txt next to eu4.exe
        std::error_code ec;
        for (fs::directory_iterator it(game_dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = relative_text(it->path(), game_dir);
            if (name.size() > 20 && same_name(name.substr(0, 16), "EU4UnicodePatch.") &&
                same_name(name.substr(name.size() - 4), ".txt") && it->is_regular_file(ec)) {
                logs.push_back(name);
            }
        }
        std::sort(logs.begin(), logs.end());
        patch.files.insert(patch.files.end(), logs.begin(), logs.end());
        out.push_back(std::move(patch));
    }
    return out;
}

// "eu4dll-2026-11-09-2" as (date, number) for sorting; the number of the
// first folder of a day is 1. Other names sort first.
std::pair<std::string, int> backup_order(std::string_view name) {
    const auto digits = [](std::string_view text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    const auto date_at = [&](size_t at) {
        if (at < 1 || at + 10 > name.size() || name[at - 1] != '-') return false;
        const std::string_view date = name.substr(at, 10);
        return digits(date.substr(0, 4)) && date[4] == '-' && digits(date.substr(5, 2)) && date[7] == '-' &&
               digits(date.substr(8, 2));
    };
    if (name.size() >= 11 && date_at(name.size() - 10)) return {std::string(name.substr(name.size() - 10)), 1};
    const size_t dash = name.rfind('-');
    if (dash != std::string_view::npos && digits(name.substr(dash + 1)) && dash >= 10 && date_at(dash - 10)) {
        return {std::string(name.substr(dash - 10, 10)), std::atoi(std::string(name.substr(dash + 1)).c_str())};
    }
    return {{}, 0};
}

// What a backup folder holds, from its name.
std::string backup_name(std::string_view folder) {
    if (folder.starts_with("eu4dll+eu4-unicode-patch-")) return "EU4DLL + EU4 Unicode Patch";
    if (folder.starts_with("eu4dll-")) return "EU4DLL";
    if (folder.starts_with("eu4-unicode-patch-")) return "EU4 Unicode Patch";
    return "version.dll";
}

std::string today() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    return text;
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

}  // namespace

std::optional<uint16_t> pe_machine(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    char header[0x400] = {};
    in.read(header, sizeof(header));
    const size_t got = static_cast<size_t>(in.gcount());
    if (got < 0x40 || header[0] != 'M' || header[1] != 'Z') return std::nullopt;
    uint32_t pe = 0;
    std::memcpy(&pe, header + 0x3C, 4);
    if (pe > got - 6 || std::memcmp(header + pe, "PE\0\0", 4) != 0) return std::nullopt;
    uint16_t machine = 0;
    std::memcpy(&machine, header + pe + 4, 2);
    return machine;
}

const char* legacy_patch_name(LegacyPatch::Kind kind) {
    return kind == LegacyPatch::Kind::Eu4dll ? "EU4DLL" : "EU4 Unicode Patch";
}

std::string Installation::legacy_name() const {
    std::string out;
    for (const LegacyPatch& patch : legacy) out += (out.empty() ? "" : " + ") + std::string(legacy_patch_name(patch.kind));
    return out;
}

std::optional<Package> load_package(ZipArchive zip, std::string version, std::string& error) {
    Package package{std::move(zip), std::move(version), {}, {}};
    std::set<std::string> plugins;
    for (const ZipEntry& entry : package.zip.entries()) {
        const auto bytes = package.zip.read(entry, error);
        if (!bytes) return std::nullopt;
        package.files.push_back({entry.path, sha256_bytes(bytes->data(), bytes->size())});
        if (const std::string folder = plugin_folder_of(entry.path); !folder.empty()) plugins.insert(folder);
    }
    if (package.zip.find("version.dll") == nullptr || package.zip.find(kPlayerToml) == nullptr || plugins.empty()) {
        error = "the package has no version.dll, anvil/anvil.toml or standard plugins";
        return std::nullopt;
    }
    package.standard_plugins.assign(plugins.begin(), plugins.end());
    return package;
}

std::optional<DllProduct> read_dll_product(const fs::path& dll) {
    const HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module == nullptr) return std::nullopt;
    std::optional<DllProduct> out;
    if (const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16))) {  // RT_VERSION
        const HGLOBAL loaded = LoadResource(module, resource);
        const auto* data = static_cast<const uint8_t*>(loaded != nullptr ? LockResource(loaded) : nullptr);
        const DWORD size = SizeofResource(module, resource);
        if (data != nullptr && size > 0) {
            DllProduct product{version_string(data, size, L"ProductName"), version_string(data, size, L"ProductVersion")};
            if (!product.name.empty()) out = std::move(product);
        }
    }
    FreeLibrary(module);
    return out;
}

bool Installation::intact() const {
    return dll == Dll::Anvil && differing.empty() && extra.empty() &&
           std::all_of(missing.begin(), missing.end(), [](const std::string& path) { return path == kPlayerToml; });
}

Installation inspect_installation(const Package& package, const fs::path& game_dir, const fs::path& manager_exe) {
    Installation out;
    const fs::path dll = game_dir / "version.dll";
    if (is_file(dll)) {
        const auto product = read_dll_product(dll);
        if (product && product->name == "Anvil") {
            out.dll = Installation::Dll::Anvil;
            out.dll_version = product->version;
        } else {
            out.dll = Installation::Dll::Foreign;
        }
    }
    if (out.dll == Installation::Dll::Foreign) out.legacy = find_legacy_patches(game_dir);
    if (!out.legacy.empty() && is_dir(game_dir / "plugins")) {
        std::vector<fs::path> files;
        list_files(game_dir / "plugins", files);
        for (const fs::path& file : files) {
            const std::string path = relative_text(file, game_dir);
            const bool theirs = std::any_of(out.legacy.begin(), out.legacy.end(), [&](const LegacyPatch& patch) {
                return std::any_of(patch.files.begin(), patch.files.end(),
                                   [&](const std::string& item) { return within(path, item); });
            });
            if (!theirs) out.other_plugins.push_back(path);
        }
        std::sort(out.other_plugins.begin(), out.other_plugins.end());
    }
    {
        std::error_code ec;
        for (fs::directory_iterator it(game_dir / kLegacyBackup, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && !it->is_symlink(ec)) out.backup_folders.push_back(relative_text(it->path(), game_dir / kLegacyBackup));
        }
        std::sort(out.backup_folders.begin(), out.backup_folders.end(), [](const std::string& a, const std::string& b) {
            return std::make_pair(backup_order(a), a) < std::make_pair(backup_order(b), b);
        });
    }
    if (!out.backup_folders.empty()) {
        LegacyBackup backup;
        backup.folder = std::string(kLegacyBackup) + "/" + out.backup_folders.back();
        backup.name = backup_name(out.backup_folders.back());
        const fs::path folder = below(game_dir, backup.folder);
        std::vector<fs::path> files;
        list_files(folder, files);
        for (const fs::path& file : files) {
            const std::string path = relative_text(file, folder);
            backup.files.push_back(path);
            const bool anvil_dll = same_name(path, "version.dll") && out.dll == Installation::Dll::Anvil;
            if (!anvil_dll && present(below(game_dir, path))) backup.taken.push_back(path);
        }
        std::sort(backup.files.begin(), backup.files.end());
        std::sort(backup.taken.begin(), backup.taken.end());
        if (!backup.files.empty()) out.backup = std::move(backup);
    }
    out.anvil_folder = is_dir(game_dir / "anvil");
    out.manager_copy = is_file(game_dir / kManagerExe);
    out.manager_current = out.manager_copy && !manager_exe.empty() &&
                          (same_file(manager_exe, game_dir / kManagerExe) ||
                           sha256_file(manager_exe) == sha256_file(game_dir / kManagerExe));
    std::set<std::string> known;
    for (const Package::File& file : package.files) {
        known.insert(file.path);
        const fs::path path = below(game_dir, file.path);
        if (!is_file(path)) {
            out.missing.push_back(file.path);
        } else if (file.path != kPlayerToml && sha256_file(path) != file.sha256) {
            out.differing.push_back(file.path);
        }
    }
    for (const std::string& plugin : package.standard_plugins) {
        const fs::path folder = below(game_dir, "anvil/plugins/" + plugin);
        if (!is_dir(folder)) continue;
        std::vector<fs::path> files;
        list_files(folder, files);
        for (const fs::path& file : files) {
            const auto relative = file.lexically_relative(game_dir).generic_u8string();
            const std::string path(relative.begin(), relative.end());
            if (known.count(path) == 0) out.extra.push_back(path);
        }
    }
    std::sort(out.extra.begin(), out.extra.end());
    return out;
}

Plan plan_install(const Package& package, const Installation& installation, const fs::path& game_dir,
                  const fs::path& manager_exe, bool replace_foreign_dll, std::string date) {
    Plan plan;
    const bool foreign = installation.dll == Installation::Dll::Foreign;
    const bool legacy = foreign && !installation.legacy.empty();
    if (foreign && !legacy && !replace_foreign_dll) {
        plan.refused = i18n::tr("version.dll in the game folder is not Anvil's: another mod may use it. Remove it "
                                "first, or replace it on purpose.");
        return plan;
    }
    if (foreign) {
        // Nothing foreign is overwritten (lab#104): it moves to a new backup folder first.
        std::string id;
        for (const LegacyPatch& patch : installation.legacy) id += (id.empty() ? "" : "+") + std::string(legacy_patch_id(patch.kind));
        if (id.empty()) id = "version-dll";
        if (date.empty()) date = today();
        std::string folder = id + "-" + date;
        for (int n = 2; std::find(installation.backup_folders.begin(), installation.backup_folders.end(), folder) !=
                        installation.backup_folders.end();
             ++n) {
            folder = id + "-" + date + "-" + std::to_string(n);
        }
        const std::string backup = std::string(kLegacyBackup) + "/" + folder;
        plan.steps.push_back({Step::Kind::Move, "version.dll", backup + "/version.dll"});
        for (const LegacyPatch& patch : installation.legacy) {
            for (const std::string& file : patch.files) plan.steps.push_back({Step::Kind::Move, file, backup + "/" + file});
        }
        if (legacy) {
            plan.steps.push_back({Step::Kind::RemoveIfEmpty, "plugins"});
            plan.notes.push_back(i18n::trf("{0} is replaced: its files move to {1}. Uninstalling Anvil can put them back.",
                                           {installation.legacy_name(), backup}));
            if (!installation.other_plugins.empty()) {
                plan.notes.push_back(i18n::trf("These files in plugins are not {0}'s and stay: {1}",
                                               {installation.legacy_name(), joined(installation.other_plugins)}));
            }
        } else {
            plan.notes.push_back(i18n::trf("version.dll is not Anvil's: it moves to {0}.", {backup}));
        }
    } else if (installation.dll == Installation::Dll::Anvil && installation.dll_version != package.version) {
        plan.notes.push_back(i18n::trf("Anvil {0} is updated to {1}.", {installation.dll_version, package.version}));
    }
    const auto listed = [](const std::vector<std::string>& list, const std::string& path) {
        return std::find(list.begin(), list.end(), path) != list.end();
    };
    for (const std::string& path : installation.extra) plan.steps.push_back({Step::Kind::Remove, path});
    bool dll = false;
    for (const Package::File& file : package.files) {
        const bool write = file.path == kPlayerToml ? listed(installation.missing, file.path)
                                                    : listed(installation.missing, file.path) ||
                                                          listed(installation.differing, file.path);
        if (!write) continue;
        if (file.path == "version.dll") {
            dll = true;
        } else {
            plan.steps.push_back({Step::Kind::Write, file.path});
        }
    }
    if (installation.dll == Installation::Dll::Foreign) dll = true;
    if (!manager_exe.empty() && !installation.manager_current && !same_file(manager_exe, game_dir / kManagerExe)) {
        plan.steps.push_back({Step::Kind::CopyManager, kManagerExe});
    }
    if (dll) plan.steps.push_back({Step::Kind::Write, "version.dll"});
    if (!listed(installation.missing, kPlayerToml)) plan.notes.push_back(i18n::tr("anvil/anvil.toml is yours and stays as it is."));
    return plan;
}

Plan plan_uninstall(const Package& package, const Installation& installation, bool everything, bool restore_backup) {
    Plan plan;
    if (installation.dll == Installation::Dll::Anvil) {
        plan.steps.push_back({Step::Kind::Remove, "version.dll"});
    } else if (installation.dll == Installation::Dll::Foreign) {
        plan.notes.push_back(i18n::tr("version.dll is not Anvil's and stays."));
    }
    if (everything) {
        if (installation.anvil_folder) plan.steps.push_back({Step::Kind::RemoveTree, "anvil"});
        plan.notes.push_back(i18n::tr("Everything in the anvil folder goes: your plugins, their settings and data, "
                                      "crash reports and logs."));
    } else {
        for (const std::string& plugin : package.standard_plugins) {
            plan.steps.push_back({Step::Kind::RemoveTree, "anvil/plugins/" + plugin});
        }
        for (const Package::File& file : package.files) {
            if (file.path == "version.dll" || file.path == kPlayerToml || !plugin_folder_of(file.path).empty() ||
                file.path.starts_with("anvil/docs/")) {
                continue;
            }
            plan.steps.push_back({Step::Kind::Remove, file.path});
        }
        plan.steps.push_back({Step::Kind::RemoveTree, "anvil/docs"});
        plan.steps.push_back({Step::Kind::RemoveIfEmpty, "anvil/plugins"});
        // Crash capture creates anvil/crashes at every start (issue #108); only
        // a folder without reports goes.
        plan.steps.push_back({Step::Kind::RemoveIfEmpty, "anvil/crashes"});
        plan.steps.push_back({Step::Kind::RemoveIfEmpty, "anvil"});
        plan.notes.push_back(i18n::tr("Your plugins, anvil/anvil.toml, anvil/data, anvil/crashes and the logs stay."));
    }
    if (installation.manager_copy) plan.steps.push_back({Step::Kind::RemoveManager, kManagerExe});
    if (restore_backup && installation.backup) {
        const LegacyBackup& backup = *installation.backup;
        const auto taken = [&](const std::string& file) {
            return std::find(backup.taken.begin(), backup.taken.end(), file) != backup.taken.end();
        };
        std::string dll;  // last: the loader once its files are back
        std::set<std::string> folders;
        for (const std::string& file : backup.files) {
            for (size_t slash = file.find('/'); slash != std::string::npos; slash = file.find('/', slash + 1)) {
                folders.insert(file.substr(0, slash));
            }
            if (taken(file)) continue;
            if (same_name(file, "version.dll")) {
                dll = file;
                continue;
            }
            plan.steps.push_back({Step::Kind::Move, backup.folder + "/" + file, file});
        }
        if (!dll.empty()) plan.steps.push_back({Step::Kind::Move, backup.folder + "/" + dll, "version.dll"});
        // What the moves empty goes, deepest first; a folder that still holds something stays.
        std::vector<std::string> emptied(folders.begin(), folders.end());
        std::sort(emptied.begin(), emptied.end(), [](const std::string& a, const std::string& b) {
            return std::count(a.begin(), a.end(), '/') > std::count(b.begin(), b.end(), '/') ||
                   (std::count(a.begin(), a.end(), '/') == std::count(b.begin(), b.end(), '/') && a < b);
        });
        for (const std::string& folder : emptied) plan.steps.push_back({Step::Kind::RemoveIfEmpty, backup.folder + "/" + folder});
        plan.steps.push_back({Step::Kind::RemoveIfEmpty, backup.folder});
        plan.steps.push_back({Step::Kind::RemoveIfEmpty, kLegacyBackup});
        plan.notes.push_back(i18n::trf("{0} goes back from {1}.", {backup.name, backup.folder}));
        if (backup.name != "version.dll") {
            plan.notes.push_back(i18n::trf("Saves made with Mei may show garbled text in {0}.", {backup.name}));
        }
        for (const std::string& file : backup.taken) {
            plan.notes.push_back(i18n::trf("{0} stays in the backup: the game folder has another file there.", {file}));
        }
    }
    return plan;
}

ApplyResult apply_plan(const Package& package, const Plan& plan, const fs::path& game_dir, const fs::path& manager_exe) {
    ApplyResult result;
    const auto fail = [&](const Step& step, std::string why, DWORD code) {
        result.ok = false;
        result.error = step.path + ": " + (code != 0 ? i18n::trf("{0} (error {1})", {why, std::to_string(code)}) : why);
        result.access_denied = result.access_denied || code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION;
        return result;
    };
    if (!plan.refused.empty()) {
        result.ok = false;
        result.error = plan.refused;
        return result;
    }
    for (const Step& step : plan.steps) {
        if (!unsafe_path_reason(step.path).empty() || (step.kind == Step::Kind::Move && !unsafe_path_reason(step.to).empty())) {
            return fail(step, i18n::tr("not a path below the game folder"), 0);
        }
        const fs::path target = below(game_dir, step.path);
        std::error_code ec;
        switch (step.kind) {
        case Step::Kind::Write: {
            const ZipEntry* entry = package.zip.find(step.path);
            if (entry == nullptr) return fail(step, i18n::tr("not in the package"), 0);
            std::string error;
            const auto bytes = package.zip.read(*entry, error);
            if (!bytes) return fail(step, error, 0);
            bool denied = false;
            if (!write_file_atomic(target, *bytes, error, &denied)) {
                result.access_denied = denied;
                return fail(step, error, 0);
            }
            break;
        }
        case Step::Kind::CopyManager: {
            if (!CopyFileW(manager_exe.c_str(), target.c_str(), FALSE)) {
                return fail(step, i18n::tr("cannot copy the Manager"), GetLastError());
            }
            break;
        }
        case Step::Kind::Remove:
        case Step::Kind::RemoveTree:
            if (const DWORD code = remove_tree(target); code != 0) return fail(step, i18n::tr("cannot remove"), code);
            break;
        case Step::Kind::RemoveIfEmpty:
            if (is_dir(target) && fs::is_empty(target, ec)) {
                if (!RemoveDirectoryW(target.c_str())) return fail(step, i18n::tr("cannot remove"), GetLastError());
            }
            break;
        case Step::Kind::Move: {
            // Never over something: a file that appeared there stops the plan.
            const fs::path to = below(game_dir, step.to);
            if (present(to)) return fail(step, i18n::trf("{0} is already there", {step.to}), 0);
            fs::create_directories(to.parent_path(), ec);
            if (!MoveFileExW(target.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) {
                return fail(step, i18n::trf("cannot move it to {0}", {step.to}), GetLastError());
            }
            break;
        }
        case Step::Kind::RemoveManager:
            if (!manager_exe.empty() && same_file(manager_exe, target)) {
                result.manager_left = true;
            } else if (!DeleteFileW(target.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                return fail(step, i18n::tr("cannot remove"), GetLastError());
            }
            break;
        }
        ++result.done;
    }
    return result;
}

}  // namespace anvil::manager
