//! Shared helpers for parsing data that comes from outside the kext:
//! firmware images, controller events, packets from user space. Every access
//! is bounds-checked and returns [`Error`] instead of reading past the end.
#![cfg_attr(not(test), no_std)]

pub mod bytes;

pub use bytes::{Reader, Writer};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// Needed `need` bytes at `offset`, but the buffer ends earlier.
    Truncated { offset: usize, need: usize },
    /// The output buffer is full.
    Overflow { need: usize, room: usize },
    /// Well-formed but unexpected content.
    Invalid(&'static str),
}

pub type Result<T> = core::result::Result<T, Error>;
