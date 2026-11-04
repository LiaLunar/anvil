#include "symbol_check.hpp"

namespace anvil {

i18n::Message check_plugin_symbols(const std::filesystem::path& symbols_file, const char* build_id,
                                   const ChangeRegistry& patches, uintptr_t image_base, SymbolVerification* verified,
                                   SymbolFile* declared, const i18n::Message& unknown_build) {
    SymbolFileResult parsed = load_symbols(symbols_file);
    if (!parsed.file) return parsed.error;
    if (build_id == nullptr) return unknown_build;
    if (!select_build(*parsed.file, build_id)) return build_mismatch(*parsed.file, build_id);
    const OriginalImageReader reader(patches, image_base);
    std::string error;
    const auto image = PeImage::parse(reader, error);
    if (!image) return i18n::Message("image_unreadable", {error});
    SymbolVerification result = verify_symbols(*parsed.file, *image);
    if (!result.ok()) return symbol_mismatch(result.problems);
    if (verified != nullptr) *verified = std::move(result);
    if (declared != nullptr) *declared = std::move(*parsed.file);
    return {};
}

}  // namespace anvil
