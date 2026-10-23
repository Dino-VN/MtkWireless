//! Runtime for Rust code linked into a macOS kext.
//!
//! On the kext target (`cfg(kext)`, set by build.rs for
//! `targets/x86_64-apple-kext.json`) this crate provides the global allocator (IOMalloc), the panic handler (the
//! kernel's `panic()`) and the default log sink (IOLog). On the host it only
//! provides [`log`], so the other crates can be unit-tested with `cargo test`.
#![cfg_attr(not(test), no_std)]

pub mod log;

#[cfg(kext)]
mod rt;
