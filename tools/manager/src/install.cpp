#include "install.hpp"

#include "core/game_build.hpp"
#include "core/log.hpp"
#include "files.hpp"
#include "game.hpp"
#include "i18n/translate.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
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

}  // namespace

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
                  const fs::path& manager_exe, bool replace_foreign_dll) {
    Plan plan;
    if (installation.dll == Installation::Dll::Foreign && !replace_foreign_dll) {
        plan.refused = i18n::tr("version.dll in the game folder is not Anvil's: another mod may use it. Remove it "
                                "first, or replace it on purpose.");
        return plan;
    }
    if (installation.dll == Installation::Dll::Foreign) {
        plan.notes.push_back(i18n::tr("version.dll is not Anvil's and will be replaced."));
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

Plan plan_uninstall(const Package& package, const Installation& installation, bool everything) {
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
        if (!unsafe_path_reason(step.path).empty()) return fail(step, i18n::tr("not a path below the game folder"), 0);
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
