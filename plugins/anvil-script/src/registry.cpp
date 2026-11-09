#include "registry.hpp"

#include <cstddef>
#include <cstring>

namespace anvil_script {
namespace {

unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c; }

}  // namespace

bool valid_key(std::string_view owner_id, std::string_view key) {
    if (owner_id.empty() || key.size() < owner_id.size() + 2 || key.size() > kMaxKey) return false;
    for (char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    for (std::size_t i = 0; i < owner_id.size(); ++i) {
        const char expected = owner_id[i] == '-' ? '_' : owner_id[i];
        if (key[i] != expected) return false;
    }
    return key[owner_id.size()] == '_';
}

AnvilResult Registry::add(const Caller& caller, const AnvilPlugin* self, const AnvilScriptEffect* effect,
                          std::string& why) {
    if (self == nullptr || effect == nullptr || !caller.known ||
        effect->size < offsetof(AnvilScriptEffect, execute) + sizeof(effect->execute) || effect->key == nullptr ||
        effect->execute == nullptr) {
        why = "a NULL argument, an unknown caller or a size that does not cover execute";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    const std::string_view key(effect->key, strnlen(effect->key, kMaxKey + 1));
    if (!caller.affects_gameplay) {
        why = "the plugin has affects_gameplay = false; only gameplay plugins add script effects";
        return ANVIL_ERROR_WRONG_KIND;
    }
    if (!caller.early) {
        why = "the plugin is not early; the game may parse its scripts before a main plugin loads";
        return ANVIL_ERROR_WRONG_PHASE;
    }
    std::lock_guard lock(mutex_);
    if (closed()) {
        why = "the game has already parsed script effects";
        return ANVIL_ERROR_WRONG_PHASE;
    }
    if (!valid_key(caller.id, key)) {
        why = "a key is <owner>_<name> with the owner's id (" + caller.id + ", '-' as '_'), [a-z0-9_], at most 63 bytes";
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    const std::size_t n = count_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
        if (key == entries_[i].key) {
            why = "the key is registered already";
            return ANVIL_ERROR_ALREADY_EXISTS;
        }
    }
    if (effect->flags != 0) {
        why = "flags must be 0 in version 1";
        return ANVIL_ERROR_UNSUPPORTED;
    }
    if (n == kMaxEffects) {
        why = "256 keys are registered already";
        return ANVIL_ERROR_OUT_OF_MEMORY;
    }
    Entry& e = entries_[n];
    std::memcpy(e.key, key.data(), key.size());
    e.key[key.size()] = 0;
    e.length = static_cast<std::uint32_t>(key.size());
    e.execute = effect->execute;
    e.describe = effect->size >= offsetof(AnvilScriptEffect, describe) + sizeof(effect->describe) ? effect->describe : nullptr;
    e.context = effect->size >= offsetof(AnvilScriptEffect, context) + sizeof(effect->context) ? effect->context : nullptr;
    e.owner = self;
    const unsigned char c = static_cast<unsigned char>(key[0]);
    first_[c >> 6].fetch_or(std::uint64_t{1} << (c & 63), std::memory_order_relaxed);
    count_.store(n + 1, std::memory_order_release);
    return ANVIL_OK;
}

int Registry::find(const char* key) const noexcept {
    const std::size_t n = count_.load(std::memory_order_acquire);
    if (n == 0 || key == nullptr) return -1;
    const unsigned char c = lower(static_cast<unsigned char>(key[0]));
    if ((first_[c >> 6].load(std::memory_order_relaxed) & (std::uint64_t{1} << (c & 63))) == 0) return -1;
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& e = entries_[i];
        std::uint32_t j = 0;
        while (j < e.length && lower(static_cast<unsigned char>(key[j])) == static_cast<unsigned char>(e.key[j])) ++j;
        if (j == e.length && key[j] == 0) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace anvil_script
