#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anvil {

struct KnownBuild {
    const char* id;
    uint32_t pe_timestamp;
    uint32_t size_of_image;
    uint32_t pe_checksum;
    uint32_t entry_point_rva;
    uint64_t file_size;
    const char* sha256;            // upper-case hex of the whole file
};

struct PeIdentity {
    uintptr_t image_base = 0;
    uint32_t pe_timestamp = 0;
    uint32_t size_of_image = 0;
    uint32_t pe_checksum = 0;
    uint32_t entry_point_rva = 0;
};

// Reads the identity of a mapped PE image (the running eu4.exe).
std::optional<PeIdentity> read_pe_identity(const void* image_base);

// Header-identity match against the compiled-in table (no file hashing).
const KnownBuild* find_known_build(const PeIdentity& identity);
// Full-file match against the compiled-in table (`sha256` in upper-case hex).
const KnownBuild* find_known_build_by_sha256(std::string_view sha256);

// True when `game_dir` is a GOG install: GOG Galaxy and GOG's installers leave
// goggame-2057001589.info there. It is the only sign that works for a build
// Anvil does not know, since a GOG folder has a steam_appid.txt too.
bool is_gog_folder(const std::filesystem::path& game_dir);

// SHA-256 of a file as upper-case hex (BCrypt); empty on failure.
std::string sha256_file(const std::filesystem::path& file);
// SHA-256 of bytes in memory as upper-case hex (BCrypt); empty on failure.
std::string sha256_bytes(const void* data, size_t size);

// Full-file check with a cache keyed by size + last write time, stored in
// `cache_file`. Returns true when the file hash equals build.sha256.
// Meant to run off the critical path (it may hash ~38 MB once).
bool verify_build_file(const KnownBuild& build, const std::filesystem::path& exe, const std::filesystem::path& cache_file,
                       std::string& actual_sha256);

}  // namespace anvil
