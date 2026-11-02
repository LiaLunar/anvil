#include "zip.hpp"

#include "core/log.hpp"
#include "i18n/translate.hpp"

#include <miniz.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

namespace anvil::manager {

struct ZipArchive::State {
    std::vector<uint8_t> bytes;
    mz_zip_archive zip{};
    std::vector<ZipEntry> entries;

    ~State() { mz_zip_reader_end(&zip); }
};

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    return out;
}

bool reserved_name(std::string_view part) {
    // CON, PRN, AUX, NUL, COM1-9, LPT1-9, with any extension.
    const std::string base = lower(part.substr(0, part.find('.')));
    static constexpr std::array<const char*, 4> plain = {"con", "prn", "aux", "nul"};
    if (std::find(plain.begin(), plain.end(), base) != plain.end()) return true;
    return base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9';
}

std::string zip_error(mz_zip_archive& zip) { return mz_zip_get_error_string(mz_zip_get_last_error(&zip)); }

}  // namespace

std::string unsafe_path_reason(std::string_view path) {
    if (path.empty()) return i18n::tr("an empty name");
    if (path.size() > 240) return i18n::tr("a name longer than 240 characters");
    if (path.front() == '/' || path.front() == '\\') return i18n::tr("an absolute path");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = path.size();
        const std::string_view part = path.substr(start, end - start);
        if (part.empty()) return i18n::tr("an empty folder name");
        if (part == "." || part == "..") return "a '" + std::string(part) + "' part";
        for (const char c : part) {
            if (static_cast<unsigned char>(c) < 0x20 || std::string_view("<>:\"|?*").find(c) != std::string_view::npos) {
                return c == ':' ? i18n::tr("a drive or stream name (':')") : i18n::tr("a character Windows does not allow in names");
            }
        }
        if (part.back() == '.' || part.back() == ' ') return i18n::tr("a part ending in a dot or space");
        if (reserved_name(part)) return i18n::trf("a reserved device name ('{0}')", {part});
        start = end + 1;
    }
    return {};
}

ZipArchive::ZipArchive(std::unique_ptr<State> state) : state_(std::move(state)) {}
ZipArchive::ZipArchive(ZipArchive&&) noexcept = default;
ZipArchive& ZipArchive::operator=(ZipArchive&&) noexcept = default;
ZipArchive::~ZipArchive() = default;

std::optional<ZipArchive> ZipArchive::open(std::vector<uint8_t> bytes, std::string& error) {
    auto state = std::make_unique<State>();
    state->bytes = std::move(bytes);
    if (!mz_zip_reader_init_mem(&state->zip, state->bytes.data(), state->bytes.size(), 0)) {
        error = i18n::trf("not a zip archive ({0})", {zip_error(state->zip)});
        return std::nullopt;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&state->zip);
    if (count > kMaxZipEntries) {
        error = i18n::trf("the archive has more than {0} entries", {std::to_string(kMaxZipEntries)});
        return std::nullopt;
    }
    std::set<std::string> seen;
    uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&state->zip, i, &stat)) {
            error = i18n::trf("entry {0} cannot be read ({1})", {std::to_string(i), zip_error(state->zip)});
            return std::nullopt;
        }
        std::string name = stat.m_filename;
        if (stat.m_is_directory) {
            if (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
            if (const std::string why = unsafe_path_reason(name); !why.empty()) {
                error = "'" + std::string(stat.m_filename) + "' in the archive has " + why;
                return std::nullopt;
            }
            continue;
        }
        if (const std::string why = unsafe_path_reason(name); !why.empty()) {
            error = "'" + name + "' in the archive has " + why;
            return std::nullopt;
        }
        std::replace(name.begin(), name.end(), '\\', '/');
        // Names are UTF-8 only when the entry says so (bit 11); others are code page 437.
        const bool ascii = std::all_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
        if (!ascii && ((stat.m_bit_flag & 0x800) == 0 ||
                       MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                                           nullptr, 0) == 0)) {
            error = "'" + name + "' in the archive is not a UTF-8 name";
            return std::nullopt;
        }
        if (stat.m_is_encrypted) {
            error = "'" + name + "' in the archive is encrypted";
            return std::nullopt;
        }
        if (!stat.m_is_supported) {
            error = "'" + name + "' in the archive uses a compression the Manager cannot read";
            return std::nullopt;
        }
        if (stat.m_uncomp_size > kMaxZipFileBytes) {
            error = "'" + name + "' in the archive is larger than " + std::to_string(kMaxZipFileBytes >> 20) + " MB";
            return std::nullopt;
        }
        total += stat.m_uncomp_size;
        if (total > kMaxZipTotalBytes) {
            error = i18n::trf("the archive unpacks to more than {0} MB", {std::to_string(kMaxZipTotalBytes >> 20)});
            return std::nullopt;
        }
        if (!seen.insert(lower(name)).second) {
            error = "'" + name + "' is in the archive twice (names differ only in case)";
            return std::nullopt;
        }
        state->entries.push_back({name, stat.m_uncomp_size, i});
    }
    // A file may not also be a folder of another one ("a" and "a/b").
    for (const ZipEntry& entry : state->entries) {
        for (size_t slash = entry.path.find('/'); slash != std::string::npos; slash = entry.path.find('/', slash + 1)) {
            if (seen.count(lower(entry.path.substr(0, slash))) != 0) {
                error = "'" + entry.path.substr(0, slash) + "' in the archive is both a file and a folder";
                return std::nullopt;
            }
        }
    }
    return ZipArchive(std::move(state));
}

std::optional<ZipArchive> ZipArchive::open_file(const std::filesystem::path& file, std::string& error) {
    const HANDLE in = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        error = i18n::trf("cannot open {0} (error {1})", {narrow(file.wstring()), std::to_string(GetLastError())});
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(in, &size) != 0;
    if (ok && static_cast<uint64_t>(size.QuadPart) > kMaxZipTotalBytes) {
        CloseHandle(in);
        error = narrow(file.filename().wstring()) + " is larger than " + std::to_string(kMaxZipTotalBytes >> 20) + " MB";
        return std::nullopt;
    }
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(in, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                               read == bytes.size());
    }
    CloseHandle(in);
    if (!ok) {
        error = i18n::trf("cannot read {0}", {narrow(file.wstring())});
        return std::nullopt;
    }
    return open(std::move(bytes), error);
}

const std::vector<ZipEntry>& ZipArchive::entries() const { return state_->entries; }

const ZipEntry* ZipArchive::find(std::string_view path) const {
    for (const ZipEntry& entry : state_->entries) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

std::optional<std::string> ZipArchive::read(const ZipEntry& entry, std::string& error) const {
    std::string out(static_cast<size_t>(entry.size), '\0');
    if (!mz_zip_reader_extract_to_mem(&state_->zip, entry.index, out.data(), out.size(), 0)) {
        error = i18n::trf("cannot unpack '{0}' ({1})", {entry.path, zip_error(state_->zip)});
        return std::nullopt;
    }
    return out;
}

std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive zip{};
    std::vector<uint8_t> out;
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return out;
    bool ok = true;
    for (const auto& [name, bytes] : files) {
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                                   static_cast<mz_uint>(MZ_DEFAULT_LEVEL));
    }
    void* buffer = nullptr;
    size_t size = 0;
    if (ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size)) {
        out.assign(static_cast<uint8_t*>(buffer), static_cast<uint8_t*>(buffer) + size);
        mz_free(buffer);  // finalize hands the buffer over
    }
    mz_zip_writer_end(&zip);
    return out;
}

}  // namespace anvil::manager
