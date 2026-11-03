#include "plugin_symbols.hpp"

namespace anvil {

PluginSymbols::PluginSymbols(const SymbolFile& file, const SymbolVerification& verified, uintptr_t image_base) {
    symbols_.reserve(file.symbols.size());
    for (size_t i = 0; i < file.symbols.size() && i < verified.symbols.size(); ++i) {
        PluginSymbol s;
        s.declaration = file.symbols[i];
        s.absent = verified.symbols[i].absent;
        s.rva = verified.symbols[i].rva;
        s.address = s.absent ? 0 : image_base + s.rva;
        s.values = verified.symbols[i].values;
        symbols_.push_back(std::move(s));
    }
}

const PluginSymbol* PluginSymbols::find(std::string_view name) const {
    for (const auto& s : symbols_) {
        if (s.declaration.name == name) return &s;
    }
    return nullptr;
}

void* PluginSymbols::address_of(uint32_t id) const {
    const PluginSymbol* s = present(id);
    return s != nullptr ? reinterpret_cast<void*>(s->address) : nullptr;
}

std::optional<uint64_t> PluginSymbols::value(uint32_t id, std::string_view name) const {
    const PluginSymbol* s = present(id);
    if (s == nullptr) return std::nullopt;
    for (const auto& [n, v] : s->values) {
        if (n == name) return v;
    }
    return std::nullopt;
}

ChangeTarget target_of(const PluginSymbol& symbol) {
    ChangeTarget t;
    t.address = symbol.address;
    t.inside_image = true;
    t.expected = symbol.declaration.bytes.data();
    t.mask = symbol.declaration.mask.empty() ? nullptr : symbol.declaration.mask.data();
    t.expected_size = symbol.declaration.bytes.size();
    t.symbol = symbol.declaration.name.c_str();
    return t;
}

namespace {

// A hook writes a 5-byte jump over declared, verified code.
ChangeStatus hookable(const PluginSymbol* s, bool function_only) {
    if (s == nullptr) return ChangeStatus::NotDeclared;
    const Symbol& d = s->declaration;
    if (d.kind != SymbolKind::Function && (function_only || d.kind != SymbolKind::Site)) return ChangeStatus::WrongKind;
    if (d.memory && *d.memory != MemoryKind::Code) return ChangeStatus::WrongKind;
    if (d.bytes.size() < 5) return ChangeStatus::UnsafeSite;
    return ChangeStatus::Ok;
}

}  // namespace

ChangeStatus hook_function(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t fn,
                           void* detour, void** next, ChangeId* out) {
    const PluginSymbol* s = symbols.present(fn);
    if (const ChangeStatus status = hookable(s, true); status != ChangeStatus::Ok) return status;
    return registry.hook_function(owner, target_of(*s), detour, next, out);
}

ChangeStatus mid_hook(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t site,
                      AnvilCpuCallback callback, void* user, ChangeId* out) {
    const PluginSymbol* s = symbols.present(site);
    if (const ChangeStatus status = hookable(s, false); status != ChangeStatus::Ok) return status;
    return registry.mid_hook(owner, target_of(*s), callback, user, out);
}

ChangeStatus replace_block(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t block,
                           AnvilCpuCallback callback, void* user, ChangeId* out) {
    const PluginSymbol* s = symbols.present(block);
    if (const ChangeStatus status = hookable(s, false); status != ChangeStatus::Ok) return status;
    uintptr_t resume = 0;  // the block's end
    if (!s->declaration.resume.empty()) {
        const PluginSymbol* r = symbols.find(s->declaration.resume);
        if (r == nullptr || r->absent) return ChangeStatus::NotDeclared;
        resume = r->address;
    }
    return registry.replace_block(owner, target_of(*s), s->declaration.bytes.size(), resume, callback, user, out);
}

ChangeStatus redirect_call(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t call_site,
                           void* target, void** next, ChangeId* out) {
    const PluginSymbol* s = symbols.present(call_site);
    if (s == nullptr) return ChangeStatus::NotDeclared;
    if (s->declaration.kind != SymbolKind::Site) return ChangeStatus::WrongKind;
    return registry.redirect_call(owner, target_of(*s), 0, target, next, out);
}

ChangeStatus patch_bytes(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t site,
                         const void* bytes, size_t size, ChangeId* out) {
    const PluginSymbol* s = symbols.present(site);
    if (s == nullptr) return ChangeStatus::NotDeclared;
    if (s->declaration.kind == SymbolKind::Slot) return ChangeStatus::WrongKind;  // swap_pointer
    if (size == 0 || size > s->declaration.bytes.size()) return ChangeStatus::InvalidArgument;
    return registry.patch_bytes(owner, target_of(*s), bytes, size, out);
}

ChangeStatus swap_pointer(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t slot,
                          void* value, void** previous, ChangeId* out) {
    const PluginSymbol* s = symbols.present(slot);
    if (s == nullptr) return ChangeStatus::NotDeclared;
    if (s->declaration.kind != SymbolKind::Slot || !s->declaration.points_to) return ChangeStatus::WrongKind;
    const uintptr_t image_base = s->address - s->rva;
    return registry.swap_pointer(owner, target_of(*s), image_base + *s->declaration.points_to, value, previous, out);
}

}  // namespace anvil
