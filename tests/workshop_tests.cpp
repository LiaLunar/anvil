// Unit tests for plugins in mods (design §14, #60): the playset (dlc_load.json
// and .mod descriptors), the folder hash, trust.toml, the decisions
// (approved, update pending, pending, conflict, shadowed, invalid), approved
// copies, approval and revocation. Made-up playsets in the temporary folder
// and the descriptors in tests/fixtures/playset; nothing here needs, or
// touches, a real game or EU4's user folder.
#include "core/files.hpp"
#include "core/log.hpp"
#include "core/mod_plugins.hpp"
#include "core/playset.hpp"
#include "core/plugin_hash.hpp"
#include "core/trust.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace anvil;
namespace fs = std::filesystem;

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string generic(const fs::path& path) {
    const auto u8 = path.lexically_normal().generic_u8string();
    return std::string(u8.begin(), u8.end());
}

bool make_junction(const fs::path& link, const fs::path& target) {
    const std::wstring command = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul";
    return _wsystem(command.c_str()) == 0;
}

// A game folder with anvil/, a user folder with dlc_load.json and mods: local
// ones in <user>/mod/<name>, Workshop ones in <root>/workshop/content/236850/<id>.
struct Playground {
    fs::path root, anvil, user, workshop;
    std::vector<std::string> enabled;

    explicit Playground(const std::string& name) {
        root = fs::temp_directory_path() / ("anvil_workshop_tests_" + name + "_" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(root);
        anvil = root / "eu4" / "anvil";
        user = root / "user";
        workshop = root / "workshop" / "content" / "236850";
        fs::create_directories(anvil / "plugins");
        fs::create_directories(user / "mod");
        save();
    }
    ~Playground() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void save() const {
        std::string json = "{\"enabled_mods\":[";
        for (size_t i = 0; i < enabled.size(); ++i) json += (i ? ",\"" : "\"") + enabled[i] + "\"";
        write(user / "dlc_load.json", json + "],\"disabled_dlcs\":[]}");
    }

    // A mod with its descriptor; returns its folder. `workshop_id` puts it in the Workshop's folder.
    fs::path mod(const std::string& name, const std::string& workshop_id = "", bool enable = true) {
        const std::string descriptor = workshop_id.empty() ? "mod/" + name + ".mod" : "mod/ugc_" + workshop_id + ".mod";
        const fs::path folder = workshop_id.empty() ? user / "mod" / name : workshop / workshop_id;
        const std::string path = workshop_id.empty() ? "mod/" + name : generic(folder);
        write(user / descriptor, "name=\"" + name + "\"\ntags={\n\t\"Utilities\"\n}\npath=\"" + path + "\"\n" +
                                     (workshop_id.empty() ? "" : "remote_file_id=\"" + workshop_id + "\"\n"));
        write(folder / "descriptor.mod", "name=\"" + name + "\"\n");
        if (enable) {
            enabled.push_back(descriptor);
            save();
        }
        return folder;
    }

    // <mod>/anvil/<id> with plugin.toml and its entry DLL.
    static fs::path plugin(const fs::path& mod, const std::string& id, const std::string& version = "1.0.0",
                           const std::string& dll = "MZ plugin") {
        const fs::path folder = mod / "anvil" / id;
        write(folder / "plugin.toml", plugin_toml(id, "", version));
        write(folder / (id + ".dll"), dll);
        return folder;
    }

    void local_plugin(const std::string& id) const {
        write(anvil / "plugins" / id / "plugin.toml", plugin_toml(id));
        write(anvil / "plugins" / id / (id + ".dll"), "MZ local");
    }

    ModDiscovery discover(const std::set<std::string>& local_ids = {}) const {
        return discover_mod_plugins(anvil, user, local_ids);
    }
};

const ModPlugin* find(const ModDiscovery& d, const std::string& id, const std::string& mod) {
    for (const ModPlugin& p : d.plugins) {
        if (p.id == id && p.mod.name == mod) return &p;
    }
    return nullptr;
}

bool approve(const Playground& g, const std::string& id, const std::string& mod, std::string keep = {}) {
    const ModDiscovery d = g.discover();
    const ModPlugin* p = find(d, id, mod);
    if (p == nullptr) return false;
    std::string error;
    const bool ok = approve_mod_plugin(g.anvil, *p, p->hash, keep, error);
    if (!ok) std::printf("  approve: %s\n", error.c_str());
    return ok;
}

}  // namespace

