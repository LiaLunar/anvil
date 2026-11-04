#include "plugin_zip.hpp"

#include "core/discovery.hpp"
#include "core/log.hpp"
#include "core/manifest.hpp"
#include "files.hpp"
#include "i18n/translate.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

fs::path below(const fs::path& dir, std::string_view relative) { return dir / fs::path(widen(relative)).make_preferred(); }

// A folder below `parent` that did not exist: <name>-<pid>-<n>. Empty on a
// failure, with Windows' error in `code`.
fs::path fresh_folder(const fs::path& parent, const std::string& name, DWORD& code) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        code = static_cast<DWORD>(ec.value());
        return {};
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        const fs::path dir = parent / widen(name + "-" + std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(counter.fetch_add(1)));
        if (CreateDirectoryW(dir.c_str(), nullptr)) return dir;
        code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) return {};
    }
    return {};
}

// Unpacks the files below `root` into `dir`.
bool unpack(const ZipArchive& zip, const std::vector<const ZipEntry*>& files, const std::string& root,
            const fs::path& dir, std::string& error, bool* access_denied = nullptr) {
    for (const ZipEntry* entry : files) {
        const auto bytes = zip.read(*entry, error);
        if (!bytes) return false;
        if (!write_file_atomic(below(dir, entry->path.substr(root.size())), *bytes, error, access_denied)) return false;
    }
    return true;
}

}  // namespace

PluginZipCheck check_plugin_zip(const ZipArchive& zip, const fs::path& game_dir, const fs::path& temp_dir,
                                const std::vector<std::string>& standard_plugins) {
    PluginZipCheck out;
    // The shallowest plugin.toml names the plugin's folder in the zip.
    const ZipEntry* manifest = nullptr;
    size_t depth = SIZE_MAX;
    bool tie = false;
    for (const ZipEntry& entry : zip.entries()) {
        if (entry.path != "plugin.toml" && !entry.path.ends_with("/plugin.toml")) continue;
        const size_t d = std::count(entry.path.begin(), entry.path.end(), '/');
        if (d < depth) {
            manifest = &entry;
            depth = d;
            tie = false;
        } else if (d == depth) {
            tie = true;
        }
    }
    if (manifest == nullptr) {
        out.error = i18n::tr("the zip holds no plugin.toml: it is not an Anvil plugin");
        return out;
    }
    if (tie) {
        out.error = i18n::tr("the zip holds more than one plugin; install them one by one");
        return out;
    }
    out.root = manifest->path.substr(0, manifest->path.size() - std::string_view("plugin.toml").size());
    for (const ZipEntry& entry : zip.entries()) {
        if (entry.path.starts_with(out.root)) {
            out.files.push_back(&entry);
        } else if (!entry.path.starts_with("__MACOSX/")) {
            out.ignored.push_back(entry.path);  // e.g. the release's SHA256SUMS.txt
        }
    }

    DWORD code = 0;
    const fs::path stage = fresh_folder(temp_dir, "anvil-manager-check", code);
    if (stage.empty()) {
        out.error = i18n::trf("cannot create a temporary folder in {0}", {narrow(temp_dir.wstring())});
        return out;
    }
    if (unpack(zip, out.files, out.root, stage, out.error)) {
        cli::CheckOptions options;
        options.plugin_dir = stage;
        options.exe = game_dir / "eu4.exe";
        options.write_lock = false;
        out.report = cli::check_plugin(options);
        // Name the files as they are in the zip, not in the temporary folder.
        const auto shown = stage.lexically_normal().generic_u8string();
        const std::string prefix = std::string(shown.begin(), shown.end()) + "/";
        const auto in_zip = [&](std::string& text) {
            for (size_t at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at)) {
                text.replace(at, prefix.size(), out.root);
                at += out.root.size();
            }
        };
        for (cli::Problem& problem : out.report.problems) {
            in_zip(problem.file);
            in_zip(problem.message);
        }
        in_zip(out.report.cannot_check);
        in_zip(out.report.skip_reason);
        const std::function<void(i18n::Message&)> in_zip_message = [&](i18n::Message& m) {
            in_zip(m.text);
            for (i18n::Message& a : m.args) in_zip_message(a);
        };
        in_zip_message(out.report.skip_why);
    }
    const i18n::Message entry_missing = out.report.manifest ? missing_entry_reason(*out.report.manifest) : i18n::Message();
    remove_tree(stage);
    if (!out.error.empty()) return out;
    if (!out.report.manifest) {
        // plugin.toml does not parse: say why in the loader's words.
        out.error = out.report.problems.empty() ? out.report.cannot_check : out.report.problems.front().message;
        if (out.error.empty()) out.error = i18n::tr("plugin.toml cannot be read");
        return out;
    }
    const Manifest& m = *out.report.manifest;
    out.id = m.id;
    out.version = m.version.to_string();
    if (std::find(standard_plugins.begin(), standard_plugins.end(), m.id) != standard_plugins.end()) {
        out.error = i18n::trf("{0} comes with Anvil; repair Anvil instead of installing it from a zip", {m.id});
        return out;
    }
    if (!entry_missing.empty() || out.report.exit_code == cli::kExitCannotCheck) {
        out.error = !entry_missing.empty() ? i18n::text(entry_missing) : out.report.cannot_check;
        return out;
    }
    out.would_skip = !out.report.skip_reason.empty();

    // The same id installed now: in its own folder, or another one.
    const fs::path plugins = game_dir / "anvil" / "plugins";
    for (const fs::path& folder : plugin_folders(plugins)) {
        const ManifestResult installed = load_manifest(folder);
        const std::string name = narrow(folder.filename().wstring());
        if (installed.manifest && installed.manifest->id != m.id && name == m.id) {
            // Another plugin in the folder this one would take: never replaced.
            out.error = i18n::trf("{0} holds the plugin {1}; move or remove it first",
                                  {"anvil/plugins/" + name, installed.manifest->id});
            return out;
        }
        if ((installed.manifest && installed.manifest->id == m.id) || (!installed.manifest && name == m.id)) {
            if (!out.installed_folder.empty() && name != m.id) continue;  // prefer the folder named like the id
            out.installed_folder = name;
            out.installed_version = installed.manifest ? installed.manifest->version.to_string() : std::string();
        }
    }
    return out;
}

