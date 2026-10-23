//! Formatted logging without allocation.
//!
//! Messages are formatted into a fixed stack buffer (truncated if too long)
//! and handed to a sink as a NUL-terminated string. The C++ side installs its
//! own sink (IOLog plus the kext's ring buffer) with `mtk_rust_set_log`.

use core::ffi::c_char;
use core::fmt;
use core::sync::atomic::{AtomicPtr, Ordering};

/// Receives one NUL-terminated message without a trailing newline.
pub type LogSink = unsafe extern "C" fn(msg: *const c_char);

static SINK: AtomicPtr<()> = AtomicPtr::new(core::ptr::null_mut());

pub fn set_sink(sink: Option<LogSink>) {
    let raw = sink.map_or(core::ptr::null_mut(), |f| f as *mut ());
    SINK.store(raw, Ordering::Release);
}

/// Formatting target for a single log line. Kept small: kernel stacks are 16 KB.
pub struct LineBuf {
    buf: [u8; Self::CAPACITY],
    len: usize,
}

impl LineBuf {
    pub const CAPACITY: usize = 256;

    pub const fn new() -> Self {
        Self {
            buf: [0; Self::CAPACITY],
            len: 0,
        }
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }

    /// Pointer to the NUL-terminated contents.
    pub fn as_c_ptr(&mut self) -> *const c_char {
        self.buf[self.len] = 0;
        self.buf.as_ptr().cast()
    }
}

impl Default for LineBuf {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Write for LineBuf {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        // Keep one byte for the terminator; drop what does not fit.
        let room = Self::CAPACITY - 1 - self.len;
        let n = s.len().min(room);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

pub fn write_fmt(args: fmt::Arguments<'_>) {
    let mut line = LineBuf::new();
    let _ = fmt::Write::write_fmt(&mut line, args);
    let msg = line.as_c_ptr();
    let raw = SINK.load(Ordering::Acquire);
    if raw.is_null() {
        default_sink(msg);
    } else {
        // SAFETY: only `set_sink` stores into SINK, always from a `LogSink`.
        let sink: LogSink = unsafe { core::mem::transmute(raw) };
        unsafe { sink(msg) };
    }
}

#[cfg(kext)]
fn default_sink(msg: *const c_char) {
    unsafe extern "C" {
        fn IOLog(format: *const c_char, ...);
    }
    unsafe { IOLog(c"MtkRust: %s\n".as_ptr(), msg) };
}

#[cfg(not(kext))]
fn default_sink(_msg: *const c_char) {}

/// `klog!("fmt", args...)`: format and send one line to the log sink.
#[macro_export]
macro_rules! klog {
    ($($arg:tt)*) => {
        $crate::log::write_fmt(::core::format_args!($($arg)*))
    };
}

#[cfg(test)]
mod tests {
    use super::LineBuf;
    use core::fmt::Write;

    #[test]
    fn truncates_and_terminates() {
        let mut line = LineBuf::new();
        for _ in 0..100 {
            write!(line, "abcdef").unwrap();
        }
        assert_eq!(line.as_bytes().len(), LineBuf::CAPACITY - 1);
        let p = line.as_c_ptr();
        assert_eq!(unsafe { *p.add(LineBuf::CAPACITY - 1) }, 0);
    }
}
