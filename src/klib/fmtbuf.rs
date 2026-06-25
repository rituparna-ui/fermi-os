//! A `core::fmt::Write` adapter over a fixed byte buffer.
//!
//! Replaces the C `ksnprintf` pattern used by the `/proc` and `*_render_*`
//! functions: format into a caller-supplied `&mut [u8]`, truncating on
//! overflow, and report how many bytes were written.

use core::fmt;

pub struct FmtBuf<'a> {
    buf: &'a mut [u8],
    pos: usize,
}

impl<'a> FmtBuf<'a> {
    pub fn new(buf: &'a mut [u8]) -> Self {
        Self { buf, pos: 0 }
    }

    /// Number of bytes written so far.
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

impl fmt::Write for FmtBuf<'_> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let bytes = s.as_bytes();
        let space = self.buf.len() - self.pos;
        let n = core::cmp::min(space, bytes.len());
        self.buf[self.pos..self.pos + n].copy_from_slice(&bytes[..n]);
        self.pos += n;
        // Report success even on truncation: kernel renderers want best-effort
        // fill, matching the C ksnprintf-into-fixed-buffer behavior.
        Ok(())
    }
}