PluginInstallResult install_plugin_zip(const ZipArchive& zip, const PluginZipCheck& check, const fs::path& game_dir) {
    PluginInstallResult result;
    if (!check.error.empty() || check.id.empty()) {
        result.error = check.error.empty() ? std::string(i18n::tr("the zip was not checked")) : check.error;
        return result;
    }
    const fs::path work = game_dir / "anvil" / "data" / "anvil" / "manager";
    DWORD incoming_code = 0;
    const fs::path incoming = fresh_folder(work, check.id + ".incoming", incoming_code);
    if (incoming.empty()) {
        result.error = i18n::trf("cannot create a folder in {0}", {narrow(work.wstring())});
        result.access_denied = incoming_code == ERROR_ACCESS_DENIED;
        return result;
    }
    if (!unpack(zip, check.files, check.root, incoming, result.error, &result.access_denied)) {
        remove_tree(incoming);
        return result;
    }
    const fs::path target = game_dir / "anvil" / "plugins" / widen(check.id);
    fs::path previous;
    const auto fail = [&](const std::string& what, DWORD code) {
        result.error = what + " (error " + std::to_string(code) + ")";
        result.access_denied = code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION;
        return result;
    };
    // Move the installed folder aside: under its own name when the id's folder holds it, and
    // the id's folder too when another plugin's files are there (they would mix otherwise).
    std::vector<fs::path> aside;
    for (const std::string& name : {check.installed_folder, check.id}) {
        if (name.empty()) continue;
        const fs::path folder = game_dir / "anvil" / "plugins" / widen(name);
        if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (std::find(aside.begin(), aside.end(), folder) != aside.end()) continue;
        DWORD code = 0;
        previous = fresh_folder(work, name + ".previous", code);
        if (previous.empty() || !RemoveDirectoryW(previous.c_str()) ||
            !MoveFileExW(folder.c_str(), previous.c_str(), 0)) {
            if (code == 0) code = GetLastError();
            remove_tree(incoming);
            return fail(i18n::trf("cannot move {0} aside; is a file in it open?", {narrow(folder.wstring())}), code);
        }
        aside.push_back(folder);
        aside.push_back(previous);
    }
    if (!MoveFileExW(incoming.c_str(), target.c_str(), 0)) {
        const DWORD code = GetLastError();
        for (size_t i = 0; i + 1 < aside.size(); i += 2) MoveFileExW(aside[i + 1].c_str(), aside[i].c_str(), 0);
        remove_tree(incoming);
        return fail(i18n::trf("cannot move the plugin to {0}", {narrow(target.wstring())}), code);
    }
    for (size_t i = 0; i + 1 < aside.size(); i += 2) remove_tree(aside[i + 1]);
    result.ok = true;
    result.folder = target;
    return result;
}

bool remove_plugin_folder(const fs::path& game_dir, const std::string& folder,
                          const std::vector<std::string>& standard_plugins, std::string& error) {
    if (folder.empty() || !unsafe_path_reason(folder).empty() || folder.find('/') != std::string::npos) {
        error = "'" + folder + "' is not a plugin folder";
        return false;
    }
    if (std::find(standard_plugins.begin(), standard_plugins.end(), folder) != standard_plugins.end()) {
        error = folder + " comes with Anvil; uninstall Anvil to remove it";
        return false;
    }
    const fs::path path = game_dir / "anvil" / "plugins" / widen(folder);
    if (const DWORD code = remove_tree(path); code != 0) {
        error = i18n::trf("cannot remove {0} (error {1})", {narrow(path.wstring()), std::to_string(code)});
        return false;
    }
    return true;
}

}  // namespace anvil::manager
