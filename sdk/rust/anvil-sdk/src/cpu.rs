use std::fmt;
use std::ops::Deref;

use crate::sys::AnvilCpuContext;

/// The registers at a mid hook or block replacement, as SafetyHook saves
/// them (`AnvilCpuContext`, 408 bytes). Reading is safe through `Deref`
/// (`cpu.rcx`); writing is [`regs_mut`](Cpu::regs_mut). Valid only during the
/// callback.
#[repr(transparent)]
pub struct Cpu(AnvilCpuContext);

impl Cpu {
    /// The context of a callback.
    ///
    /// # Safety
    /// `raw` is the context Anvil passed to the callback that is running.
    pub(crate) unsafe fn from_raw<'a>(raw: *mut AnvilCpuContext) -> Option<&'a mut Cpu> {
        // SAFETY: Cpu is a transparent AnvilCpuContext; the caller's contract.
        unsafe { raw.cast::<Cpu>().as_mut() }
    }

    /// The registers, writable: the game continues with what the callback
    /// leaves in them. In a mid hook a changed `rip` ends the chain there.
    ///
    /// # Safety
    /// The game's code at the site must work with the values written: a
    /// pointer it dereferences, a stack it returns through. `rsp` and
    /// `reserved` must not change.
    pub unsafe fn regs_mut(&mut self) -> &mut AnvilCpuContext {
        &mut self.0
    }

    /// XMM register `index` (0-15) as bytes.
    pub fn xmm_bytes(&self, index: usize) -> [u8; 16] {
        // SAFETY: every bit pattern is a valid [u8; 16].
        unsafe { self.0.xmm[index].u8_ }
    }

    /// XMM register `index` (0-15) as four `f32`.
    pub fn xmm_f32(&self, index: usize) -> [f32; 4] {
        // SAFETY: every bit pattern is a valid f32.
        unsafe { self.0.xmm[index].f32_ }
    }

    /// XMM register `index` (0-15) as two `f64` (`xmm0` holds a `double`
    /// argument or result in the low one).
    pub fn xmm_f64(&self, index: usize) -> [f64; 2] {
        // SAFETY: every bit pattern is a valid f64.
        unsafe { self.0.xmm[index].f64_ }
    }
}

impl Deref for Cpu {
    type Target = AnvilCpuContext;

    fn deref(&self) -> &AnvilCpuContext {
        &self.0
    }
}

impl fmt::Debug for Cpu {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let c = &self.0;
        f.debug_struct("Cpu")
            .field("rax", &format_args!("{:#x}", c.rax))
            .field("rcx", &format_args!("{:#x}", c.rcx))
            .field("rdx", &format_args!("{:#x}", c.rdx))
            .field("r8", &format_args!("{:#x}", c.r8))
            .field("r9", &format_args!("{:#x}", c.r9))
            .field("rsp", &format_args!("{:#x}", c.rsp))
            .field("rip", &format_args!("{:#x}", c.rip))
            .finish_non_exhaustive()
    }
}
