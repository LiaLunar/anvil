use std::borrow::Cow;
use std::fmt;

use crate::sys::{self, AnvilResult};

/// The result of a host or service call.
pub type Result<T, E = Error> = std::result::Result<T, E>;

/// Why Anvil or a service refused a call: an `AnvilResult` other than
/// `ANVIL_OK`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum ErrorCode {
    /// `ANVIL_ERROR_INVALID_ARGUMENT`.
    InvalidArgument,
    /// `ANVIL_ERROR_NOT_DECLARED`: an unknown symbol or setting, a raw pointer
    /// into eu4.exe, or a symbol the game build declares absent.
    NotDeclared,
    /// `ANVIL_ERROR_WRONG_KIND`: e.g. a pointer swap on a function symbol.
    WrongKind,
    /// `ANVIL_ERROR_CONFLICT`: overlaps another change at a different address.
    Conflict,
    /// `ANVIL_ERROR_UNSAFE_SITE`.
    UnsafeSite,
    /// `ANVIL_ERROR_BYTES_MISMATCH`.
    BytesMismatch,
    /// `ANVIL_ERROR_OUT_OF_MEMORY`.
    OutOfMemory,
    /// `ANVIL_ERROR_WRONG_PHASE`: e.g. a group inside `load`, or a world read
    /// off the game's main thread.
    WrongPhase,
    /// `ANVIL_ERROR_UNSUPPORTED`: also a function the running host or service
    /// is too old to have.
    Unsupported,
    /// `ANVIL_ERROR_INTERNAL`.
    Internal,
    /// `ANVIL_ERROR_NOT_FOUND`.
    NotFound,
    /// `ANVIL_ERROR_ALREADY_EXISTS`.
    AlreadyExists,
    /// `ANVIL_ERROR_SUSPENDED`: the caller is suspended in this session
    /// (ironman, multiplayer). Since Anvil 1.17.0.
    Suspended,
    /// `ANVIL_ERROR_LIMIT`: more than a service takes at once; nothing of the
    /// call was done. Since Anvil 1.17.0.
    Limit,
    /// `ANVIL_ERROR_DISABLED`: the player turned this module off
    /// ([`Host::module`](crate::Host::module)). Since Anvil 1.21.0 (working
    /// number; ADR 0011).
    Disabled,
    /// A result this crate does not know.
    Other(AnvilResult),
}

impl ErrorCode {
    /// The code of `result`; `None` for `ANVIL_OK`.
    pub const fn from_result(result: AnvilResult) -> Option<Self> {
        Some(match result {
            sys::ANVIL_OK => return None,
            sys::ANVIL_ERROR_INVALID_ARGUMENT => Self::InvalidArgument,
            sys::ANVIL_ERROR_NOT_DECLARED => Self::NotDeclared,
            sys::ANVIL_ERROR_WRONG_KIND => Self::WrongKind,
            sys::ANVIL_ERROR_CONFLICT => Self::Conflict,
            sys::ANVIL_ERROR_UNSAFE_SITE => Self::UnsafeSite,
            sys::ANVIL_ERROR_BYTES_MISMATCH => Self::BytesMismatch,
            sys::ANVIL_ERROR_OUT_OF_MEMORY => Self::OutOfMemory,
            sys::ANVIL_ERROR_WRONG_PHASE => Self::WrongPhase,
            sys::ANVIL_ERROR_UNSUPPORTED => Self::Unsupported,
            sys::ANVIL_ERROR_INTERNAL => Self::Internal,
            sys::ANVIL_ERROR_NOT_FOUND => Self::NotFound,
            sys::ANVIL_ERROR_ALREADY_EXISTS => Self::AlreadyExists,
            sys::ANVIL_ERROR_SUSPENDED => Self::Suspended,
            sys::ANVIL_ERROR_LIMIT => Self::Limit,
            sys::ANVIL_ERROR_DISABLED => Self::Disabled,
            other => Self::Other(other),
        })
    }

