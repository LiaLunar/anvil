#include "game_build.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace anvil {
namespace {

// Builds Anvil knows. Plugins refer to them by id in symbols.toml. Ids carry
// the store and, from the GOG builds on, all four parts of the version, since
// GOG ships 1.37.5.0 and 1.37.5.2 (design §6); the Steam id keeps its name.
constexpr KnownBuild kKnownBuilds[] = {
    {"eu4-1.37.5-steam-win64", 0x66FE6AB9u, 0x25E0000u, 0x024BAF19u, 0x1A337C8u, 38462504ull,
     "9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A"},
    {"eu4-1.37.5.2-gog-win64", 0x6915E11Fu, 0x25C3000u, 0x0249C6C0u, 0x1A1D448u, 38345336ull,
     "D29CF745F245A5CDD74FE7B500599DDDD4A61089AFFB04B02044056F23155319"},
};

}  // namespace

std::optional<PeIdentity> read_pe_identity(const void* image_base) {
    if (image_base == nullptr) return std::nullopt;
    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(image_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(static_cast<const uint8_t*>(image_base) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return std::nullopt;
    }
    PeIdentity id;
    id.image_base = reinterpret_cast<uintptr_t>(image_base);
    id.pe_timestamp = nt->FileHeader.TimeDateStamp;
    id.size_of_image = nt->OptionalHeader.SizeOfImage;
    id.pe_checksum = nt->OptionalHeader.CheckSum;
    id.entry_point_rva = nt->OptionalHeader.AddressOfEntryPoint;
    return id;
}

const KnownBuild* find_known_build(const PeIdentity& identity) {
    for (const auto& b : kKnownBuilds) {
        if (b.pe_timestamp == identity.pe_timestamp && b.size_of_image == identity.size_of_image &&
            b.pe_checksum == identity.pe_checksum && b.entry_point_rva == identity.entry_point_rva) {
            return &b;
        }
    }
    return nullptr;
}

const KnownBuild* find_known_build_by_sha256(std::string_view sha256) {
    for (const auto& b : kKnownBuilds) {
        if (sha256 == b.sha256) return &b;
    }
    return nullptr;
}

bool is_gog_folder(const std::filesystem::path& game_dir) {
    const DWORD attributes = GetFileAttributesW((game_dir / L"goggame-2057001589.info").c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string known_gog_version() {
    constexpr std::string_view prefix = "eu4-", suffix = "-gog-win64";
    std::string version;
    for (const auto& b : kKnownBuilds) {
        const std::string_view id = b.id;
        if (!id.starts_with(prefix) || !id.ends_with(suffix)) continue;
        version = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    }
    return version;
}

i18n::Message unknown_build_reason(const std::filesystem::path& game_dir) {
    if (is_gog_folder(game_dir)) return i18n::Message("build_unknown_gog", {known_gog_version()});
    return i18n::Message("build_unknown", {});
}

namespace {

// SHA-256 as upper-case hex of what `feed` passes to BCryptHashData; empty on failure.
template <typename Feed> std::string sha256_of(Feed&& feed) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        uint8_t digest[32] = {};
        if (feed(hash) && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char hex[65] = {};
            for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02X", digest[i]);
            result = hex;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

}  // namespace

std::string sha256_file(const std::filesystem::path& file) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    std::string result = sha256_of([&](BCRYPT_HASH_HANDLE hash) {
        std::vector<uint8_t> buffer(1 << 20);
        DWORD read = 0;
        while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0) return false;
        }
        return true;
    });
    CloseHandle(handle);
    return result;
}

std::string sha256_bytes(const void* data, size_t size) {
    return sha256_of([&](BCRYPT_HASH_HANDLE hash) {
        return BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) == 0;
    });
}

bool verify_build_file(const KnownBuild& build, const std::filesystem::path& exe, const std::filesystem::path& cache_file,
                       std::string& actual_sha256) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &data)) return false;
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const uint64_t mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    char key[64];
    std::snprintf(key, sizeof(key), "%llu:%llu", static_cast<unsigned long long>(size), static_cast<unsigned long long>(mtime));

    // Cache format: one line "<size>:<mtime> <sha256>".
    {
        std::ifstream in(cache_file);
        std::string cachedKey, cachedSha;
        if (in >> cachedKey >> cachedSha && cachedKey == key) {
            actual_sha256 = cachedSha;
            return cachedSha == build.sha256;
        }
    }
    actual_sha256 = sha256_file(exe);
    if (actual_sha256.empty()) return false;
    std::error_code ignored;
    std::filesystem::create_directories(cache_file.parent_path(), ignored);
    std::ofstream out(cache_file, std::ios::trunc);
    out << key << ' ' << actual_sha256 << '\n';
    return actual_sha256 == build.sha256;
}

}  // namespace anvil
