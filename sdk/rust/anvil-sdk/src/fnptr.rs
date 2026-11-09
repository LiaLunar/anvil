use std::ffi::c_void;

/// A function pointer type the game can call: `unsafe extern "C" fn(…) -> R`
/// with up to 12 arguments, as `anvil gen --lang rust` writes them
/// (`sym::<Name>Fn`).
///
/// Only `unsafe` function pointers implement it, so calling a hook's
/// [`next`](crate::FunctionHook::next) or a game function is always an
/// `unsafe` call. Variadic functions are not covered.
///
/// # Safety
/// An implementation must be a function pointer type, so that the raw
/// conversions keep its value.
pub unsafe trait FnPtr: Copy + Send + Sync + 'static {
    /// The pointer as Anvil takes it.
    fn into_raw(self) -> *mut c_void;

    /// The function pointer back from Anvil's pointer.
    ///
    /// # Safety
    /// `raw` is not null and points to a function of this type.
    unsafe fn from_raw(raw: *mut c_void) -> Self;
}

macro_rules! fn_ptr {
    ($($arg:ident),*) => {
        // SAFETY: a function pointer type.
        unsafe impl<R: 'static, $($arg: 'static),*> FnPtr for unsafe extern "C" fn($($arg),*) -> R {
            fn into_raw(self) -> *mut c_void {
                self as *const () as *mut c_void
            }

            unsafe fn from_raw(raw: *mut c_void) -> Self {
                // SAFETY: the caller's contract; both are one pointer wide.
                unsafe { std::mem::transmute_copy::<*mut c_void, Self>(&raw) }
            }
        }
    };
}

fn_ptr!();
fn_ptr!(A);
fn_ptr!(A, B);
fn_ptr!(A, B, C);
fn_ptr!(A, B, C, D);
fn_ptr!(A, B, C, D, E);
fn_ptr!(A, B, C, D, E, F);
fn_ptr!(A, B, C, D, E, F, G);
fn_ptr!(A, B, C, D, E, F, G, H);
fn_ptr!(A, B, C, D, E, F, G, H, I);
fn_ptr!(A, B, C, D, E, F, G, H, I, J);
fn_ptr!(A, B, C, D, E, F, G, H, I, J, K);
fn_ptr!(A, B, C, D, E, F, G, H, I, J, K, L);