int main() {
    const fs::path fixtures = fs::path(ANVIL_SOURCE_DIR) / "tests" / "fixtures" / "playset";

    run("dlc_load.json: enabled_mods in order; other keys skipped; broken files are errors", [] {
        std::string error;
        const auto mods = parse_dlc_load(R"({"enabled_mods":["mod/ugc_1.mod","mod/a b.mod"],"disabled_dlcs":["x"]})", error);
        CHECK(mods && mods->size() == 2 && (*mods)[0] == "mod/ugc_1.mod" && (*mods)[1] == "mod/a b.mod");
        const auto spaced = parse_dlc_load("\xEF\xBB\xBF { \"disabled_dlcs\" : [ ] , \"enabled_mods\" : [ ] }\n", error);
        CHECK(spaced && spaced->empty());
        const auto none = parse_dlc_load("{}", error);
        CHECK(none && none->empty());
        const auto escaped =
            parse_dlc_load("{\"enabled_mods\":[\"mod/\\" "u0141\\" "u00f3d\\" "u017a.mod\",\"mod\\/x.mod\"]}", error);
        CHECK(escaped && (*escaped)[0] == "mod/\xC5\x81\xC3\xB3" "d\xC5\xBA.mod" && (*escaped)[1] == "mod/x.mod");
        CHECK(!parse_dlc_load("[]", error) && error == "not a JSON object");
        CHECK(!parse_dlc_load(R"({"enabled_mods":"mod/x.mod"})", error) && error == "enabled_mods is not an array");
        CHECK(!parse_dlc_load(R"({"enabled_mods":[1]})", error) && contains(error, "other than strings"));
        CHECK(!parse_dlc_load(R"({"enabled_mods":[]} trailing)", error));
        CHECK(!parse_dlc_load("{\"enabled_mods\":[\"\\" "ud800\"]}", error));  // a lone surrogate
    });

    run("descriptors: name, path and archive; blocks and comments are skipped", [&] {
        const PlaysetMod workshop = parse_mod_descriptor(read(fixtures / "mod" / "ugc_2016264376.mod"));
        CHECK(workshop.name == "A Workshop Mod" && workshop.error.empty());
        CHECK(generic(workshop.folder) == "C:/Program Files (x86)/Steam/steamapps/workshop/content/236850/2016264376");
        const PlaysetMod local = parse_mod_descriptor(read(fixtures / "mod" / "local-mod.mod"));
        CHECK(local.name == "A Local Mod" && generic(local.folder) == "mod/local-mod");
        const PlaysetMod zipped = parse_mod_descriptor(read(fixtures / "mod" / "ugc_1385440355.mod"));
        CHECK(zipped.folder.empty() && zipped.error.starts_with("a zipped mod"));
        CHECK(parse_mod_descriptor("name = \"x\"").error == "the descriptor has no path");
        // Keys inside blocks and nested blocks do not count; bare words are values too.
        const PlaysetMod nested = parse_mod_descriptor("tags = { path = \"no\" inner = { name = \"no\" } }\nname = yes\npath = \"C:/m\"");
        CHECK(nested.name == "yes" && generic(nested.folder) == "C:/m");
    });

    run("the playset: descriptors relative to the user folder; Workshop ids from the Workshop folder only", [] {
        Playground g("playset");
        g.mod("local-a");
        g.mod("item", "123456");
        g.mod("disabled", "", false);
        write(g.user / "mod" / "uploaded.mod", "name=\"Uploaded\"\npath=\"mod/uploaded\"\nremote_file_id=\"999\"\n");
        g.enabled.push_back("mod/uploaded.mod");
        g.enabled.push_back("mod/missing.mod");
        g.save();
        const Playset p = read_playset(g.user);
        CHECK(p.found && p.error.empty() && p.mods.size() == 4);
        CHECK(p.mods[0].descriptor == "mod/local-a.mod" && p.mods[0].folder == (g.user / "mod" / "local-a").lexically_normal());
        CHECK(p.mods[0].workshop_id.empty());
        CHECK(p.mods[1].name == "item" && p.mods[1].workshop_id == "123456");
        CHECK(p.mods[2].name == "Uploaded" && p.mods[2].workshop_id.empty());  // remote_file_id alone is no Workshop item
        CHECK(p.mods[3].name == "missing.mod" && contains(p.mods[3].error, "cannot read the descriptor"));

        write(g.user / "dlc_load.json", "{not json");
        const Playset broken = read_playset(g.user);
        CHECK(broken.found && contains(broken.error, "dlc_load.json: ") && broken.mods.empty());
        fs::remove(g.user / "dlc_load.json");
        const Playset none = read_playset(g.user);
        CHECK(!none.found && none.error.empty() && none.mods.empty());
    });

    run("the user folder: userdir.txt names it, otherwise Documents", [] {
        Playground g("userdir");
        const fs::path game = g.root / "eu4";
        const fs::path documents = eu4_user_dir(game);
        CHECK(documents.filename() == L"Europa Universalis IV" && documents.parent_path().filename() == L"Paradox Interactive");
        write(game / "userdir.txt", "");  // the game ships an empty one
        CHECK(eu4_user_dir(game) == documents);
        write(game / "userdir.txt", "  " + generic(g.user) + "\r\n");
        CHECK(eu4_user_dir(game) == fs::path(widen(generic(g.user))));
        // A GOG install has its own folder beside Steam's, and userdir.txt still wins.
        write(game / "goggame-2057001589.info", "{}");
        CHECK(eu4_user_dir(game) == fs::path(widen(generic(g.user))));
        write(game / "userdir.txt", "");
        CHECK(eu4_user_dir(game) == documents.parent_path() / L"Europa Universalis IV GOG");
    });

    run("the folder hash: sorted path, size and file hash lines, the same on every machine", [] {
        Playground g("hash");
        const fs::path folder = g.root / "h";
        write(folder / "plugin.toml", "a");
        write(folder / "Sub" / "B.dll", "bc");
        fs::create_directories(folder / "empty");
        const FolderHash h = hash_plugin_folder(folder);
        // Computed independently: SHA-256 of "plugin.toml\0" "1\0" sha("a") "\n" "sub/b.dll\0" "2\0" sha("bc") "\n".
        CHECK(h.error.empty() && h.sha256 == "b44c904cb3c2ae17fbf90e0e0f0a04d837ce3a4a9ae008bd74f50e6a2700a2c2");
        CHECK(h.files.size() == 2 && h.files[1].relative == "sub/b.dll" && h.files[1].size == 2 && h.bytes == 3);
        CHECK(h.files[0].sha256 == "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
        CHECK(short_hash(h.sha256) == "b44c904c…");

        write(folder / "sub" / "b.dll", "bd");  // same name, other content (the folder is case-insensitive)
        CHECK(hash_plugin_folder(folder).sha256 != h.sha256);
        write(folder / "sub" / "b.dll", "bc");
        CHECK(hash_plugin_folder(folder).sha256 == h.sha256);
        fs::rename(folder / "sub" / "b.dll", folder / "sub" / "c.dll");
        CHECK(hash_plugin_folder(folder).sha256 != h.sha256);
        fs::rename(folder / "sub" / "c.dll", folder / "sub" / "b.dll");
        write(folder / "extra.txt", "");
        CHECK(hash_plugin_folder(folder).sha256 != h.sha256);
        fs::remove(folder / "extra.txt");

        fs::create_directories(g.root / "outside");
        CHECK(make_junction(folder / "link", g.root / "outside"));
        const FolderHash linked = hash_plugin_folder(folder);
        CHECK(linked.sha256.empty() && contains(linked.error, "link is a link"));
        RemoveDirectoryW((folder / "link").c_str());
        CHECK(contains(hash_plugin_folder(g.root / "none").error, "is not a folder"));
    });

    run("the folder hash with locks: the files cannot be written or deleted until released", [] {
        Playground g("locks");
        const fs::path folder = g.root / "h";
        write(folder / "x.dll", "MZ");
        FileLocks locks;
        const FolderHash h = hash_plugin_folder(folder, &locks);
        CHECK(h.error.empty());
        const HANDLE w = CreateFileW((folder / "x.dll").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(w == INVALID_HANDLE_VALUE);
        if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
        CHECK(!DeleteFileW((folder / "x.dll").c_str()));
        CHECK(!MoveFileExW(folder.c_str(), (g.root / "moved").c_str(), 0));
        // Reading (as LoadLibrary does) still works.
        const HANDLE r = CreateFileW((folder / "x.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(r != INVALID_HANDLE_VALUE);
        if (r != INVALID_HANDLE_VALUE) CloseHandle(r);
        locks.release();
        CHECK(DeleteFileW((folder / "x.dll").c_str()));
    });

    run("trust.toml: strict schema, written by id and read back", [] {
        const std::string hash(64, 'a');
        const std::string entry = "[[plugin]]\nid = \"hello\"\nversion = \"1.0.0\"\nmod = \"mod/ugc_1.mod\"\nmod_name = \"Hello\"\n"
                                  "workshop_id = \"1\"\nhash = \"" + hash + "\"\napproved = 2026-10-20T18:05:11Z\n";
        const TrustFile ok = parse_trust_text("# comment\n" + entry);
        CHECK(ok.error.empty() && ok.entries.size() == 1 && ok.find("hello") != nullptr);
        CHECK(ok.entries[0].approved == "2026-10-20T18:05:11Z" && ok.entries[0].workshop_id == "1" && ok.entries[0].line == 2);
        CHECK(parse_trust_text("").error.empty());
        CHECK(parse_trust_text("other = 1\n").error == "unknown key 'other'");
        CHECK(parse_trust_text(entry + "extra = 1\n").error == "unknown key 'extra'");
        CHECK(parse_trust_text(entry + entry).error == "plugin 'hello' is approved twice");
        CHECK(contains(parse_trust_text("[[plugin]]\nid = \"x\"\n").error, "missing key"));
        std::string upper = entry;
        upper.replace(upper.find(hash), 64, std::string(64, 'A'));
        CHECK(contains(parse_trust_text(upper).error, "64 lower-case hex digits"));
        std::string local_time = entry;
        local_time.replace(local_time.find("18:05:11Z"), 9, "18:05:11");
        CHECK(contains(parse_trust_text(local_time).error, "UTC"));
        CHECK(!parse_trust_text("[[plugin]\n").error.empty());

        TrustEntry b{"b-two", "2.0.0", "mod/b.mod", "B \"quoted\"", "", std::string(64, 'b'), "2026-10-20T00:00:00Z"};
        TrustEntry a{"a-one", "1.0.0", "mod/ugc_5.mod", "A", "5", std::string(64, 'c'), "2026-10-19T23:59:59Z"};
        const std::string text = trust_text({b, a});
        CHECK(text.starts_with("# Plugins from mods") && text.find("a-one") < text.find("b-two"));
        const TrustFile back = parse_trust_text(text);
        CHECK(back.error.empty() && back.entries.size() == 2 && back.entries[1].mod_name == "B \"quoted\"");
        CHECK(utc_now().size() == 20 && utc_now().back() == 'Z');

        Playground g("trust");
        const fs::path file = g.anvil / "trust.toml";
        std::string error;
        CHECK(write_trust(file, {a}, error) && read_trust(file).entries.size() == 1);
        write(file, "broken = [");
        CHECK(!write_trust(file, {a}, error) && contains(error, "does not parse") && read(file) == "broken = [");
        CHECK(!read_trust(file).error.empty() && read_trust(file).found);
        CHECK(!read_trust(g.anvil / "none.toml").found);
    });

    run("decisions: pending, approved from the approved copy, an update keeps the approved copy", [] {
        Playground g("decide");
        const fs::path mod = g.mod("Hello Mod", "111");
        Playground::plugin(mod, "hello", "1.0.0", "MZ v1");

        ModDiscovery d = g.discover();
        CHECK(d.warnings.empty() && d.plugins.size() == 1);
        const ModPlugin* p = find(d, "hello", "Hello Mod");
        CHECK(p != nullptr && p->state == ModPluginState::Pending && !p->loads() && contains(p->reason, "waiting for approval"));
        CHECK(p->mod.workshop_id == "111" && p->manifest && p->hash.size() == 64 && p->problem.empty());

        CHECK(approve(g, "hello", "Hello Mod"));
        const TrustFile trust = read_trust(g.anvil / "trust.toml");
        CHECK(trust.entries.size() == 1 && trust.entries[0].mod == "mod/ugc_111.mod" && trust.entries[0].workshop_id == "111");
        CHECK(trust.entries[0].mod_name == "Hello Mod" && trust.entries[0].version == "1.0.0" && trust.entries[0].hash == p->hash);
        const fs::path copy = approved_copy(g.anvil, "hello", p->hash);
        CHECK(read(copy / "hello.dll") == "MZ v1" && hash_plugin_folder(copy).sha256 == p->hash);

        d = g.discover();
        p = find(d, "hello", "Hello Mod");
        CHECK(p->state == ModPluginState::Approved && p->loads() && p->load_folder == copy && !p->copy_missing && p->bound);
        CHECK(contains(p->reason, "approved on ") && state_name(p->state) == std::string("approved"));

        // Steam updates the mod: the new hash waits, the approved copy loads.
        const std::string approved_hash = p->hash;
        Playground::plugin(mod, "hello", "1.1.0", "MZ v2");
        d = g.discover();
        p = find(d, "hello", "Hello Mod");
        CHECK(p->state == ModPluginState::UpdatePending && p->loads() && p->load_folder == copy);
        CHECK(contains(p->reason, "version 1.1.0") && contains(p->reason, "the approved 1.0.0 loads until then"));

        // Approving the update while the old copy runs keeps that copy (in use) and adds the new one.
        CHECK(approve(g, "hello", "Hello Mod", approved_hash));
        d = g.discover();
        p = find(d, "hello", "Hello Mod");
        CHECK(p->state == ModPluginState::Approved && p->hash != approved_hash && fs::exists(copy));
        // The next start prunes the copies trust.toml no longer names.
        prune_approved_copies(g.anvil, d.trust);
        CHECK(!fs::exists(copy) && fs::exists(approved_copy(g.anvil, "hello", p->hash)));

        // An update whose approved copy is gone just waits.
        Playground::plugin(mod, "hello", "1.2.0", "MZ v3");
        remove_tree(approved_copy(g.anvil, "hello", p->hash));
        d = g.discover();
        p = find(d, "hello", "Hello Mod");
        CHECK(p->state == ModPluginState::Pending && contains(p->reason, "approved copy is missing"));
    });

    run("decisions: a missing copy of an approved hash is made; a damaged one is replaced on approval", [] {
        Playground g("copy");
        const fs::path mod = g.mod("local-mod");
        Playground::plugin(mod, "tool");
        CHECK(approve(g, "tool", "local-mod"));
        ModDiscovery d = g.discover();
        const ModPlugin* p = find(d, "tool", "local-mod");
        CHECK(p->mod.workshop_id.empty() && read_trust(g.anvil / "trust.toml").entries[0].workshop_id.empty());
        remove_tree(p->load_folder);
        d = g.discover();
        p = find(d, "tool", "local-mod");
        CHECK(p->state == ModPluginState::Approved && p->copy_missing);
        std::string error;
        CHECK(make_approved_copy(g.anvil, *p, error) && hash_plugin_folder(p->load_folder).sha256 == p->hash);

        write(p->load_folder / "tool.dll", "tampered");
        CHECK(hash_plugin_folder(p->load_folder).sha256 != p->hash);
        CHECK(approve(g, "tool", "local-mod"));
        CHECK(read(p->load_folder / "tool.dll") == "MZ plugin");

        // The mod's files changed after they were shown: no approval.
        Playground::plugin(mod, "tool", "1.0.1");
        CHECK(!approve_mod_plugin(g.anvil, *p, p->hash, "", error) && contains(error, "files changed"));
    });

    run("decisions: two mods with one id load neither, until the player approves one", [] {
        Playground g("conflict");
        const fs::path a = g.mod("Mod A", "1");
        const fs::path b = g.mod("Mod B");
        Playground::plugin(a, "shared", "1.0.0", "MZ a");
        Playground::plugin(b, "shared", "1.0.0", "MZ b");
        ModDiscovery d = g.discover();
        const ModPlugin* pa = find(d, "shared", "Mod A");
        const ModPlugin* pb = find(d, "shared", "Mod B");
        CHECK(pa->state == ModPluginState::Conflict && pb->state == ModPluginState::Conflict);
        CHECK(contains(pa->reason, "2 enabled mods have a plugin 'shared' ('Mod A', 'Mod B')") && contains(pa->reason, "none loads"));
        CHECK(pa->others == std::vector<std::string>{"Mod B"} && pb->others == std::vector<std::string>{"Mod A"});

        CHECK(approve(g, "shared", "Mod B"));
        d = g.discover();
        pa = find(d, "shared", "Mod A");
        pb = find(d, "shared", "Mod B");
        CHECK(pb->state == ModPluginState::Approved && pa->state == ModPluginState::Conflict);
        CHECK(contains(pa->reason, "mod 'Mod B' has this plugin too and is approved for it"));

        // Approving the other one moves the binding.
        CHECK(approve(g, "shared", "Mod A"));
        d = g.discover();
        CHECK(find(d, "shared", "Mod A")->state == ModPluginState::Approved);
        CHECK(find(d, "shared", "Mod B")->state == ModPluginState::Conflict);
        CHECK(read_trust(g.anvil / "trust.toml").entries.size() == 1);

        // Mod A disabled: its plugin is gone, and B's id is bound to another mod.
        g.enabled = {"mod/Mod B.mod"};
        g.save();
        d = g.discover();
        CHECK(d.plugins.size() == 1 && d.plugins[0].state == ModPluginState::Pending);
        CHECK(contains(d.plugins[0].reason, "approved for another mod ('Mod A')"));
    });

    run("decisions: a local plugin shadows every mod with its id; bad folders are invalid", [] {
        Playground g("shadow");
        const fs::path a = g.mod("Mod A", "7");
        Playground::plugin(a, "anvil-overlay");
        Playground::plugin(a, "Bad_Name");
        Playground::plugin(a, "renamed");
        write(a / "anvil" / "renamed" / "plugin.toml", plugin_toml("other-id"));
        write(a / "anvil" / "broken" / "plugin.toml", "id = ");
        Playground::plugin(a, "no-entry");
        fs::remove(a / "anvil" / "no-entry" / "no-entry.dll");
        write(a / "anvil" / "README.txt", "not a plugin");
        const ModDiscovery d = g.discover({"anvil-overlay"});
        CHECK(d.plugins.size() == 5);
        CHECK(find(d, "anvil-overlay", "Mod A")->state == ModPluginState::Shadowed);
        CHECK(contains(find(d, "anvil-overlay", "Mod A")->reason, "local plugin"));
        CHECK(find(d, "Bad_Name", "Mod A")->state == ModPluginState::Invalid);
        CHECK(contains(find(d, "renamed", "Mod A")->reason, "plugin.toml has id 'other-id'"));
        CHECK(find(d, "broken", "Mod A")->state == ModPluginState::Invalid && contains(find(d, "broken", "Mod A")->reason, "plugin.toml: "));
        CHECK(contains(find(d, "no-entry", "Mod A")->reason, "entry no-entry.dll not found"));
        std::string error;
        CHECK(!approve_mod_plugin(g.anvil, *find(d, "broken", "Mod A"), find(d, "broken", "Mod A")->hash, "", error));
        CHECK(contains(error, "cannot be approved"));
    });

    run("decisions: a broken update keeps the approved copy; a broken trust.toml approves nothing", [] {
        Playground g("broken-update");
        const fs::path mod = g.mod("M");
        Playground::plugin(mod, "p");
        CHECK(approve(g, "p", "M"));
        write(mod / "anvil" / "p" / "plugin.toml", "id = ");
        ModDiscovery d = g.discover();
        CHECK(d.plugins[0].state == ModPluginState::UpdatePending && contains(d.plugins[0].reason, "cannot be approved"));

        const std::string trust = read(g.anvil / "trust.toml");
        write(g.anvil / "trust.toml", trust + "oops");
        d = g.discover();
        CHECK(d.warnings.size() == 1 && contains(d.warnings[0], "trust.toml does not parse"));
        CHECK(d.plugins[0].state == ModPluginState::Invalid);
        const fs::path copies = g.anvil / "data" / "anvil" / "approved" / "p";
        prune_approved_copies(g.anvil, d.trust);  // nothing goes while the file is broken
        CHECK(!fs::is_empty(copies));
        std::string error;
        CHECK(!revoke_mod_plugin(g.anvil, "p", "", error) && contains(error, "does not parse"));
        write(g.anvil / "trust.toml", trust);
    });

    run("revoking: the approval and its copies go, except the copy in use", [] {
        Playground g("revoke");
        const fs::path mod = g.mod("M");
        Playground::plugin(mod, "p");
        Playground::plugin(mod, "q");
        CHECK(approve(g, "p", "M") && approve(g, "q", "M"));
        ModDiscovery d = g.discover();
        const std::string hash_p = find(d, "p", "M")->hash;
        std::string error;
        CHECK(revoke_mod_plugin(g.anvil, "p", hash_p, error));
        CHECK(read_trust(g.anvil / "trust.toml").find("p") == nullptr && read_trust(g.anvil / "trust.toml").find("q") != nullptr);
        CHECK(fs::exists(approved_copy(g.anvil, "p", hash_p)));
        d = g.discover();
        CHECK(find(d, "p", "M")->state == ModPluginState::Pending && find(d, "q", "M")->state == ModPluginState::Approved);
        prune_approved_copies(g.anvil, d.trust);
        CHECK(!fs::exists(g.anvil / "data" / "anvil" / "approved" / "p"));
        CHECK(fs::exists(approved_copy(g.anvil, "q", find(d, "q", "M")->hash)));
        CHECK(revoke_mod_plugin(g.anvil, "never-approved", "", error));
        CHECK(revoke_mod_plugin(g.anvil, "q", "", error) && !fs::exists(g.anvil / "data" / "anvil" / "approved" / "q"));
    });

    run("mods that cannot be read are warnings; zipped mods are not", [] {
        Playground g("warnings");
        g.enabled = {"mod/missing.mod", "mod/zipped.mod"};
        write(g.user / "mod" / "zipped.mod", "name=\"Z\"\narchive=\"z.zip\"\n");
        g.save();
        ModDiscovery d = g.discover();
        CHECK(d.warnings.size() == 1 && contains(d.warnings[0], "mod mod/missing.mod: cannot read the descriptor"));
        write(g.user / "dlc_load.json", "{");
        d = g.discover();
        CHECK(d.warnings.size() == 1 && contains(d.warnings[0], "no plugins from mods are loaded"));
    });

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
