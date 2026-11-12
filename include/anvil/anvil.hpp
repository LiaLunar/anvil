// The C++ wrapper over the plugin API 2 C header (docs/design.md §9).
// Header-only, C++20. What it adds over anvil_plugin.h:
//   - ANVIL_PLUGIN(Type): load() is straight-line code. A failing host call
//     throws anvil::Error; the macro turns any exception into a result, and
//     Anvil rolls back every change the plugin staged.
//   - Typed symbols from the header `anvil gen` writes (symbols.gen.hpp): a
//     detour, a call or a hook object that does not match the signature
//     declared in symbols.toml fails with a static_assert, and the compiler's
//     note names the symbol (sym::Types::<name>).
//   - Mid hooks, block replacements and event handlers take any callable. The
//     adapter catches what it throws, logs the first exception per change and
//     lets the game continue: Anvil's stubs have no unwind information.
//   - anvil::Host is a stateless value: anvil::Host{} works on any thread once
//     the plugin is loading, also inside hooks and in threads it starts.
//   - anvil::Change is a move-only handle: destroying it removes the change.
//   - Typed settings from the header `anvil gen` writes (settings.gen.hpp):
//     cfg::<name>.get(), and Host::subscribe_setting for restart = false
//     settings.
//
// Function-hook detours, redirect targets and import replacements are plain
// functions the game calls directly, so no adapter stands between them and the
// game: they must not throw (declare them noexcept, so a throw terminates
// instead of unwinding through game code).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <anvil/anvil_plugin.h>
#include <anvil/anvil_symbols.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace anvil {

using Cpu = AnvilCpuContext;

constexpr const char* result_name(AnvilResult result) noexcept {
    switch (result) {
    case ANVIL_OK: return "ok";
    case ANVIL_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case ANVIL_ERROR_NOT_DECLARED: return "not declared in symbols.toml or settings.toml";
    case ANVIL_ERROR_WRONG_KIND: return "wrong kind of symbol or setting";
    case ANVIL_ERROR_CONFLICT: return "conflict with another change";
    case ANVIL_ERROR_UNSAFE_SITE: return "unsafe site";
    case ANVIL_ERROR_BYTES_MISMATCH: return "bytes mismatch";
    case ANVIL_ERROR_OUT_OF_MEMORY: return "out of memory";
    case ANVIL_ERROR_WRONG_PHASE: return "wrong phase";
    case ANVIL_ERROR_UNSUPPORTED: return "unsupported";
    case ANVIL_ERROR_INTERNAL: return "internal error";
    case ANVIL_ERROR_NOT_FOUND: return "not found";
    case ANVIL_ERROR_ALREADY_EXISTS: return "already exists";
    case ANVIL_ERROR_SUSPENDED: return "suspended in this session";
    case ANVIL_ERROR_LIMIT: return "over the limit";
    }
    return "unknown result";
}

// A host call that failed, e.g. "hook_function speed_object: conflict with
// another change".
class Error : public std::exception {
public:
    Error(AnvilResult result, std::string message) : result_(result), message_(std::move(message)) {}
    AnvilResult result() const noexcept { return result_; }
    const char* what() const noexcept override { return message_.c_str(); }

private:
    AnvilResult result_;
    std::string message_;
};

// What a symbol of symbols.gen.hpp is.
template <class S>
concept FunctionSymbol = requires { typename S::Signature; } && std::derived_from<S, Function<typename S::Signature>>;
template <class S>
concept SiteSymbol = std::derived_from<S, Site>;
template <class S>
concept SlotSymbol = std::derived_from<S, Slot>;
// Code a hook can go to: a function or a site.
template <class S>
concept CodeSymbol = FunctionSymbol<S> || SiteSymbol<S>;

