use std::ffi::{c_char, c_void, CStr};
use std::fmt::{self, Display, Write as _};
use std::mem::{offset_of, size_of};
use std::path::PathBuf;
use std::ptr::{self, NonNull};
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};

use crate::change::{Change, FunctionHook, PointerSwap};
use crate::cpu::Cpu;
use crate::error::{check, need, Result};
use crate::event::{Event, Payload};
use crate::fnptr::FnPtr;
use crate::panic::{catch_raw, run_callback};
use crate::sys::{
    AnvilChange, AnvilCpuCallback, AnvilCpuContext, AnvilHost, AnvilLogLevel, AnvilPlugin,
    AnvilResult, AnvilSetting, AnvilSymbol, ANVIL_LOG_DEBUG, ANVIL_LOG_ERROR, ANVIL_LOG_INFO,
    ANVIL_LOG_WARNING,
};
use crate::text::{c_string, owned_string, static_str};

static TABLE: AtomicPtr<AnvilHost> = AtomicPtr::new(ptr::null_mut());
static PLUGIN: AtomicPtr<AnvilPlugin> = AtomicPtr::new(ptr::null_mut());

/// Whether the table has every function of API 2.0 (up to `publish`).
pub(crate) fn covers_api_2_0(table: &AnvilHost) -> bool {
    table.size as usize
        >= offset_of!(AnvilHost, publish) + size_of::<Option<unsafe extern "C" fn()>>()
}

/// Keeps the host table and the plugin's handle for every later call; Anvil
/// keeps both for the process.
pub(crate) fn install(table: &'static AnvilHost, plugin: *mut AnvilPlugin) {
    PLUGIN.store(plugin, Ordering::Release);
    TABLE.store(ptr::from_ref(table).cast_mut(), Ordering::Release);
}

/// A log level of `anvil.log`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum LogLevel {
    /// Debug.
    Debug,
    /// Information.
    Info,
    /// Warning.
    Warning,
    /// Error.
    Error,
}

impl LogLevel {
    const fn raw(self) -> AnvilLogLevel {
        match self {
            Self::Debug => ANVIL_LOG_DEBUG,
            Self::Info => ANVIL_LOG_INFO,
            Self::Warning => ANVIL_LOG_WARNING,
            Self::Error => ANVIL_LOG_ERROR,
        }
    }
}

/// The running game executable.
#[derive(Clone, Copy, Debug)]
pub struct GameInfo {
    /// `eu4-1.37.5-steam-win64`; `None` when unknown.
    pub build_id: Option<&'static str>,
    /// Where eu4.exe is in memory.
    pub image_base: usize,
    /// The size of the image.
    pub image_size: u32,
    /// The SHA-256 of eu4.exe, upper-case hex; `None` until Anvil verified
    /// it off the critical path.
    pub sha256: Option<&'static str>,
}

/// A key binding (settings.toml type `key`): the Windows virtual-key code in
/// bits 0-7 with the modifiers above it; 0 is no binding.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Key(pub i64);

impl Key {
    /// Shift.
    pub const SHIFT: i64 = 0x100;
    /// Ctrl.
    pub const CTRL: i64 = 0x200;
    /// Alt.
    pub const ALT: i64 = 0x400;

    /// The virtual-key code.
    pub const fn vk(self) -> u8 {
        (self.0 & 0xFF) as u8
    }
    /// Whether Shift is part of it.
    pub const fn shift(self) -> bool {
        self.0 & Self::SHIFT != 0
    }
    /// Whether Ctrl is part of it.
    pub const fn ctrl(self) -> bool {
        self.0 & Self::CTRL != 0
    }
    /// Whether Alt is part of it.
    pub const fn alt(self) -> bool {
        self.0 & Self::ALT != 0
    }
    /// Whether a key is bound.
    pub const fn is_bound(self) -> bool {
        self.0 != 0
    }
}

