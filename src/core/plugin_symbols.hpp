#pragma once

#include "changes.hpp"
#include "symbols.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace anvil {

// One verified symbol of a plugin: its declaration and where it is in the
// running game.
struct PluginSymbol {
    Symbol declaration;
    bool absent = false;  // not in the running build (`absent`): no address
    uint32_t rva = 0;
    uintptr_t address = 0;
    std::vector<std::pair<std::string, uint64_t>> values;  // in declaration order
};

// The verified symbols of one plugin, looked up by AnvilSymbol (the
// declaration index in its symbols.toml). Built after verification succeeded,
// before the plugin DLL is loaded.
class PluginSymbols {
public:
    PluginSymbols() = default;
    // `verified` must be ok() and come from verifying `file`.
    PluginSymbols(const SymbolFile& file, const SymbolVerification& verified, uintptr_t image_base);

    const PluginSymbol* get(uint32_t id) const { return id < symbols_.size() ? &symbols_[id] : nullptr; }
    // get(), but nullptr for a symbol absent on the running build too: what
    // address_of, symbol_value and the changes may use (design §5.2).
    const PluginSymbol* present(uint32_t id) const {
        const PluginSymbol* s = get(id);
        return s != nullptr && !s->absent ? s : nullptr;
    }
    const PluginSymbol* find(std::string_view name) const;
    size_t size() const { return symbols_.size(); }

    // address_of / symbol_value of the host table: nullptr / nullopt
    // for an unknown id or name, or a symbol absent on the running build.
    void* address_of(uint32_t id) const;
    std::optional<uint64_t> value(uint32_t id, std::string_view name) const;

private:
    std::vector<PluginSymbol> symbols_;
};

// Change requests on declared symbols (design §8.1): the expected bytes come
// from the declaration and are compared with the original bytes again, the
// symbol's kind must fit the change. Unknown ids, and symbols absent on the
// running build, are NotDeclared. These are the
// calls the host table (host.cpp) forwards to.
// Hooks need at least the 5 declared bytes of the jump (UnsafeSite): `anvil
// check` proves those are whole instructions (design §16).
ChangeStatus hook_function(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t fn,
                           void* detour, void** next, ChangeId* out = nullptr);
// On a code symbol (a function or a site).
ChangeStatus mid_hook(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t site,
                      AnvilCpuCallback callback, void* user, ChangeId* out = nullptr);
// The block is the symbol's declared bytes; the game resumes at its `resume`
// symbol or at the block's end.
ChangeStatus replace_block(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t block,
                           AnvilCpuCallback callback, void* user, ChangeId* out = nullptr);
ChangeStatus redirect_call(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t call_site,
                           void* target, void** next, ChangeId* out = nullptr);
// The patch must lie within the symbol's declared bytes.
ChangeStatus patch_bytes(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t site,
                         const void* bytes, size_t size, ChangeId* out = nullptr);
// On a slot symbol: the pointer must still hold its `points_to`. `previous`
// follows the chain (ChangeRegistry::swap_pointer).
ChangeStatus swap_pointer(ChangeRegistry& registry, OwnerId owner, const PluginSymbols& symbols, uint32_t slot,
                          void* value, void** previous, ChangeId* out = nullptr);

// The declared target of a symbol: its address and declared bytes and mask.
ChangeTarget target_of(const PluginSymbol& symbol);

}  // namespace anvil
