#include "platform.hpp"

#include "log.hpp"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

namespace anvil {
namespace {

std::string text_or_empty(const char* text) { return text ? std::string(text) : std::string(); }

// One block of a version resource: wLength, wValueLength, wType, a key, then
// the value and the children, each aligned to 4 bytes from the start.
struct Block {
    size_t start = 0;
    size_t end = 0;  // start + wLength
    uint16_t value_length = 0;
    uint16_t type = 0;  // 1: text, value_length in characters
    std::wstring key;
    size_t value = 0;     // offset of the value
    size_t children = 0;  // offset of the first child
};

size_t align4(size_t offset) { return (offset + 3) & ~size_t{3}; }

uint16_t read16(const uint8_t* data, size_t offset) {
    uint16_t v;
    std::memcpy(&v, data + offset, sizeof(v));
    return v;
}

std::optional<Block> read_block(const uint8_t* data, size_t size, size_t start, size_t limit) {
    if (start + 6 > limit) return std::nullopt;
    Block b;
    b.start = start;
    const uint16_t length = read16(data, start);
    if (length < 6 || start + length > limit || start + length > size) return std::nullopt;
    b.end = start + length;
    b.value_length = read16(data, start + 2);
    b.type = read16(data, start + 4);
    size_t p = start + 6;
    for (;; p += 2) {
        if (p + 2 > b.end) return std::nullopt;
        const wchar_t c = static_cast<wchar_t>(read16(data, p));
        if (c == 0) break;
        b.key.push_back(c);
    }
    b.value = align4(p + 2);
    const size_t value_bytes = b.type == 1 ? size_t{b.value_length} * 2 : b.value_length;
    b.children = align4(b.value + value_bytes);
    if (b.value > b.end) b.value = b.end;
    if (b.children > b.end) b.children = b.end;
    return b;
}

// The text value of a String block, up to its terminator or the block's end
// (writers disagree on whether wValueLength counts characters or bytes).
std::wstring block_text(const uint8_t* data, const Block& b) {
    std::wstring text;
    for (size_t p = b.value; p + 2 <= b.end; p += 2) {
        const wchar_t c = static_cast<wchar_t>(read16(data, p));
        if (c == 0) break;
        text.push_back(c);
    }
    return text;
}

template <class F>
void for_each_child(const uint8_t* data, size_t size, const Block& parent, F&& f) {
    size_t p = parent.children;
    while (p < parent.end) {
        const auto child = read_block(data, size, p, parent.end);
        if (!child) return;
        f(*child);
        p = align4(child->end);
    }
}

bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b) {
    const std::wstring x = a.lexically_normal().wstring();
    const std::wstring y = b.lexically_normal().wstring();
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) ==
           CSTR_EQUAL;
}

std::string folder_name(HMODULE module, const std::filesystem::path& game_dir) {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (n == 0 || n >= std::size(path)) return "folder unknown";
    const std::filesystem::path folder = std::filesystem::path(path).parent_path();
    wchar_t system[MAX_PATH] = {};
    const UINT s = GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    if (s > 0 && s < std::size(system) && same_folder(folder, system)) return "system folder";
    if (!game_dir.empty() && same_folder(folder, game_dir)) return "game folder";
    return "other folder";
}

// The DXVK version in the module's read-only data, as mapped in memory.
std::optional<std::string> module_dxvk_version(HMODULE module) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return std::nullopt;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const DWORD flags = section->Characteristics;
        if (!(flags & IMAGE_SCN_MEM_READ) || (flags & IMAGE_SCN_MEM_EXECUTE) || (flags & IMAGE_SCN_MEM_DISCARDABLE) ||
            (flags & IMAGE_SCN_MEM_WRITE))
            continue;
        const DWORD size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        if (size == 0 || section->VirtualAddress + size > nt->OptionalHeader.SizeOfImage) continue;
        const std::string_view bytes(reinterpret_cast<const char*>(base + section->VirtualAddress), size);
        if (auto version = find_dxvk_version(bytes)) return version;
    }
    return std::nullopt;
}

}  // namespace

std::optional<WineInfo> detect_wine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return std::nullopt;
    using GetText = const char*(__cdecl*)();
    using GetHost = void(__cdecl*)(const char**, const char**);
    const auto version = reinterpret_cast<GetText>(GetProcAddress(ntdll, "wine_get_version"));
    if (!version) return std::nullopt;
    WineInfo wine;
    wine.version = text_or_empty(version());
    if (const auto build = reinterpret_cast<GetText>(GetProcAddress(ntdll, "wine_get_build_id"))) {
        wine.build_id = text_or_empty(build());
    }
    if (const auto host = reinterpret_cast<GetHost>(GetProcAddress(ntdll, "wine_get_host_version"))) {
        const char* system = nullptr;
        const char* release = nullptr;
        host(&system, &release);
        wine.host_system = text_or_empty(system);
        wine.host_release = text_or_empty(release);
    }
    return wine;
}