namespace detail {

// Set by ANVIL_PLUGIN before load() and never cleared: one plugin per DLL.
inline std::atomic<const AnvilHost*> g_host{nullptr};
inline std::atomic<AnvilPlugin*> g_self{nullptr};

inline const AnvilHost& host() {
    const AnvilHost* h = g_host.load(std::memory_order_acquire);
    if (h == nullptr) throw Error(ANVIL_ERROR_WRONG_PHASE, "anvil::Host used before anvil_plugin_load");
    return *h;
}

inline AnvilPlugin* self() noexcept { return g_self.load(std::memory_order_acquire); }

inline void log(AnvilLogLevel level, const char* text) noexcept {
    if (const AnvilHost* h = g_host.load(std::memory_order_acquire)) h->log(self(), level, text);
}

template <class S> constexpr const char* name_of() noexcept {
    if constexpr (requires { S::symbol_name; }) {
        return S::symbol_name;
    } else {
        return "(unnamed symbol)";
    }
}

inline void check(AnvilResult result, const char* call, std::string_view what) {
    if (result == ANVIL_OK) return;
    if (what.empty()) throw Error(result, std::format("{}: {}", call, result_name(result)));
    throw Error(result, std::format("{} {}: {}", call, what, result_name(result)));
}

inline std::string address_text(const void* address) { return std::format("{}", address); }

// A FunctionHook or PointerSwap holds one change: Anvil writes its next or
// previous while the change lives.
inline void require_unused(bool active, const char* call) {
    if (active) {
        throw Error(ANVIL_ERROR_INVALID_ARGUMENT, std::format("{}: the hook object is in use; remove() it first", call));
    }
}

// Only named where a static_assert has already failed, so that no second
// error follows it; never defined.
template <class R> R unreachable();

template <class F> struct ResultOf;
template <class R, class... A> struct ResultOf<R(A...)> {
    using Type = R;
};

// The first exception that leaves a callback of one change is logged; the
// game continues as if the callback had returned.
inline void report(std::atomic<bool>& reported, const char* kind, const std::string& where,
                   const char* message) noexcept {
    if (reported.exchange(true)) return;
    try {
        const std::string text = std::format("{} {}: the callback threw ({}); the game continues, "
                                             "later exceptions of this callback are not logged",
                                             kind, where, message);
        log(ANVIL_LOG_ERROR, text.c_str());
    } catch (...) {
        log(ANVIL_LOG_ERROR, "a callback threw; the game continues");
    }
}

// A mid hook's or block replacement's callable. Never freed: a game thread
// may still be inside it after the change is removed.
template <class F> struct CpuCallback {
    F fn;
    const char* kind;
    std::string where;
    std::atomic<bool> reported{false};

    static void invoke(AnvilCpuContext* cpu, void* user) noexcept {
        auto* self = static_cast<CpuCallback*>(user);
        try {
            if constexpr (std::invocable<F&, Cpu&>) {
                self->fn(*cpu);
            } else {
                self->fn();
            }
        } catch (const std::exception& e) {
            report(self->reported, self->kind, self->where, e.what());
        } catch (...) {
            report(self->reported, self->kind, self->where, "an unknown exception");
        }
    }
};

template <class F> struct EventCallback {
    F fn;
    std::string event;
    std::atomic<bool> reported{false};

    static void invoke(const char* event, const void* payload, void* context) noexcept {
        auto* self = static_cast<EventCallback*>(context);
        try {
            if constexpr (std::invocable<F&, std::string_view, const void*>) {
                self->fn(std::string_view(event), payload);
            } else if constexpr (std::invocable<F&, const void*>) {
                self->fn(payload);
            } else {
                self->fn();
            }
        } catch (const std::exception& e) {
            report(self->reported, "event", self->event, e.what());
        } catch (...) {
            report(self->reported, "event", self->event, "an unknown exception");
        }
    }
};

// A setting subscription's callable: gets the new value (or nothing) on the
// thread that changed it. Kept for the process.
template <class S, class F> struct SettingCallback {
    F fn;
    S setting;
    std::atomic<bool> reported{false};

    static void invoke(AnvilSetting, void* context) noexcept {
        auto* self = static_cast<SettingCallback*>(context);
        try {
            if constexpr (std::invocable<F&, typename S::Value>) {
                self->fn(self->setting.get());
            } else {
                self->fn();
            }
        } catch (const std::exception& e) {
            report(self->reported, "setting", S::setting_name, e.what());
        } catch (...) {
            report(self->reported, "setting", S::setting_name, "an unknown exception");
        }
    }
};

template <class F>
concept CpuCallable = std::invocable<F&, Cpu&> || std::invocable<F&>;
template <class F>
concept EventCallable =
    std::invocable<F&, std::string_view, const void*> || std::invocable<F&, const void*> || std::invocable<F&>;

}  // namespace detail

