use std::ffi::c_void;
use std::fmt;
use std::marker::PhantomData;
use std::ptr::{self, NonNull};
use std::sync::atomic::{AtomicPtr, Ordering};

use crate::error::{check, Error, ErrorCode, Result};
use crate::fnptr::FnPtr;
use crate::host::Host;
use crate::sys::AnvilChange;

/// A change's handle: dropping it removes the change; [`keep`](Change::keep)
/// keeps the change for the rest of the process.
///
/// Anvil never frees its own stubs, so a game thread still inside a removed
/// change finishes safely; what the change runs (an adapter of this crate)
/// is never freed either.
#[must_use = "dropping a Change removes the change; keep it in the plugin, or call keep()"]
pub struct Change {
    handle: Option<NonNull<AnvilChange>>,
}

// SAFETY: the handle is an opaque id; Anvil's remove may be called from any thread.
unsafe impl Send for Change {}
// SAFETY: as above; &Change only reads the id.
unsafe impl Sync for Change {}

impl Change {
    pub(crate) fn from_raw(handle: *mut AnvilChange) -> Self {
        Self {
            handle: NonNull::new(handle),
        }
    }

    /// The raw handle, for `AnvilHost::remove`.
    pub fn as_raw(&self) -> *mut AnvilChange {
        self.handle.map_or(ptr::null_mut(), NonNull::as_ptr)
    }

    /// Removes the change now. A change that is already gone (e.g. rolled
    /// back after a failed load) is no error.
    pub fn remove(mut self) -> Result<()> {
        remove_handle(self.handle.take())
    }

    /// Keeps the change for the rest of the process.
    pub fn keep(mut self) {
        self.handle = None;
    }
}

impl Drop for Change {
    fn drop(&mut self) {
        let _ = remove_handle(self.handle.take());
    }
}

impl fmt::Debug for Change {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("Change").field(&self.as_raw()).finish()
    }
}

fn remove_handle(handle: Option<NonNull<AnvilChange>>) -> Result<()> {
    let (Some(handle), Some(host)) = (handle, Host::current()) else {
        return Ok(());
    };
    let Some(remove) = host.c_api().remove else {
        return Ok(());
    };
    // SAFETY: a handle Anvil gave this plugin; Anvil answers NOT_FOUND for one that is gone.
    let result = unsafe { remove(host.raw_plugin(), handle.as_ptr()) };
    match check(result, || "remove") {
        Err(error) if error.code() == ErrorCode::NotFound => Ok(()),
        other => other,
    }
}

/// Takes the change out of `slot` and removes it.
fn remove_slot(slot: &AtomicPtr<AnvilChange>) -> Result<()> {
    remove_handle(NonNull::new(slot.swap(ptr::null_mut(), Ordering::AcqRel)))
}

/// A function hook, call redirect or import replacement, and the rest of its
/// chain. It lives in a `static`, so the detour (a plain function the game
/// calls) reaches `next`, and Anvil can write `next` into it before the change
/// is enabled:
///
/// ```ignore
/// static SPEED_OBJECT: anvil::FunctionHook<sym::SpeedObjectFn> = anvil::FunctionHook::new();
///
/// unsafe extern "C" fn on_speed_object(session: *mut c_void) -> *mut c_void {
///     let Some(next) = SPEED_OBJECT.next() else { return std::ptr::null_mut() };
///     // SAFETY: the game's own function, called as it declares.
///     unsafe { next(session) }
/// }
/// ```
///
/// A detour is called by the game directly, with no adapter between: it must
/// not panic (a panic out of an `extern "C"` function aborts the process).
/// Wrap fallible work in [`catch_panic`](crate::catch_panic).
pub struct FunctionHook<F: FnPtr> {
    next: AtomicPtr<c_void>,
    change: AtomicPtr<AnvilChange>,
    _function: PhantomData<F>,
}

impl<F: FnPtr> FunctionHook<F> {
    /// An unused hook.
    pub const fn new() -> Self {
        Self {
            next: AtomicPtr::new(ptr::null_mut()),
            change: AtomicPtr::new(ptr::null_mut()),
            _function: PhantomData,
        }
    }

    /// The rest of the chain: the next plugin's detour or the game's code;
    /// `None` before Anvil wrote it. It stays valid after the change is
    /// removed, for a thread still inside the detour.
    pub fn next(&self) -> Option<F> {
        let raw = self.next.load(Ordering::Acquire);
        // SAFETY: Anvil wrote a function of the declared type (or nothing).
        (!raw.is_null()).then(|| unsafe { F::from_raw(raw) })
    }

    /// Whether the hook holds a change.
    pub fn is_active(&self) -> bool {
        !self.change.load(Ordering::Acquire).is_null()
    }

    /// Removes the change; nothing when there is none.
    pub fn remove(&self) -> Result<()> {
        remove_slot(&self.change)
    }

    pub(crate) fn claim(&self, call: &'static str) -> Result<()> {
        if self.is_active() {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                format!("{call}: the hook object already holds a change; remove() it first"),
            ));
        }
        Ok(())
    }

    pub(crate) fn next_slot(&self) -> *mut *mut c_void {
        self.next.as_ptr()
    }

    pub(crate) fn set_change(&self, change: *mut AnvilChange) {
        self.change.store(change, Ordering::Release);
    }
}

impl<F: FnPtr> Default for FunctionHook<F> {
    fn default() -> Self {
        Self::new()
    }
}

impl<F: FnPtr> fmt::Debug for FunctionHook<F> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("FunctionHook")
            .field("next", &self.next.load(Ordering::Relaxed))
            .field("active", &self.is_active())
            .finish()
    }
}

/// A pointer swap on a slot symbol, and the rest of its chain. Anvil rewrites
/// [`previous`](PointerSwap::previous) whenever the chain changes, so it lives
/// in a `static`.
pub struct PointerSwap {
    previous: AtomicPtr<c_void>,
    change: AtomicPtr<AnvilChange>,
}

impl PointerSwap {
    /// An unused swap.
    pub const fn new() -> Self {
        Self {
            previous: AtomicPtr::new(ptr::null_mut()),
            change: AtomicPtr::new(ptr::null_mut()),
        }
    }

    /// The rest of the chain, read on each use: another plugin's value or the
    /// slot's original pointer.
    pub fn previous(&self) -> *mut c_void {
        self.previous.load(Ordering::Acquire)
    }

    /// Whether the swap holds a change.
    pub fn is_active(&self) -> bool {
        !self.change.load(Ordering::Acquire).is_null()
    }

    /// Removes the change; nothing when there is none.
    pub fn remove(&self) -> Result<()> {
        remove_slot(&self.change)
    }

    pub(crate) fn claim(&self) -> Result<()> {
        if self.is_active() {
            return Err(Error::new(
                ErrorCode::InvalidArgument,
                "swap_pointer: the swap object already holds a change; remove() it first",
            ));
        }
        Ok(())
    }

    pub(crate) fn previous_slot(&self) -> *mut *mut c_void {
        self.previous.as_ptr()
    }

    pub(crate) fn set_change(&self, change: *mut AnvilChange) {
        self.change.store(change, Ordering::Release);
    }
}

impl Default for PointerSwap {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Debug for PointerSwap {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("PointerSwap")
            .field("previous", &self.previous())
            .field("active", &self.is_active())
            .finish()
    }
}
