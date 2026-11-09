use std::ffi::{c_char, CStr, CString};
use std::fmt;

use crate::error::{check, Result};
use crate::sys::{AnvilGameTag, AnvilResult};

/// `text` as a C string; a NUL inside it is dropped.
pub(crate) fn c_string(text: &str) -> CString {
    CString::new(text).unwrap_or_else(|error| {
        let mut bytes = error.into_vec();
        bytes.retain(|&byte| byte != 0);
        // SAFETY: the NULs were just removed.
        unsafe { CString::from_vec_unchecked(bytes) }
    })
}

/// A C string the host or a service keeps for the process.
///
/// # Safety
/// `text` is null or a NUL-terminated string that lives until the process ends.
pub(crate) unsafe fn static_str(text: *const c_char) -> Option<&'static str> {
    if text.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    unsafe { CStr::from_ptr(text) }.to_str().ok()
}

/// A C string as owned text (invalid UTF-8 replaced).
///
/// # Safety
/// `text` is null or NUL-terminated and valid during the call.
pub(crate) unsafe fn owned_string(text: *const c_char) -> String {
    if text.is_null() {
        return String::new();
    }
    // SAFETY: the caller's contract.
    unsafe { CStr::from_ptr(text) }
        .to_string_lossy()
        .into_owned()
}

/// Text from a call that fills the caller's buffer as far as it fits and
/// writes the whole length (without the NUL): asked with capacity 0 first.
pub(crate) fn read_text(
    context: impl Fn() -> String,
    mut call: impl FnMut(*mut c_char, u32, *mut u32) -> AnvilResult,
) -> Result<String> {
    let mut length = 0u32;
    check(call(std::ptr::null_mut(), 0, &mut length), &context)?;
    for _ in 0..4 {
        let mut buffer = vec![0u8; length as usize + 1];
        let capacity = u32::try_from(buffer.len()).unwrap_or(u32::MAX);
        let mut whole = 0u32;
        check(
            call(buffer.as_mut_ptr().cast(), capacity, &mut whole),
            &context,
        )?;
        if whole <= length {
            buffer.truncate(whole as usize);
            return Ok(String::from_utf8_lossy(&buffer).into_owned());
        }
        // It grew between the calls; ask again with the new size.
        length = whole;
    }
    Err(crate::Error::new(
        crate::ErrorCode::Internal,
        format!("{}: the text keeps growing", context()),
    ))
}

/// A list from a call that fills the caller's array as far as it fits and
/// writes the whole count: asked with capacity 0 first.
pub(crate) fn read_list<T: Copy>(
    empty: T,
    context: impl Fn() -> String,
    mut call: impl FnMut(*mut T, u32, *mut u32) -> AnvilResult,
) -> Result<Vec<T>> {
    let mut count = 0u32;
    check(call(std::ptr::null_mut(), 0, &mut count), &context)?;
    for _ in 0..4 {
        let mut items = vec![empty; count as usize];
        let mut whole = 0u32;
        check(call(items.as_mut_ptr(), count, &mut whole), &context)?;
        if whole <= count {
            items.truncate(whole as usize);
            return Ok(items);
        }
        count = whole;
    }
    Err(crate::Error::new(
        crate::ErrorCode::Internal,
        format!("{}: the list keeps growing", context()),
    ))
}

/// A country's tag: three ASCII letters or digits, e.g. `RUS`.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Tag([u8; 3]);

impl Tag {
    /// The tag of `text`, `None` unless it is three ASCII letters or digits.
    /// The world service ignores case; the tag keeps it as given.
    pub fn new(text: &str) -> Option<Self> {
        let bytes: [u8; 3] = text.as_bytes().try_into().ok()?;
        bytes
            .iter()
            .all(u8::is_ascii_alphanumeric)
            .then_some(Self(bytes))
    }

    /// The tag a service wrote; `None` for "" (no country).
    pub fn from_raw(raw: &AnvilGameTag) -> Option<Self> {
        let bytes = raw.text.map(|c| c as u8);
        if bytes[0] == 0 {
            return None;
        }
        let tag = [bytes[0], bytes[1], bytes[2]];
        // The game's tags are ASCII; anything else shows as '?'.
        Some(Self(
            tag.map(|b| if b.is_ascii_graphic() { b } else { b'?' }),
        ))
    }

    /// The tag as the C API takes it: three characters and a NUL.
    pub fn to_raw(self) -> AnvilGameTag {
        AnvilGameTag {
            text: [
                self.0[0] as c_char,
                self.0[1] as c_char,
                self.0[2] as c_char,
                0,
            ],
        }
    }

    pub(crate) fn c_text(self) -> [c_char; 4] {
        self.to_raw().text
    }

    /// The three characters.
    pub fn as_str(&self) -> &str {
        // Only ASCII is ever stored.
        std::str::from_utf8(&self.0).unwrap_or("???")
    }
}

impl fmt::Display for Tag {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl fmt::Debug for Tag {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Tag({})", self.as_str())
    }
}

impl std::str::FromStr for Tag {
    type Err = crate::Error;

    fn from_str(text: &str) -> Result<Self> {
        Self::new(text).ok_or_else(|| {
            crate::Error::new(
                crate::ErrorCode::InvalidArgument,
                format!("{text:?} is not a country tag"),
            )
        })
    }
}