// A change's handle. Destroying it, assigning over it or reset() removes the
// change; release() keeps the change for the rest of the process.
class Change {
public:
    Change() noexcept = default;
    explicit Change(AnvilChange* handle) noexcept : handle_(handle) {}
    Change(Change&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Change& operator=(Change&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    Change(const Change&) = delete;
    Change& operator=(const Change&) = delete;
    ~Change() { reset(); }

    // Removes the change. A change that is already gone (e.g. rolled back
    // after a failed load) is no error.
    void reset() noexcept {
        if (AnvilChange* handle = std::exchange(handle_, nullptr)) {
            if (const AnvilHost* h = detail::g_host.load(std::memory_order_acquire)) h->remove(detail::self(), handle);
        }
    }
    AnvilChange* release() noexcept { return std::exchange(handle_, nullptr); }
    AnvilChange* get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    AnvilChange* handle_ = nullptr;
};

// A function hook, call redirect or import replacement and the rest of its
// chain. Keep it where it outlives the change (a global or a member of the
// plugin): Anvil writes `next` into it before the change is enabled.
template <class F> class FunctionHook;
template <class R, class... A> class FunctionHook<R(A...)> {
public:
    using Signature = R(A...);
    using Pointer = R (*)(A...);

    FunctionHook() = default;
    FunctionHook(const FunctionHook&) = delete;
    FunctionHook& operator=(const FunctionHook&) = delete;

    // The rest of the chain: the next plugin's detour or the game's code.
    R next(A... args) const { return next_(args...); }
    Pointer next_pointer() const noexcept { return next_; }
    bool active() const noexcept { return static_cast<bool>(change_); }
    // A thread still inside the detour can still call next() afterwards.
    void remove() noexcept { change_.reset(); }

private:
    friend class Host;
    Change change_;
    Pointer next_ = nullptr;
};

// anvil::HookOf<sym::speed_object> is the hook type for that symbol.
template <auto Symbol>
    requires FunctionSymbol<decltype(Symbol)>
using HookOf = FunctionHook<typename decltype(Symbol)::Signature>;

// A pointer swap and the value it passes on. Anvil rewrites `previous` while
// the change lives, so the object must not move (a global or a member of the
// plugin). T is what the slot points to: a function type for a vtable entry
// or callback slot, or void.
template <class T = void> class PointerSwap {
public:
    PointerSwap() = default;
    PointerSwap(const PointerSwap&) = delete;
    PointerSwap& operator=(const PointerSwap&) = delete;

    // The rest of the chain, read on each use: another plugin's value or the
    // slot's original pointer.
    T* previous() const noexcept {
        return reinterpret_cast<T*>(std::atomic_ref<void*>(previous_).load(std::memory_order_acquire));
    }
    bool active() const noexcept { return static_cast<bool>(change_); }
    void remove() noexcept { change_.reset(); }

private:
    friend class Host;
    alignas(8) mutable void* previous_ = nullptr;
    Change change_;
};

// A key binding (settings.toml type "key", e.g. "Ctrl+Shift+F12"): the
// Windows virtual-key code with the modifiers above it, as the Win32 hot-key
// control encodes them. 0 is no binding.
struct Key {
    static constexpr std::int64_t shift_flag = 0x100;
    static constexpr std::int64_t ctrl_flag = 0x200;
    static constexpr std::int64_t alt_flag = 0x400;

    std::int64_t code = 0;

    constexpr int vk() const noexcept { return static_cast<int>(code & 0xFF); }
    constexpr bool shift() const noexcept { return (code & shift_flag) != 0; }
    constexpr bool ctrl() const noexcept { return (code & ctrl_flag) != 0; }
    constexpr bool alt() const noexcept { return (code & alt_flag) != 0; }
    constexpr explicit operator bool() const noexcept { return code != 0; }
    friend constexpr bool operator==(const Key&, const Key&) = default;
};

// A setting of settings.toml, as settings.gen.hpp declares it (cfg::<name>):
// get() reads the value Anvil validated (the default, the player's value or
// an ANVIL_<ID>_<NAME> override). T is bool, std::int64_t, double,
// std::string, anvil::Key or the setting's generated enum class.
template <class T> struct Setting {
    using Value = T;
    AnvilSetting id;

    T get() const {
        const AnvilHost& h = detail::host();
        if constexpr (std::is_same_v<T, bool>) {
            return h.setting_bool(detail::self(), id) != 0;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return h.setting_int(detail::self(), id);
        } else if constexpr (std::is_same_v<T, double>) {
            return h.setting_float(detail::self(), id);
        } else if constexpr (std::is_same_v<T, std::string>) {
            const char* text = h.setting_string(detail::self(), id);
            return text != nullptr ? std::string(text) : std::string();
        } else if constexpr (std::is_same_v<T, Key>) {
            return Key{h.setting_int(detail::self(), id)};
        } else {
            static_assert(std::is_enum_v<T>, "anvil::Setting<T>: T must be a type settings.gen.hpp uses");
            return static_cast<T>(h.setting_int(detail::self(), id));
        }
    }
};

// The host, as a value: copy it, capture it, or write anvil::Host{} (the
// plugin's own handle) anywhere once the plugin is loading; module() gives the
// host of one of its modules. Every change call throws anvil::Error when Anvil
// refuses it; logging never throws.
class Host {
public:
    const AnvilHost& c_api() const { return detail::host(); }
    AnvilPlugin* self() const noexcept { return handle_ != nullptr ? handle_ : detail::self(); }

    // The host of module `id`, which plugin.toml declares in [[modules]]
    // (Anvil 1.22.0, working number; ADR 0011): every call on it acts for the
    // module, which Anvil suspends by its own affects_gameplay and opt-ins.
    // std::nullopt when the player turned the module off (its code should not
    // run then); anvil::Error for an undeclared module or an Anvil without
    // modules (ANVIL_ERROR_UNSUPPORTED).
    std::optional<Host> module(const char* id) const {
        const AnvilHost& h = c_api();
        if (!ANVIL_HOST_HAS(&h, module) || h.module == nullptr) {
            throw Error(ANVIL_ERROR_UNSUPPORTED, "module: this Anvil has no modules (1.22.0 or newer needed)");
        }
        AnvilPlugin* out = nullptr;
        const AnvilResult r = h.module(self(), id, &out);
        if (r == ANVIL_ERROR_DISABLED) return std::nullopt;
        detail::check(r, "module", id);
        Host module_host;
        module_host.handle_ = out;
        return module_host;
    }

    const AnvilGameInfo& game() const { return *c_api().game; }
    std::string_view anvil_version() const { return c_api().anvil_version; }
    std::filesystem::path plugin_dir() const { return c_api().plugin_dir(self()); }
    std::filesystem::path data_dir() const { return c_api().data_dir(self()); }

    // --- log ---------------------------------------------------------------
    void log(AnvilLogLevel level, std::string_view text) const noexcept {
        try {
            emit(level, std::string(text).c_str());
        } catch (...) {
        }
    }
    template <class... Args> void debug(std::format_string<Args...> f, Args&&... args) const noexcept {
        write(ANVIL_LOG_DEBUG, f, std::forward<Args>(args)...);
    }
    template <class... Args> void info(std::format_string<Args...> f, Args&&... args) const noexcept {
        write(ANVIL_LOG_INFO, f, std::forward<Args>(args)...);
    }
    template <class... Args> void warn(std::format_string<Args...> f, Args&&... args) const noexcept {
        write(ANVIL_LOG_WARNING, f, std::forward<Args>(args)...);
    }
    template <class... Args> void error(std::format_string<Args...> f, Args&&... args) const noexcept {
        write(ANVIL_LOG_ERROR, f, std::forward<Args>(args)...);
    }

    // --- changes on declared symbols (design §8) ----------------------------

    // Function hook: the detour must have the signature symbols.toml declares.
    template <class Sig, class S, class D> void hook(FunctionHook<Sig>& hook, const S& fn, D detour) const {
        static_assert(FunctionSymbol<S>, "anvil::Host::hook needs a function symbol (one with a `signature` in "
                                         "symbols.toml); the symbol is named in the note below");
        if constexpr (FunctionSymbol<S>) {
            static_assert(std::is_same_v<Sig, typename S::Signature>,
                          "anvil::Host::hook: the hook object is not the HookOf<> of this symbol; the symbol is "
                          "named in the note below");
            static_assert(std::is_convertible_v<D, typename S::Pointer>,
                          "anvil::Host::hook: the detour does not match the signature symbols.toml declares for "
                          "this symbol; the symbol is named in the note below");
            if constexpr (std::is_same_v<Sig, typename S::Signature> && std::is_convertible_v<D, typename S::Pointer>) {
                const typename S::Pointer target = detour;
                detail::require_unused(hook.active(), "hook_function");
                AnvilChange* handle = nullptr;
                detail::check(c_api().hook_function(self(), fn.id, reinterpret_cast<void*>(target),
                                                    reinterpret_cast<void**>(&hook.next_), &handle),
                              "hook_function", detail::name_of<S>());
                hook.change_ = Change(handle);
            }
        }
    }

    // Mid hook: `callback(anvil::Cpu&)` (or `callback()`) runs before the code
    // at a function or site symbol. It is kept for the process.
    template <class S, class F>
    [[nodiscard("keep the Change: destroying it removes the mid hook")]] Change mid_hook(const S& site,
                                                                                         F&& callback) const {
        static_assert(CodeSymbol<S>, "anvil::Host::mid_hook needs a site or function symbol; the symbol is named in "
                                     "the note below");
        static_assert(detail::CpuCallable<std::decay_t<F>>,
                      "anvil::Host::mid_hook: the callback must be callable as callback(anvil::Cpu&) or callback()");
        if constexpr (CodeSymbol<S> && detail::CpuCallable<std::decay_t<F>>) {
            return cpu_change<std::decay_t<F>>("mid_hook", detail::name_of<S>(), std::forward<F>(callback),
                                               [&](AnvilCpuCallback fn, void* user, AnvilChange** out) {
                                                   return c_api().mid_hook(self(), site.id, fn, user, out);
                                               });
        } else {
            return Change();
        }
    }

    // Block replacement: the declared bytes never run; `callback` runs instead
    // and the game resumes at the block's `resume` or end (unless it writes
    // cpu.rip).
    template <class S, class F>
    [[nodiscard("keep the Change: destroying it removes the block replacement")]] Change replace_block(
        const S& block, F&& callback) const {
        static_assert(CodeSymbol<S>, "anvil::Host::replace_block needs a site or function symbol; the symbol is "
                                     "named in the note below");
        static_assert(detail::CpuCallable<std::decay_t<F>>,
                      "anvil::Host::replace_block: the callback must be callable as callback(anvil::Cpu&) or "
                      "callback()");
        if constexpr (CodeSymbol<S> && detail::CpuCallable<std::decay_t<F>>) {
            return cpu_change<std::decay_t<F>>("replace_block", detail::name_of<S>(), std::forward<F>(callback),
                                               [&](AnvilCpuCallback fn, void* user, AnvilChange** out) {
                                                   return c_api().replace_block(self(), block.id, fn, user, out);
                                               });
        } else {
            return Change();
        }
    }

    // Call redirect at a `call rel32` site: `target` gets the call, `hook.next`
    // is the previous target. The site does not declare a signature, so the
    // hook object's type gives it.
    template <class Sig, class S, class D> void redirect(FunctionHook<Sig>& hook, const S& call_site, D target) const {
        static_assert(SiteSymbol<S>, "anvil::Host::redirect needs a site symbol (a `call rel32`); the symbol is "
                                     "named in the note below");
        static_assert(std::is_convertible_v<D, typename FunctionHook<Sig>::Pointer>,
                      "anvil::Host::redirect: the target does not match the hook object's signature");
        if constexpr (SiteSymbol<S> && std::is_convertible_v<D, typename FunctionHook<Sig>::Pointer>) {
            const typename FunctionHook<Sig>::Pointer fn = target;
            detail::require_unused(hook.active(), "redirect_call");
            AnvilChange* handle = nullptr;
            detail::check(c_api().redirect_call(self(), call_site.id, reinterpret_cast<void*>(fn),
                                                reinterpret_cast<void**>(&hook.next_), &handle),
                          "redirect_call", detail::name_of<S>());
            hook.change_ = Change(handle);
        }
    }

    // Byte patch within the symbol's declared bytes.
    template <class S>
    [[nodiscard("keep the Change: destroying it removes the byte patch")]] Change patch(
        const S& site, std::span<const std::uint8_t> bytes) const {
        static_assert(!SlotSymbol<S>, "anvil::Host::patch: a pointer slot is changed with swap(); the symbol is "
                                      "named in the note below");
        AnvilChange* handle = nullptr;
        detail::check(c_api().patch_bytes(self(), site.id, bytes.data(), bytes.size(), &handle), "patch_bytes",
                      detail::name_of<S>());
        return Change(handle);
    }
    template <class S>
    [[nodiscard("keep the Change: destroying it removes the byte patch")]] Change patch(
        const S& site, std::initializer_list<std::uint8_t> bytes) const {
        return patch(site, std::span<const std::uint8_t>(bytes.begin(), bytes.size()));
    }

    // Pointer swap on a slot symbol: it holds `value`, swap.previous() the
    // rest of the chain.
    template <class T, class S, class V> void swap(PointerSwap<T>& swap, const S& slot, V value) const {
        static_assert(SlotSymbol<S>, "anvil::Host::swap needs a pointer slot symbol (`points_to` in symbols.toml); "
                                     "the symbol is named in the note below");
        static_assert(std::is_convertible_v<V, T*>, "anvil::Host::swap: the value is not a T* of the PointerSwap<T>");
        if constexpr (SlotSymbol<S> && std::is_convertible_v<V, T*>) {
            T* const pointer = value;
            detail::require_unused(swap.active(), "swap_pointer");
            AnvilChange* handle = nullptr;
            detail::check(c_api().swap_pointer(self(), slot.id, to_void(pointer), &swap.previous_, &handle),
                          "swap_pointer", detail::name_of<S>());
            swap.change_ = Change(handle);
        }
    }

    // Import replacement in eu4.exe's import table, e.g. ("KERNEL32.dll", "GetTickCount").
    template <class Sig, class D>
    void replace_import(FunctionHook<Sig>& hook, const char* module, const char* function, D replacement) const {
        static_assert(std::is_convertible_v<D, typename FunctionHook<Sig>::Pointer>,
                      "anvil::Host::replace_import: the replacement does not match the hook object's signature");
        if constexpr (std::is_convertible_v<D, typename FunctionHook<Sig>::Pointer>) {
            const typename FunctionHook<Sig>::Pointer fn = replacement;
            detail::require_unused(hook.active(), "replace_import");
            AnvilChange* handle = nullptr;
            detail::check(c_api().replace_import(self(), module, function, reinterpret_cast<void*>(fn),
                                                 reinterpret_cast<void**>(&hook.next_), &handle),
                          "replace_import", std::string(module) + "!" + function);
            hook.change_ = Change(handle);
        }
    }

    // --- changes outside eu4.exe (e.g. Direct3D 9), by raw address ----------

    template <class Sig, class D>
    void hook_at(FunctionHook<Sig>& hook, void* address, D detour,
                 std::span<const std::uint8_t> expected_prologue = {}) const {
        static_assert(std::is_convertible_v<D, typename FunctionHook<Sig>::Pointer>,
                      "anvil::Host::hook_at: the detour does not match the hook object's signature");
        if constexpr (std::is_convertible_v<D, typename FunctionHook<Sig>::Pointer>) {
            const typename FunctionHook<Sig>::Pointer fn = detour;
            detail::require_unused(hook.active(), "hook_function_at");
            AnvilChange* handle = nullptr;
            detail::check(c_api().hook_function_at(self(), address, data_or_null(expected_prologue),
                                                   expected_prologue.size(), reinterpret_cast<void*>(fn),
                                                   reinterpret_cast<void**>(&hook.next_), &handle),
                          "hook_function_at", detail::address_text(address));
            hook.change_ = Change(handle);
        }
    }

    template <class F>
    [[nodiscard("keep the Change: destroying it removes the mid hook")]] Change mid_hook_at(
        void* address, F&& callback, std::span<const std::uint8_t> expected = {}) const {
        static_assert(detail::CpuCallable<std::decay_t<F>>,
                      "anvil::Host::mid_hook_at: the callback must be callable as callback(anvil::Cpu&) or callback()");
        if constexpr (detail::CpuCallable<std::decay_t<F>>) {
            return cpu_change<std::decay_t<F>>("mid_hook_at", detail::address_text(address), std::forward<F>(callback),
                                               [&](AnvilCpuCallback fn, void* user, AnvilChange** out) {
                                                   return c_api().mid_hook_at(self(), address, data_or_null(expected),
                                                                              expected.size(), fn, user, out);
                                               });
        } else {
            return Change();
        }
    }

    // After load: the changes `body` requests are enabled together, or none
    // when it throws (the exception goes on). Inside load() the load itself is
    // the group, so this throws ANVIL_ERROR_WRONG_PHASE there.
    template <class F> void group(F&& body) const {
        detail::check(c_api().begin_group(self()), "begin_group", "");
        try {
            std::forward<F>(body)();
        } catch (...) {
            c_api().abort_group(self());
            throw;
        }
        detail::check(c_api().commit_group(self()), "commit_group", "");
    }

    // --- addresses and calls ------------------------------------------------

    // The verified function; call it with call() to have the arguments checked.
    template <class S> typename S::Pointer function(const S& fn) const {
        static_assert(FunctionSymbol<S>, "anvil::Host::function needs a function symbol; the symbol is named in the "
                                         "note below");
        return reinterpret_cast<typename S::Pointer>(address(fn));
    }

    // Calls a declared game function with arguments that fit its signature.
    template <class S, class... Args>
        requires FunctionSymbol<S>
    typename detail::ResultOf<typename S::Signature>::Type call(const S& fn, Args&&... args) const {
        using R = typename detail::ResultOf<typename S::Signature>::Type;
        static_assert(std::is_invocable_v<typename S::Pointer, Args...>,
                      "anvil::Host::call: the arguments do not match the signature symbols.toml declares for this "
                      "symbol; the symbol is named in the note below");
        if constexpr (std::is_invocable_v<typename S::Pointer, Args...>) {
            return function(fn)(std::forward<Args>(args)...);
        } else {
            return detail::unreachable<R>();
        }
    }

    // The verified address of any symbol.
    template <class S> void* address(const S& symbol) const {
        void* p = c_api().address_of(self(), symbol.id);
        if (p == nullptr) {
            throw Error(ANVIL_ERROR_NOT_DECLARED,
                        std::format("address_of {}: not declared, or absent on this game build", detail::name_of<S>()));
        }
        return p;
    }

    // False for a symbol symbols.toml declares `absent` on the running game
    // build (design §5.2): check it before calling, hooking or reading one.
    template <class S> bool present(const S& symbol) const { return c_api().address_of(self(), symbol.id) != nullptr; }

    // Typed data: a `type` symbol, or a typed rip_relative value (sym::root).
    template <class T> T* data(const Data<T>& d) const {
        if (d.value == nullptr) return static_cast<T*>(address(d));
        return reinterpret_cast<T*>(game().image_base + static_cast<std::uintptr_t>(value(d, d.value)));
    }

    // A value of symbols.toml `values` (rip_relative ones are RVAs).
    template <class S> std::uint64_t value(const S& symbol, const char* name) const {
        std::uint64_t out = 0;
        detail::check(c_api().symbol_value(self(), symbol.id, name, &out), "symbol_value",
                      std::format("{}.{}", detail::name_of<S>(), name));
        return out;
    }

    // --- services and events ---------------------------------------------------

    // `service` is a struct of function pointers whose first field is
    // `uint32_t size`; it must live for the process.
    template <class T> void register_service(const char* name, std::uint32_t version, const T& service) const {
        detail::check(c_api().register_service(self(), name, version, &service), "register_service", name);
    }
    template <class T> const T* service(const char* name, std::uint32_t version) const {
        return static_cast<const T*>(c_api().get_service(self(), name, version));
    }

    // `handler(std::string_view event, const void* payload)`, `handler(payload)`
    // or `handler()`; kept for the process.
    template <class F> void subscribe(const char* event, F&& handler) const {
        static_assert(detail::EventCallable<std::decay_t<F>>,
                      "anvil::Host::subscribe: the handler must be callable as (std::string_view, const void*), "
                      "(const void*) or ()");
        if constexpr (detail::EventCallable<std::decay_t<F>>) {
            using Callback = detail::EventCallback<std::decay_t<F>>;
            auto* callback = new Callback{std::forward<F>(handler), event};
            detail::check(c_api().subscribe(self(), event, &Callback::invoke, callback), "subscribe", event);
        }
    }
    void publish(const char* event, const void* payload = nullptr) const {
        detail::check(c_api().publish(self(), event, payload), "publish", event);
    }

    // --- settings (settings.gen.hpp) ----------------------------------------------

    // `handler(new value)` or `handler()` runs after a restart = false setting
    // changes (in the overlay), on the thread that changed it; kept for the
    // process. A restart = true setting does not compile: it changes at the
    // next start only.
    template <class S, class F> void subscribe_setting(const S& setting, F&& handler) const {
        static_assert(!S::restart, "anvil::Host::subscribe_setting: this setting applies at the next start "
                                   "(restart = true in settings.toml), so it never changes while the game runs");
        static_assert(std::invocable<std::decay_t<F>&, typename S::Value> || std::invocable<std::decay_t<F>&>,
                      "anvil::Host::subscribe_setting: the handler must be callable as (value) or ()");
        if constexpr (!S::restart) {
            using Callback = detail::SettingCallback<S, std::decay_t<F>>;
            auto* callback = new Callback{std::forward<F>(handler), setting};
            detail::check(c_api().subscribe_setting(self(), setting.id, &Callback::invoke, callback),
                          "subscribe_setting", S::setting_name);
        }
    }

private:
    // A module's handle (module()); null: the plugin's own.
    AnvilPlugin* handle_ = nullptr;

    // The log under this host's handle: a module's lines carry its key.
    void emit(AnvilLogLevel level, const char* text) const noexcept {
        if (const AnvilHost* h = detail::g_host.load(std::memory_order_acquire)) h->log(self(), level, text);
    }

    template <class... Args> void write(AnvilLogLevel level, std::format_string<Args...> f, Args&&... args) const noexcept {
        try {
            const std::string text = std::format(f, std::forward<Args>(args)...);
            emit(level, text.c_str());
        } catch (...) {
            emit(level, "(a log line could not be formatted)");
        }
    }

    template <class T> static void* to_void(T* pointer) noexcept {
        if constexpr (std::is_function_v<T>) {
            return reinterpret_cast<void*>(pointer);
        } else {
            return const_cast<void*>(static_cast<const volatile void*>(pointer));
        }
    }

    static const void* data_or_null(std::span<const std::uint8_t> bytes) noexcept {
        return bytes.empty() ? nullptr : bytes.data();
    }

    template <class F, class Callable, class Request>
    Change cpu_change(const char* call, std::string where, Callable&& callable, Request&& request) const {
        using Callback = detail::CpuCallback<F>;
        // Never freed: a game thread may still run it after the change is removed.
        auto* callback = new Callback{std::forward<Callable>(callable), call, where};
        AnvilChange* handle = nullptr;
        detail::check(request(&Callback::invoke, callback, &handle), call, where);
        return Change(handle);
    }
};

// The plugin: one object, created by ANVIL_PLUGIN when Anvil loads the DLL
// and kept for the process.
class Plugin {
public:
    virtual ~Plugin() = default;
    // Request the changes here; they are enabled together when it returns.
    // Throw (or let a Host call throw) to refuse loading: Anvil rolls back.
    virtual void load(Host& host) = 0;
};

namespace detail {

inline void report_load(const char* what) noexcept {
    try {
        log(ANVIL_LOG_ERROR, std::format("load() failed: {}", what).c_str());
    } catch (...) {
        log(ANVIL_LOG_ERROR, "load() failed");
    }
}

template <class T> AnvilResult load_plugin(const AnvilHost* host, AnvilPlugin* plugin_self) noexcept {
    static_assert(std::derived_from<T, Plugin>, "ANVIL_PLUGIN(Type): Type must derive from anvil::Plugin");
    if (host == nullptr || host->api_version != ANVIL_API_VERSION || !ANVIL_HOST_HAS(host, publish)) {
        return ANVIL_ERROR_UNSUPPORTED;
    }
    g_self.store(plugin_self, std::memory_order_release);
    g_host.store(host, std::memory_order_release);
    try {
        Host h;
        // Kept for the process, also when load() fails: threads it started may
        // still use it.
        T* plugin = new T();
        plugin->load(h);
        return ANVIL_OK;
    } catch (const Error& e) {
        report_load(e.what());
        return e.result() != ANVIL_OK ? e.result() : ANVIL_ERROR_INTERNAL;
    } catch (const std::exception& e) {
        report_load(e.what());
        return ANVIL_ERROR_INTERNAL;
    } catch (...) {
        report_load("an unknown exception");
        return ANVIL_ERROR_INTERNAL;
    }
}

}  // namespace detail

}  // namespace anvil

// One line per plugin, in one source file: exports anvil_plugin_load, which
// creates the plugin object and calls its load().
#define ANVIL_PLUGIN(Type)                                                                               \
    extern "C" ANVIL_PLUGIN_EXPORT AnvilResult anvil_plugin_load(const AnvilHost* host, AnvilPlugin* self) { \
        return ::anvil::detail::load_plugin<Type>(host, self);                                           \
    }
