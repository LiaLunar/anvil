use std::ffi::{c_void, CStr};
use std::fmt;
use std::marker::PhantomData;
use std::mem::{size_of, MaybeUninit};

/// An event name with the type of its payload. [`Host::subscribe`] hands
/// the handler a `&P`; [`Host::subscribe_raw`] takes any name.
///
/// [`Host::subscribe`]: crate::Host::subscribe
/// [`Host::subscribe_raw`]: crate::Host::subscribe_raw
pub struct Event<P: Payload> {
    name: &'static CStr,
    _payload: PhantomData<fn() -> P>,
}

impl<P: Payload> Event<P> {
    /// The event `name`, whose publisher passes payloads that `P` reads.
    ///
    /// # Safety
    /// Every payload published under `name` is one [`Payload::read`] of `P`
    /// accepts.
    pub const unsafe fn new(name: &'static CStr) -> Self {
        Self {
            name,
            _payload: PhantomData,
        }
    }

    /// The name, e.g. `anvil-game.session`.
    pub const fn name(&self) -> &'static CStr {
        self.name
    }
}

impl<P: Payload> Clone for Event<P> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<P: Payload> Copy for Event<P> {}

impl<P: Payload> fmt::Debug for Event<P> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("Event").field(&self.name).finish()
    }
}

/// The payload of an event, read from the publisher's pointer during the
/// handler.
///
/// # Safety
/// `read` must accept exactly the payloads its events publish.
pub unsafe trait Payload: Sized + 'static {
    /// The payload behind `raw`; `None` when it cannot be read (e.g. null).
    ///
    /// # Safety
    /// `raw` is the payload of an event this type belongs to, valid during the call.
    unsafe fn read(raw: *const c_void) -> Option<Self>;
}

// SAFETY: the payload is not read.
unsafe impl Payload for () {
    unsafe fn read(_raw: *const c_void) -> Option<Self> {
        Some(())
    }
}

/// A struct that starts with `uint32_t size` (the size its publisher knows):
/// only that many bytes are read, the rest stays zero, so a payload of an
/// older publisher is never read past its end.
///
/// # Safety
/// `raw` is null or such a struct, valid during the call; `T` is plain data
/// for which all zero bytes are a valid value.
pub(crate) unsafe fn read_sized<T: Copy>(raw: *const c_void) -> Option<T> {
    if raw.is_null() {
        return None;
    }
    // SAFETY: the struct starts with its size, a u32.
    let size = unsafe { raw.cast::<u32>().read_unaligned() } as usize;
    let mut value = MaybeUninit::<T>::zeroed();
    // SAFETY: at most `size` bytes of the publisher's struct, into a zeroed T.
    unsafe {
        std::ptr::copy_nonoverlapping(
            raw.cast::<u8>(),
            value.as_mut_ptr().cast::<u8>(),
            size.min(size_of::<T>()),
        );
        Some(value.assume_init())
    }
}

/// `anvil.plugins_loaded`: every plugin is processed. Payload none.
// SAFETY: Anvil publishes it with a NULL payload.
pub const PLUGINS_LOADED: Event<()> = unsafe { Event::new(c"anvil.plugins_loaded") };
/// `anvil.suspended`: to the plugin itself, its changes are out of the game
/// (ironman, multiplayer). Payload none.
// SAFETY: as above.
pub const SUSPENDED: Event<()> = unsafe { Event::new(c"anvil.suspended") };
/// `anvil.resumed`: to the plugin itself, its changes are back in the game.
/// Payload none.
// SAFETY: as above.
pub const RESUMED: Event<()> = unsafe { Event::new(c"anvil.resumed") };