/// The host: Anvil's table and this plugin's handle. It is `Copy`: capture it
/// in callbacks, or get it anywhere with [`Host::current`] once the plugin is
/// loading.
///
/// Logging never fails. Every other call returns the [`Error`](crate::Error)
/// Anvil answered with.
#[derive(Clone, Copy)]
pub struct Host {
    table: &'static AnvilHost,
    plugin: *mut AnvilPlugin,
}

// SAFETY: Anvil's host functions may be called from any thread; the handle is an id.
unsafe impl Send for Host {}
// SAFETY: as above.
unsafe impl Sync for Host {}

impl fmt::Debug for Host {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Host")
            .field("anvil_version", &self.anvil_version())
            .finish_non_exhaustive()
    }
}

/// The adapter of a mid hook or block replacement; never freed (a game
/// thread may still run it after the change is removed).
struct CpuAdapter<F> {
    callback: F,
    what: String,
    panicked: AtomicBool,
}

unsafe extern "C" fn cpu_adapter<F: Fn(&mut Cpu) + Send + Sync + 'static>(
    cpu: *mut AnvilCpuContext,
    user: *mut c_void,
) {
    // SAFETY: `user` is the adapter this crate registered with the change.
    let adapter = unsafe { &*user.cast::<CpuAdapter<F>>() };
    // SAFETY: the context Anvil passes, valid during the callback.
    let Some(cpu) = (unsafe { Cpu::from_raw(cpu) }) else {
        return;
    };
    run_callback(&adapter.what, &adapter.panicked, (), || {
        (adapter.callback)(cpu)
    });
}

struct EventAdapter<P, F> {
    handler: F,
    what: String,
    panicked: AtomicBool,
    _payload: std::marker::PhantomData<fn() -> P>,
}

unsafe extern "C" fn event_adapter<P: Payload, F: Fn(&P) + Send + Sync + 'static>(
    _event: *const c_char,
    payload: *const c_void,
    context: *mut c_void,
) {
    // SAFETY: `context` is the adapter this crate subscribed with.
    let adapter = unsafe { &*context.cast::<EventAdapter<P, F>>() };
    run_callback(&adapter.what, &adapter.panicked, (), || {
        // SAFETY: the Event<P> promises that P reads this event's payloads.
        if let Some(payload) = unsafe { P::read(payload) } {
            (adapter.handler)(&payload);
        }
    });
}

struct RawEventAdapter<F> {
    handler: F,
    what: String,
    panicked: AtomicBool,
}

unsafe extern "C" fn raw_event_adapter<F: Fn(&str, *const c_void) + Send + Sync + 'static>(
    event: *const c_char,
    payload: *const c_void,
    context: *mut c_void,
) {
    // SAFETY: `context` is the adapter this crate subscribed with.
    let adapter = unsafe { &*context.cast::<RawEventAdapter<F>>() };
    run_callback(&adapter.what, &adapter.panicked, (), || {
        // SAFETY: the event name Anvil passes, valid during the call.
        let name = unsafe { owned_string(event) };
        (adapter.handler)(&name, payload);
    });
}

struct SettingAdapter<F> {
    handler: F,
    what: String,
    panicked: AtomicBool,
}

unsafe extern "C" fn setting_adapter<F: Fn() + Send + Sync + 'static>(
    _setting: AnvilSetting,
    context: *mut c_void,
) {
    // SAFETY: `context` is the adapter this crate subscribed with.
    let adapter = unsafe { &*context.cast::<SettingAdapter<F>>() };
    run_callback(&adapter.what, &adapter.panicked, (), || (adapter.handler)());
}

/// Writes one log line through the host; never fails.
fn write_log(table: &AnvilHost, plugin: *mut AnvilPlugin, level: LogLevel, message: &dyn Display) {
    let Some(log) = table.log else {
        return;
    };
    let mut text = String::new();
    if write!(text, "{message}").is_err() {
        text = "(a log line could not be formatted)".to_owned();
    }
    let text = c_string(&text);
    // SAFETY: the host's own function with this plugin and a NUL-terminated UTF-8 line.
    unsafe { log(plugin, level.raw(), text.as_ptr()) };
}

