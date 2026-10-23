//! C ABI exported to the kext's C++ shell. The header is
//! `kext/include/mtk_rust.h`; keep both in sync.
#![cfg_attr(kext, no_std)]

#[cfg(all(feature = "wifi", feature = "bt"))]
compile_error!(
    "enable exactly one of `wifi` (GPL-2.0) or `bt` (GPL-3.0-only): they must not share a binary"
);

extern crate alloc;
// Links the allocator and panic handler on the kext target.
extern crate mtk_kernel;

#[cfg(feature = "bt")]
mod bt;
mod selftest;

use core::ffi::c_char;
use mtk_kernel::log::{self, LogSink};

#[cfg(feature = "wifi")]
const COMPONENT: &core::ffi::CStr = c"wifi";
#[cfg(feature = "bt")]
const COMPONENT: &core::ffi::CStr = c"bt";
#[cfg(not(any(feature = "wifi", feature = "bt")))]
const COMPONENT: &core::ffi::CStr = c"none";

/// Route Rust log lines to `sink` (NULL restores the IOLog default).
#[unsafe(no_mangle)]
pub extern "C" fn mtk_rust_set_log(sink: Option<LogSink>) {
    log::set_sink(sink);
}

/// Static string: "<crate version> <component>".
#[unsafe(no_mangle)]
pub extern "C" fn mtk_rust_version() -> *const c_char {
    const _: () = assert!(env!("CARGO_PKG_VERSION").len() < 16);
    static VERSION: [u8; 24] = {
        let v = env!("CARGO_PKG_VERSION").as_bytes();
        let c = COMPONENT.to_bytes();
        let mut out = [0u8; 24];
        let mut i = 0;
        while i < v.len() {
            out[i] = v[i];
            i += 1;
        }
        out[i] = b' ';
        let mut j = 0;
        while j < c.len() {
            out[i + 1 + j] = c[j];
            j += 1;
        }
        out
    };
    VERSION.as_ptr().cast()
}

/// Exercise the runtime (heap, aligned heap, formatting, 128-bit builtins,
/// bounds checks) and the component's tables. Returns a bitmask of failed
/// checks; 0 means everything passed.
#[unsafe(no_mangle)]
pub extern "C" fn mtk_rust_selftest() -> u32 {
    selftest::run()
}