std::string wine_log_line(const std::optional<WineInfo>& wine) {
    if (!wine) return "Wine: not detected";
    std::string line = "Wine: " + (wine->version.empty() ? std::string("(no version)") : wine->version);
    std::string details;
    if (!wine->build_id.empty()) details = "build " + wine->build_id;
    if (!wine->host_system.empty()) {
        if (!details.empty()) details += ", ";
        details += "host " + wine->host_system;
        if (!wine->host_release.empty()) details += " " + wine->host_release;
    }
    if (!details.empty()) line += " (" + details + ")";
    return line;
}

VersionResource parse_version_resource(const void* data, size_t size) {
    VersionResource out;
    if (!data) return out;
    const auto* bytes = static_cast<const uint8_t*>(data);
    const auto root = read_block(bytes, size, 0, size);
    if (!root || root->key != L"VS_VERSION_INFO") return out;
    if (root->value_length >= sizeof(VS_FIXEDFILEINFO) && root->value + sizeof(VS_FIXEDFILEINFO) <= root->end) {
        VS_FIXEDFILEINFO fixed;
        std::memcpy(&fixed, bytes + root->value, sizeof(fixed));
        if (fixed.dwSignature == 0xFEEF04BD) {
            out.file_version = std::to_string(HIWORD(fixed.dwFileVersionMS)) + "." +
                               std::to_string(LOWORD(fixed.dwFileVersionMS)) + "." +
                               std::to_string(HIWORD(fixed.dwFileVersionLS)) + "." +
                               std::to_string(LOWORD(fixed.dwFileVersionLS));
        }
    }
    bool table_read = false;
    for_each_child(bytes, size, *root, [&](const Block& info) {
        if (info.key != L"StringFileInfo") return;
        for_each_child(bytes, size, info, [&](const Block& table) {
            if (table_read) return;
            table_read = true;
            for_each_child(bytes, size, table, [&](const Block& entry) {
                out.strings[narrow(entry.key)] = narrow(block_text(bytes, entry));
            });
        });
    });
    return out;
}

VersionResource module_version_resource(HMODULE module) {
    HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    if (!info) return {};
    HGLOBAL loaded = LoadResource(module, info);
    if (!loaded) return {};
    return parse_version_resource(LockResource(loaded), SizeofResource(module, info));
}

std::optional<std::string> find_dxvk_version(std::string_view bytes) {
    static constexpr std::string_view kPrefix("DXVK: \0", 7);
    for (size_t at = bytes.find(kPrefix); at != std::string_view::npos; at = bytes.find(kPrefix, at + 1)) {
        size_t p = at + kPrefix.size();
        // The version string may sit after padding.
        for (size_t pad = 0; p < bytes.size() && bytes[p] == '\0' && pad < 16; ++p, ++pad) {
        }
        std::string version;
        while (p < bytes.size() && version.size() <= 64) {
            const char c = bytes[p++];
            if (c == '\0') break;
            if (c < 0x20 || c > 0x7E) {
                version.clear();
                break;
            }
            version.push_back(c);
        }
        if (version.size() >= 2 && version.size() <= 64 && version[0] == 'v' && version[1] >= '0' && version[1] <= '9')
            return version;
    }
    return std::nullopt;
}

std::string d3d9_log_line(HMODULE d3d9, const std::filesystem::path& game_dir) {
    if (!d3d9) return "d3d9.dll: not loaded";
    const VersionResource resource = module_version_resource(d3d9);
    auto string_of = [&](const char* key) {
        const auto it = resource.strings.find(key);
        return it == resource.strings.end() ? std::string() : it->second;
    };
    const std::string product = string_of("ProductName");
    const std::string company = string_of("CompanyName");
    std::string described = !company.empty() ? company : (!product.empty() ? product : std::string("(no version resource)"));
    if (!resource.file_version.empty()) described += " " + resource.file_version;
    const std::string folder = folder_name(d3d9, game_dir);
    // Microsoft's own d3d9.dll holds no DXVK string; leave its 1 MB unread.
    const bool microsoft = product.find("Windows") != std::string::npos;
    const std::optional<std::string> dxvk = microsoft ? std::nullopt : module_dxvk_version(d3d9);
    if (dxvk) return "d3d9.dll: DXVK " + *dxvk + " (version resource: " + described + ", " + folder + ")";
    std::string line = "d3d9.dll: " + described;
    if (!product.empty() && product != company) line += " (" + product + ", " + folder + ")";
    else line += " (" + folder + ")";
    return line;
}

}  // namespace anvil