fn wide_path(text: *const u16) -> PathBuf {
    if text.is_null() {
        return PathBuf::new();
    }
    // SAFETY: a NUL-terminated wide string the host keeps.
    let units = unsafe {
        let mut length = 0;
        while *text.add(length) != 0 {
            length += 1;
        }
        std::slice::from_raw_parts(text, length)
    };
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStringExt;
        PathBuf::from(std::ffi::OsString::from_wide(units))
    }
    #[cfg(not(windows))]
    {
        PathBuf::from(String::from_utf16_lossy(units))
    }
}

impl Host {
    pub(crate) fn from_parts(table: &'static AnvilHost, plugin: *mut AnvilPlugin) -> Self {
        Self { table, plugin }
    }

    /// The host of this plugin; `None` before Anvil called `anvil_plugin_load`.
    pub fn current() -> Option<Self> {
        let table = TABLE.load(Ordering::Acquire);
        let plugin = PLUGIN.load(Ordering::Acquire);
        // SAFETY: install() stored Anvil's table, which lives for the process.
        unsafe { table.as_ref() }.map(|table| Self { table, plugin })
    }

    /// Anvil's C table, for what this crate does not wrap.
    pub fn c_api(&self) -> &'static AnvilHost {
        self.table
    }

    /// This plugin's handle, the `self` of every C host call.
    pub fn raw_plugin(&self) -> *mut AnvilPlugin {
        self.plugin
    }

    /// The host of module `name`, which `plugin.toml` declares in
    /// `[[modules]]` (Anvil 1.21.0, working number; ADR 0011). Every call on
    /// the returned `Host` acts for the module: Anvil suspends its changes by
    /// the module's own `affects_gameplay`, `allow_ironman` and
    /// `allow_multiplayer`, `anvil.suspended` reaches what it subscribes, and a
    /// service that checks its caller sees the module. Settings, symbols,
    /// folders and the names of services and events stay the plugin's.
    ///
    /// [`ErrorCode::Disabled`](crate::ErrorCode::Disabled) when the player
    /// turned the module off (its code should not run then),
    /// [`ErrorCode::NotDeclared`](crate::ErrorCode::NotDeclared) for a name
    /// `plugin.toml` does not declare or when `self` is a module already, and
    /// [`ErrorCode::Unsupported`](crate::ErrorCode::Unsupported) from an Anvil
    /// without modules.
    pub fn module(&self, name: &str) -> Result<Host> {
        let provided = self.table.size as usize
            >= offset_of!(AnvilHost, module) + size_of::<Option<unsafe extern "C" fn()>>();
        let module = need(if provided { self.table.module } else { None }, "module")?;
        let text = c_string(name);
        let mut out: *mut AnvilPlugin = ptr::null_mut();
        // SAFETY: the host's function with this plugin, a NUL-terminated name
        // and a place for the handle; the table is long enough (checked above).
        let result = unsafe { module(self.plugin, text.as_ptr(), &mut out) };
        check(result, || format!("module {name}"))?;
        Ok(Self {
            table: self.table,
            plugin: out,
        })
    }

    /// Anvil's version (SemVer).
    pub fn anvil_version(&self) -> &'static str {
        // SAFETY: the host fills it for the process.
        unsafe { static_str(self.table.anvil_version) }.unwrap_or("")
    }

    /// The running game executable.
    pub fn game(&self) -> GameInfo {
        // SAFETY: the host's game info lives for the process.
        match unsafe { self.table.game.as_ref() } {
            Some(game) => GameInfo {
                // SAFETY: as above; build_id and sha256 may be null.
                build_id: unsafe { static_str(game.build_id) },
                image_base: game.image_base,
                image_size: game.image_size,
                // SAFETY: as above.
                sha256: unsafe { static_str(game.sha256) },
            },
            None => GameInfo {
                build_id: None,
                image_base: 0,
                image_size: 0,
                sha256: None,
            },
        }
    }

    /// The plugin's folder (read-only).
    pub fn plugin_dir(&self) -> PathBuf {
        // SAFETY: the host's function with this plugin.
        self.table
            .plugin_dir
            .map_or_else(PathBuf::new, |f| wide_path(unsafe { f(self.plugin) }))
    }

    /// The plugin's writable `anvil/data/<id>/`, created on the first call.
    pub fn data_dir(&self) -> PathBuf {
        // SAFETY: the host's function with this plugin.
        self.table
            .data_dir
            .map_or_else(PathBuf::new, |f| wide_path(unsafe { f(self.plugin) }))
    }

    // --- log -------------------------------------------------------------------

    /// One line of `anvil.log`, under the plugin's id: `host.log(level, "text")`
    /// or `host.log(level, format_args!(…))`.
    pub fn log(&self, level: LogLevel, message: impl Display) {
        write_log(self.table, self.plugin, level, &message);
    }

    /// A debug line.
    pub fn debug(&self, message: impl Display) {
        self.log(LogLevel::Debug, message);
    }

    /// An information line.
    pub fn info(&self, message: impl Display) {
        self.log(LogLevel::Info, message);
    }

    /// A warning.
    pub fn warn(&self, message: impl Display) {
        self.log(LogLevel::Warning, message);
    }

    /// An error line.
    pub fn error(&self, message: impl Display) {
        self.log(LogLevel::Error, message);
    }

    // --- changes on declared symbols ------------------------------------------------

    /// Hooks a function symbol: the game calls `detour`, which passes on
    /// through `hook.next()`. `F` is the symbol's generated type
    /// (`sym::<Name>Fn`), so a detour of another shape does not compile.
    ///
    /// During `load` the change is staged and enabled with the others when
    /// load returns; later it is enabled at once (or with its group).
    pub fn hook<F: FnPtr>(
        &self,
        hook: &'static FunctionHook<F>,
        symbol: AnvilSymbol,
        detour: F,
    ) -> Result<()> {
        hook.claim("hook_function")?;
        let call = need(self.table.hook_function, "hook_function")?;
        let mut change: *mut AnvilChange = ptr::null_mut();
        // SAFETY: a declared symbol; `next` lives in a static; the detour has the declared type.
        let result = unsafe {
            call(
                self.plugin,
                symbol,
                detour.into_raw(),
                hook.next_slot(),
                &mut change,
            )
        };
        check(result, || format!("hook_function symbol {symbol}"))?;
        hook.set_change(change);
        Ok(())
    }

    /// A mid hook: `callback` runs when the game reaches the site (a function
    /// or site symbol with at least 5 declared bytes), before the code there.
    /// It may run on several game threads at once. A panic in it is logged
    /// once and the game carries on.
    pub fn mid_hook<F>(&self, site: AnvilSymbol, callback: F) -> Result<Change>
    where
        F: Fn(&mut Cpu) + Send + Sync + 'static,
    {
        let call = need(self.table.mid_hook, "mid_hook")?;
        let what = format!("mid_hook symbol {site}");
        self.cpu_change(what, callback, |function, user, out| {
            // SAFETY: a declared symbol; the adapter is never freed and never unwinds.
            unsafe { call(self.plugin, site, function, user, out) }
        })
    }

    /// A block replacement: the symbol's declared bytes never run; `callback`
    /// runs instead and the game resumes at the block's `resume` or end
    /// (unless the callback writes `rip` with [`Cpu::regs_mut`]).
    pub fn replace_block<F>(&self, block: AnvilSymbol, callback: F) -> Result<Change>
    where
        F: Fn(&mut Cpu) + Send + Sync + 'static,
    {
        let call = need(self.table.replace_block, "replace_block")?;
        let what = format!("replace_block symbol {block}");
        self.cpu_change(what, callback, |function, user, out| {
            // SAFETY: as in mid_hook.
            unsafe { call(self.plugin, block, function, user, out) }
        })
    }

    /// A call redirect at a `call rel32` site symbol: `target` gets the call,
    /// `hook.next()` is the previous target.
    ///
    /// # Safety
    /// A call site declares no signature: `F` must be the type of the
    /// function the site calls.
    pub unsafe fn redirect_call<F: FnPtr>(
        &self,
        hook: &'static FunctionHook<F>,
        call_site: AnvilSymbol,
        target: F,
    ) -> Result<()> {
        hook.claim("redirect_call")?;
        let call = need(self.table.redirect_call, "redirect_call")?;
        let mut change: *mut AnvilChange = ptr::null_mut();
        // SAFETY: a declared symbol; the caller vouches for the type.
        let result = unsafe {
            call(
                self.plugin,
                call_site,
                target.into_raw(),
                hook.next_slot(),
                &mut change,
            )
        };
        check(result, || format!("redirect_call symbol {call_site}"))?;
        hook.set_change(change);
        Ok(())
    }

    /// A byte patch within the symbol's declared bytes.
    ///
    /// # Safety
    /// The bytes become the game's code or data at the site: they must be
    /// what the game can run or read there.
    pub unsafe fn patch_bytes(&self, site: AnvilSymbol, bytes: &[u8]) -> Result<Change> {
        let call = need(self.table.patch_bytes, "patch_bytes")?;
        let mut change: *mut AnvilChange = ptr::null_mut();
        // SAFETY: Anvil copies the bytes during the call; the caller vouches for them.
        let result = unsafe {
            call(
                self.plugin,
                site,
                bytes.as_ptr().cast(),
                bytes.len(),
                &mut change,
            )
        };
        check(result, || format!("patch_bytes symbol {site}"))?;
        Ok(Change::from_raw(change))
    }

    /// A pointer swap on a slot symbol (`points_to`): the slot holds `value`,
    /// `swap.previous()` the rest of the chain.
    ///
    /// # Safety
    /// The game uses `value` as the slot's pointer: it must be what the slot
    /// points to (a function of the vtable entry's type, an object), for as
    /// long as the change lives.
    pub unsafe fn swap_pointer(
        &self,
        swap: &'static PointerSwap,
        slot: AnvilSymbol,
        value: *mut c_void,
    ) -> Result<()> {
        swap.claim()?;
        let call = need(self.table.swap_pointer, "swap_pointer")?;
        let mut change: *mut AnvilChange = ptr::null_mut();
        // SAFETY: `previous` lives in a static and is 8-byte aligned; the caller vouches for `value`.
        let result = unsafe { call(self.plugin, slot, value, swap.previous_slot(), &mut change) };
        check(result, || format!("swap_pointer symbol {slot}"))?;
        swap.set_change(change);
        Ok(())
    }

    /// Replaces an import of eu4.exe, e.g. `("KERNEL32.dll", "GetTickCount")`
    /// (the module ignores case).
    ///
    /// # Safety
    /// `F` must be the type of the imported function.
    pub unsafe fn replace_import<F: FnPtr>(
        &self,
        hook: &'static FunctionHook<F>,
        module: &str,
        function: &str,
        replacement: F,
    ) -> Result<()> {
        hook.claim("replace_import")?;
        let call = need(self.table.replace_import, "replace_import")?;
        let (module_c, function_c) = (c_string(module), c_string(function));
        let mut change: *mut AnvilChange = ptr::null_mut();
        // SAFETY: NUL-terminated names; the caller vouches for the type.
        let result = unsafe {
            call(
                self.plugin,
                module_c.as_ptr(),
                function_c.as_ptr(),
                replacement.into_raw(),
                hook.next_slot(),
                &mut change,
            )
        };
        check(result, || format!("replace_import {module}!{function}"))?;
        hook.set_change(change);
        Ok(())
    }

    /// A function hook at a raw address outside the eu4.exe image (e.g. a
    /// Direct3D 9 vtable entry); `expected_prologue` may be empty.
    ///
    /// # Safety
    /// `address` is the start of a function of type `F` that lives while the
    /// change does.
    pub unsafe fn hook_at<F: FnPtr>(
        &self,
        hook: &'static FunctionHook<F>,
        address: NonNull<c_void>,
        expected_prologue: &[u8],
        detour: F,
    ) -> Result<()> {
        hook.claim("hook_function_at")?;
        let call = need(self.table.hook_function_at, "hook_function_at")?;
        let mut change: *mut AnvilChange = ptr::null_mut();
        // SAFETY: the caller's contract; the bytes are read during the call.
        let result = unsafe {
            call(
                self.plugin,
                address.as_ptr(),
                bytes_or_null(expected_prologue),
                expected_prologue.len(),
                detour.into_raw(),
                hook.next_slot(),
                &mut change,
            )
        };
        check(result, || {
            format!("hook_function_at {:p}", address.as_ptr())
        })?;
        hook.set_change(change);
        Ok(())
    }

    /// A mid hook at a raw address outside the eu4.exe image; `expected` may
    /// be empty.
    ///
    /// # Safety
    /// `address` is the start of an instruction that lives while the change does.
    pub unsafe fn mid_hook_at<F>(
        &self,
        address: NonNull<c_void>,
        expected: &[u8],
        callback: F,
    ) -> Result<Change>
    where
        F: Fn(&mut Cpu) + Send + Sync + 'static,
    {
        let call = need(self.table.mid_hook_at, "mid_hook_at")?;
        let what = format!("mid_hook_at {:p}", address.as_ptr());
        self.cpu_change(what, callback, |function, user, out| {
            // SAFETY: the caller's contract; the adapter never unwinds.
            unsafe {
                call(
                    self.plugin,
                    address.as_ptr(),
                    bytes_or_null(expected),
                    expected.len(),
                    function,
                    user,
                    out,
                )
            }
        })
    }

    /// After load: the changes `body` requests are enabled together, or none
    /// when it returns an error or panics (the panic goes on). Inside `load`
    /// the load itself is the group: `WrongPhase`.
    pub fn group<R>(&self, body: impl FnOnce() -> Result<R>) -> Result<R> {
        let begin = need(self.table.begin_group, "begin_group")?;
        let commit = need(self.table.commit_group, "commit_group")?;
        let abort = need(self.table.abort_group, "abort_group")?;
        // SAFETY: the host's functions with this plugin.
        check(unsafe { begin(self.plugin) }, || "begin_group")?;
        match catch_raw(body) {
            Ok(Ok(value)) => {
                // SAFETY: as above.
                check(unsafe { commit(self.plugin) }, || "commit_group")?;
                Ok(value)
            }
            Ok(Err(error)) => {
                // SAFETY: as above.
                unsafe { abort(self.plugin) };
                Err(error)
            }
            Err(panic) => {
                // SAFETY: as above.
                unsafe { abort(self.plugin) };
                std::panic::resume_unwind(panic)
            }
        }
    }

    fn cpu_change<F>(
        &self,
        what: String,
        callback: F,
        request: impl FnOnce(AnvilCpuCallback, *mut c_void, *mut *mut AnvilChange) -> AnvilResult,
    ) -> Result<Change>
    where
        F: Fn(&mut Cpu) + Send + Sync + 'static,
    {
        let adapter: &'static CpuAdapter<F> = Box::leak(Box::new(CpuAdapter {
            callback,
            what,
            panicked: AtomicBool::new(false),
        }));
        let mut change: *mut AnvilChange = ptr::null_mut();
        let result = request(
            Some(cpu_adapter::<F>),
            ptr::from_ref(adapter).cast_mut().cast(),
            &mut change,
        );
        check(result, || adapter.what.clone())?;
        Ok(Change::from_raw(change))
    }

    // --- addresses -------------------------------------------------------------------

    /// The verified address of a symbol; `None` for an unknown one, or one
    /// symbols.toml declares `absent` on the running game build.
    pub fn address(&self, symbol: AnvilSymbol) -> Option<NonNull<c_void>> {
        // SAFETY: the host's function with this plugin.
        self.table
            .address_of
            .and_then(|f| NonNull::new(unsafe { f(self.plugin, symbol) }))
    }

    /// Whether the symbol is there on the running game build: check it before
    /// using a symbol symbols.toml may declare `absent`.
    pub fn present(&self, symbol: AnvilSymbol) -> bool {
        self.address(symbol).is_some()
    }

    /// The game function of a function symbol, to call.
    ///
    /// # Safety
    /// `F` is the symbol's generated type (`sym::<Name>Fn`).
    pub unsafe fn function<F: FnPtr>(&self, symbol: AnvilSymbol) -> Option<F> {
        // SAFETY: a verified function of the caller's type.
        self.address(symbol)
            .map(|address| unsafe { F::from_raw(address.as_ptr()) })
    }

    /// The game's data at a data symbol, for the life of the process.
    ///
    /// # Safety
    /// The symbol's memory holds a `T`, and nothing writes it while the
    /// reference is used (data the game never changes, or read only on the
    /// thread that writes it).
    pub unsafe fn data<T>(&self, symbol: AnvilSymbol) -> Option<&'static T> {
        // SAFETY: the caller's contract.
        self.address(symbol)
            .map(|address| unsafe { address.cast::<T>().as_ref() })
    }

    /// A value of symbols.toml `values` (a `rip_relative` value is an RVA).
    pub fn value(&self, symbol: AnvilSymbol, name: &str) -> Result<u64> {
        let call = need(self.table.symbol_value, "symbol_value")?;
        let name_c = c_string(name);
        let mut out = 0u64;
        // SAFETY: the host's function with a NUL-terminated name.
        let result = unsafe { call(self.plugin, symbol, name_c.as_ptr(), &mut out) };
        check(result, || format!("symbol_value symbol {symbol}.{name}"))?;
        Ok(out)
    }

    // --- settings (settings.gen.rs) -----------------------------------------------------

    /// A bool setting (false for an unknown one or one of another type).
    pub fn setting_bool(&self, setting: AnvilSetting) -> bool {
        // SAFETY: the host's function with this plugin.
        self.table
            .setting_bool
            .is_some_and(|f| unsafe { f(self.plugin, setting) } != 0)
    }

    /// An int, bool, enum index (`<NAME>_<OPTION>`) or key code.
    pub fn setting_int(&self, setting: AnvilSetting) -> i64 {
        // SAFETY: as above.
        self.table
            .setting_int
            .map_or(0, |f| unsafe { f(self.plugin, setting) })
    }

    /// A float or int setting.
    pub fn setting_float(&self, setting: AnvilSetting) -> f64 {
        // SAFETY: as above.
        self.table
            .setting_float
            .map_or(0.0, |f| unsafe { f(self.plugin, setting) })
    }

    /// A string, an enum option's name or a key ("Ctrl+F12").
    pub fn setting_string(&self, setting: AnvilSetting) -> String {
        // SAFETY: valid until the next change of the setting; copied at once.
        self.table
            .setting_string
            .map_or_else(String::new, |f| unsafe {
                owned_string(f(self.plugin, setting))
            })
    }

    /// A key setting.
    pub fn setting_key(&self, setting: AnvilSetting) -> Key {
        Key(self.setting_int(setting))
    }

    /// `handler` runs after a `restart = false` setting changes (in the
    /// overlay), on the thread that changed it; kept for the process.
    pub fn subscribe_setting<F>(&self, setting: AnvilSetting, handler: F) -> Result<()>
    where
        F: Fn() + Send + Sync + 'static,
    {
        let call = need(self.table.subscribe_setting, "subscribe_setting")?;
        let what = format!("setting {setting} handler");
        let adapter: &'static SettingAdapter<F> = Box::leak(Box::new(SettingAdapter {
            handler,
            what,
            panicked: AtomicBool::new(false),
        }));
        // SAFETY: the adapter is never freed and never unwinds.
        let result = unsafe {
            call(
                self.plugin,
                setting,
                Some(setting_adapter::<F>),
                ptr::from_ref(adapter).cast_mut().cast(),
            )
        };
        check(result, || format!("subscribe_setting setting {setting}"))
    }

    // --- events ----------------------------------------------------------------------

    /// `handler(&payload)` runs for each `event`, synchronously on the
    /// publishing thread; kept for the process.
    pub fn subscribe<P, F>(&self, event: Event<P>, handler: F) -> Result<()>
    where
        P: Payload,
        F: Fn(&P) + Send + Sync + 'static,
    {
        let call = need(self.table.subscribe, "subscribe")?;
        let name = event.name();
        let what = format!("{} handler", name.to_string_lossy());
        let adapter: &'static EventAdapter<P, F> = Box::leak(Box::new(EventAdapter {
            handler,
            what,
            panicked: AtomicBool::new(false),
            _payload: std::marker::PhantomData,
        }));
        // SAFETY: a NUL-terminated name; the adapter is never freed and never unwinds.
        let result = unsafe {
            call(
                self.plugin,
                name.as_ptr(),
                Some(event_adapter::<P, F>),
                ptr::from_ref(adapter).cast_mut().cast(),
            )
        };
        check(result, || format!("subscribe {}", name.to_string_lossy()))
    }

    /// `handler(name, payload)` for any event name, with the publisher's raw
    /// payload pointer (valid during the handler).
    pub fn subscribe_raw<F>(&self, event: &str, handler: F) -> Result<()>
    where
        F: Fn(&str, *const c_void) + Send + Sync + 'static,
    {
        let call = need(self.table.subscribe, "subscribe")?;
        let name = c_string(event);
        let what = format!("{event} handler");
        let adapter: &'static RawEventAdapter<F> = Box::leak(Box::new(RawEventAdapter {
            handler,
            what,
            panicked: AtomicBool::new(false),
        }));
        // SAFETY: as in subscribe.
        let result = unsafe {
            call(
                self.plugin,
                name.as_ptr(),
                Some(raw_event_adapter::<F>),
                ptr::from_ref(adapter).cast_mut().cast(),
            )
        };
        check(result, || format!("subscribe {event}"))
    }

    /// Publishes `<plugin-id>.<name>` with no payload.
    pub fn publish(&self, event: &str) -> Result<()> {
        self.publish_raw(event, ptr::null())
    }

    /// Publishes `<plugin-id>.<name>` with a payload its subscribers read
    /// during the call.
    // The pointer is only handed on; reading it is the subscribers' business.
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn publish_raw(&self, event: &str, payload: *const c_void) -> Result<()> {
        let call = need(self.table.publish, "publish")?;
        let name = c_string(event);
        // SAFETY: a NUL-terminated name; the payload is the subscribers' business.
        check(unsafe { call(self.plugin, name.as_ptr(), payload) }, || {
            format!("publish {event}")
        })
    }

    // --- services ----------------------------------------------------------------------

    /// Registers a service `<plugin-id>.<service>` of major `version`.
    ///
    /// # Safety
    /// `service` is a `#[repr(C)]` struct whose first field is `u32 size`
    /// (its size), followed by `extern "C"` functions that never unwind, as
    /// the service's C header describes it.
    pub unsafe fn register_service<T: Sync>(
        &self,
        name: &str,
        version: u32,
        service: &'static T,
    ) -> Result<()> {
        let call = need(self.table.register_service, "register_service")?;
        let name_c = c_string(name);
        // SAFETY: the service lives for the process; the caller vouches for its layout.
        let result = unsafe {
            call(
                self.plugin,
                name_c.as_ptr(),
                version,
                ptr::from_ref(service).cast(),
            )
        };
        check(result, || format!("register_service {name} v{version}"))
    }

    /// A service by name and major version, as raw table; the typed wrappers
    /// ([`game::Game`](crate::game::Game), [`world::World`](crate::world::World),
    /// [`panel::Panels`](crate::panel::Panels),
    /// [`benchmark::Benchmark`](crate::benchmark::Benchmark)) use it.
    ///
    /// # Safety
    /// `T` is the service's table type for that version. Check its `size`
    /// before using a function appended after the first release of the version.
    pub unsafe fn service<T>(&self, name: &CStr, version: u32) -> Option<&'static T> {
        let call = self.table.get_service?;
        // SAFETY: a NUL-terminated name; a service lives for the process.
        unsafe {
            call(self.plugin, name.as_ptr(), version)
                .cast::<T>()
                .as_ref()
        }
    }
}

fn bytes_or_null(bytes: &[u8]) -> *const c_void {
    if bytes.is_empty() {
        ptr::null()
    } else {
        bytes.as_ptr().cast()
    }
}
