//! String / formatting helpers.
//!
//! In Rust the original `uart_printf` is replaced by `kprint!`/`kprintln!`
//! (core::fmt over the UART), and `memcpy/memmove/memset/memcmp` are provided
//! by `compiler_builtins`. This module supplies the remaining pieces the C
//! `strings` library offered: a fixed-buffer formatter (the `ksnprintf`
//! equivalent) and a few C-string helpers used where the kernel handles
//! null-terminated names (FAT32, paths, syscalls).

use core::fmt::{self, Write};

/// A `core::fmt::Write` sink over a fixed `&mut [u8]`. Truncates on overflow.
pub struct BufWriter<'a> {
    buf: &'a mut [u8],
    pos: usize,
}

impl<'a> BufWriter<'a> {
    pub fn new(buf: &'a mut [u8]) -> Self {
        Self { buf, pos: 0 }
    }
    /// Bytes written so far.
    pub fn len(&self) -> usize {
        self.pos
    }
    pub fn is_empty(&self) -> bool {
        self.pos == 0
    }
    /// The written portion as a byte slice.
    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.pos]
    }
}

impl<'a> Write for BufWriter<'a> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let bytes = s.as_bytes();
        let n = core::cmp::min(bytes.len(), self.buf.len() - self.pos);
        self.buf[self.pos..self.pos + n].copy_from_slice(&bytes[..n]);
        self.pos += n;
        if n < bytes.len() {
            Err(fmt::Error) // truncated
        } else {
            Ok(())
        }
    }
}

/// snprintf-style formatting into a fixed buffer. Returns bytes written
/// (may be truncated to `buf.len()`).
///
/// Usage: `let n = ksnprintf!(buf, "x={} y={:#x}", x, y);`
#[macro_export]
macro_rules! ksnprintf {
    ($buf:expr, $($arg:tt)*) => {{
        use core::fmt::Write as _;
        let mut w = $crate::strings::BufWriter::new($buf);
        let _ = w.write_fmt(format_args!($($arg)*));
        w.len()
    }};
}

/// Length of a NUL-terminated C string. Caller must ensure validity.
///
/// # Safety
/// `ptr` must point to a NUL-terminated, readable byte sequence.
pub unsafe fn cstr_len(ptr: *const u8) -> usize {
    let mut n = 0usize;
    while *ptr.add(n) != 0 {
        n += 1;
    }
    n
}

/// Borrow a NUL-terminated C string as a `&str` (lossy: stops at NUL, assumes
/// UTF-8/ASCII). Returns None if not valid UTF-8.
///
/// # Safety
/// `ptr` must point to a NUL-terminated, readable byte sequence.
pub unsafe fn cstr_as_str<'a>(ptr: *const u8) -> Option<&'a str> {
    let len = cstr_len(ptr);
    let slice = core::slice::from_raw_parts(ptr, len);
    core::str::from_utf8(slice).ok()
}

/// Compare a byte slice with a NUL-terminated C string for equality.
///
/// # Safety
/// `ptr` must point to a NUL-terminated, readable byte sequence.
pub unsafe fn cstr_eq_bytes(ptr: *const u8, bytes: &[u8]) -> bool {
    for (i, &b) in bytes.iter().enumerate() {
        if *ptr.add(i) != b {
            return false;
        }
    }
    *ptr.add(bytes.len()) == 0
}
