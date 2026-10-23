//! AWDL (Apple Wireless Direct Link): wire protocol, peer election and
//! clock, availability-window timeline and ROC scheduling.
//!
//! Ported from SimpleMtkWlan `include/AWDL/*.hpp` (GPL-2.0-or-later). The
//! structs are `#[repr(C)]` with the same layout as the C++ declarations in
//! `kext/Wifi/vendor/include/AWDL`, which keep their data members and call
//! these functions through `mtk-ffi` (see `mtk_awdl_layout` for the layout
//! check). Frames come from nearby devices: every access is bounds-checked,
//! and an out-of-range read yields 0 instead of panicking.

pub mod advertisers;
pub mod protocol;
pub mod timeline;

pub(crate) mod bytes {
    /// `f[i]`, or 0 past the end.
    #[inline]
    pub fn at(f: &[u8], i: usize) -> u8 {
        f.get(i).copied().unwrap_or(0)
    }

    #[inline]
    pub fn le16(f: &[u8], i: usize) -> u16 {
        u16::from_le_bytes([at(f, i), at(f, i.wrapping_add(1))])
    }

    #[inline]
    pub fn le32(f: &[u8], i: usize) -> u32 {
        le16(f, i) as u32 | (le16(f, i.wrapping_add(2)) as u32) << 16
    }

    #[inline]
    pub fn be32(f: &[u8], i: usize) -> u32 {
        u32::from_be_bytes([
            at(f, i),
            at(f, i.wrapping_add(1)),
            at(f, i.wrapping_add(2)),
            at(f, i.wrapping_add(3)),
        ])
    }

    /// Six bytes at `i` (zero-filled past the end).
    #[inline]
    pub fn mac(f: &[u8], i: usize) -> [u8; 6] {
        let mut m = [0; 6];
        for (k, b) in m.iter_mut().enumerate() {
            *b = at(f, i.wrapping_add(k));
        }
        m
    }

    #[inline]
    pub fn set(f: &mut [u8], i: usize, v: u8) {
        if let Some(b) = f.get_mut(i) {
            *b = v;
        }
    }

    #[inline]
    pub fn put16(f: &mut [u8], i: usize, v: u16) {
        set(f, i, v as u8);
        set(f, i.wrapping_add(1), (v >> 8) as u8);
    }

    #[inline]
    pub fn put32(f: &mut [u8], i: usize, v: u32) {
        put16(f, i, v as u16);
        put16(f, i.wrapping_add(2), (v >> 16) as u16);
    }

    /// `f[i..i + src.len()] = src`, clipped to `f`.
    #[inline]
    pub fn copy(f: &mut [u8], i: usize, src: &[u8]) {
        for (k, &b) in src.iter().enumerate() {
            set(f, i.wrapping_add(k), b);
        }
    }

    #[inline]
    pub fn fill(f: &mut [u8], i: usize, n: usize, v: u8) {
        for k in 0..n {
            set(f, i.wrapping_add(k), v);
        }
    }

    #[inline]
    pub fn eq(f: &[u8], i: usize, want: &[u8]) -> bool {
        f.get(i..).and_then(|r| r.get(..want.len())) == Some(want)
    }
}