    /// The `AnvilResult` of the code, never `ANVIL_OK`.
    pub const fn result(self) -> AnvilResult {
        match self {
            Self::InvalidArgument => sys::ANVIL_ERROR_INVALID_ARGUMENT,
            Self::NotDeclared => sys::ANVIL_ERROR_NOT_DECLARED,
            Self::WrongKind => sys::ANVIL_ERROR_WRONG_KIND,
            Self::Conflict => sys::ANVIL_ERROR_CONFLICT,
            Self::UnsafeSite => sys::ANVIL_ERROR_UNSAFE_SITE,
            Self::BytesMismatch => sys::ANVIL_ERROR_BYTES_MISMATCH,
            Self::OutOfMemory => sys::ANVIL_ERROR_OUT_OF_MEMORY,
            Self::WrongPhase => sys::ANVIL_ERROR_WRONG_PHASE,
            Self::Unsupported => sys::ANVIL_ERROR_UNSUPPORTED,
            Self::Internal => sys::ANVIL_ERROR_INTERNAL,
            Self::NotFound => sys::ANVIL_ERROR_NOT_FOUND,
            Self::AlreadyExists => sys::ANVIL_ERROR_ALREADY_EXISTS,
            Self::Suspended => sys::ANVIL_ERROR_SUSPENDED,
            Self::Limit => sys::ANVIL_ERROR_LIMIT,
            Self::Disabled => sys::ANVIL_ERROR_DISABLED,
            Self::Other(sys::ANVIL_OK) => sys::ANVIL_ERROR_INTERNAL,
            Self::Other(other) => other,
        }
    }

    /// The words Anvil's log uses for it, as `anvil.hpp`'s `result_name`.
    pub const fn description(self) -> &'static str {
        match self {
            Self::InvalidArgument => "invalid argument",
            Self::NotDeclared => "not declared in symbols.toml or settings.toml",
            Self::WrongKind => "wrong kind of symbol or setting",
            Self::Conflict => "conflict with another change",
            Self::UnsafeSite => "unsafe site",
            Self::BytesMismatch => "bytes mismatch",
            Self::OutOfMemory => "out of memory",
            Self::WrongPhase => "wrong phase",
            Self::Unsupported => "unsupported",
            Self::Internal => "internal error",
            Self::NotFound => "not found",
            Self::AlreadyExists => "already exists",
            Self::Suspended => "suspended in this session",
            Self::Limit => "over the limit",
            Self::Disabled => "turned off by the player",
            Self::Other(_) => "unknown result",
        }
    }
}

impl fmt::Display for ErrorCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.description())
    }
}

/// A call that failed: its [`ErrorCode`] and what was called, e.g.
/// "hook_function symbol 0: conflict with another change".
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    code: ErrorCode,
    context: Cow<'static, str>,
}

impl Error {
    /// An error of the plugin's own, e.g. to refuse loading from
    /// [`Plugin::load`](crate::Plugin::load): `context` says why.
    pub fn new(code: ErrorCode, context: impl Into<Cow<'static, str>>) -> Self {
        Self {
            code,
            context: context.into(),
        }
    }

    /// The code, which becomes `anvil_plugin_load`'s result when `load` returns it.
    pub const fn code(&self) -> ErrorCode {
        self.code
    }

    /// What failed, without the code's words.
    pub fn context(&self) -> &str {
        &self.context
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.context.is_empty() {
            f.write_str(self.code.description())
        } else {
            write!(f, "{}: {}", self.context, self.code.description())
        }
    }
}

impl std::error::Error for Error {}

/// `Ok` for `ANVIL_OK`, else the error with `context()` as its context (only
/// built on failure).
pub(crate) fn check<C: Into<Cow<'static, str>>>(
    result: AnvilResult,
    context: impl FnOnce() -> C,
) -> Result<()> {
    match ErrorCode::from_result(result) {
        None => Ok(()),
        Some(code) => Err(Error::new(code, context())),
    }
}

/// The function `name` of a table, or `Unsupported` when it is missing.
pub(crate) fn need<F>(function: Option<F>, name: &'static str) -> Result<F> {
    function.ok_or_else(|| {
        Error::new(
            ErrorCode::Unsupported,
            format!("{name}: not provided by this version"),
        )
    })
}
