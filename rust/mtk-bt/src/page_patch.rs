//! Masked find/replace inside one code page of bluetoothd, as the kernel
//! validates it (kext/BT/Patcher.cpp `csValidatePage`).
//!
//! A site is `find`/`find_mask`/`replace`/`replace_mask`, all the same length.
//! A page byte matches when `page & find_mask == find & find_mask`, so a zero
//! mask byte is a wildcard (rel32/RIP displacements that move between macOS
//! builds). Applying writes `replace` where `replace_mask` is set and keeps
//! the page's own bits elsewhere. A site must match exactly once in the page:
//! a second match means the pattern is not specific enough for this build,
//! and nothing is changed.

use mtk_common::{Error, Result};

/// Where a site matched in a page.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Match {
    None,
    /// Offset of the only match.
    One(usize),
    /// At least two matches; the site is ambiguous on this page.
    Many,
}

/// One masked site. `mask` arguments may be empty: exact compare / write.
#[derive(Debug, Clone, Copy)]
pub struct Site<'a> {
    pub find: &'a [u8],
    pub find_mask: &'a [u8],
    pub replace: &'a [u8],
    pub replace_mask: &'a [u8],
}

impl Site<'_> {
    /// Lengths agree, the site is not empty, and its first byte is compared
    /// (the search anchors on it).
    pub fn check(&self) -> Result<()> {
        let n = self.find.len();
        if n == 0 || self.replace.len() != n {
            return Err(Error::Invalid("page patch: length"));
        }
        if !(self.find_mask.is_empty() || self.find_mask.len() == n)
            || !(self.replace_mask.is_empty() || self.replace_mask.len() == n)
        {
            return Err(Error::Invalid("page patch: mask length"));
        }
        if self.find_mask.first().is_some_and(|&m| m != 0xff) {
            return Err(Error::Invalid("page patch: first byte must be exact"));
        }
        Ok(())
    }

    fn find_mask(&self, i: usize) -> u8 {
        self.find_mask.get(i).copied().unwrap_or(0xff)
    }

    fn replace_mask(&self, i: usize) -> u8 {
        self.replace_mask.get(i).copied().unwrap_or(0xff)
    }

    fn matches_at(&self, page: &[u8], at: usize) -> bool {
        let Some(window) = at.checked_add(self.find.len()).and_then(|end| page.get(at..end)) else {
            return false;
        };
        window
            .iter()
            .zip(self.find)
            .enumerate()
            .all(|(i, (&p, &f))| (p ^ f) & self.find_mask(i) == 0)
    }

    /// Searches the whole page; matches that would run past its end are not
    /// matches (a site never spans two validated pages).
    pub fn find_in(&self, page: &[u8]) -> Result<Match> {
        self.check()?;
        let n = self.find.len();
        if page.len() < n {
            return Ok(Match::None);
        }
        let first = self.find[0];
        let mut found = Match::None;
        for at in 0..=page.len() - n {
            if page[at] != first || !self.matches_at(page, at) {
                continue;
            }
            if found != Match::None {
                return Ok(Match::Many);
            }
            found = Match::One(at);
        }
        Ok(found)
    }

    /// Writes the site at `at`, which must still match.
    pub fn apply_at(&self, page: &mut [u8], at: usize) -> Result<()> {
        self.check()?;
        if !self.matches_at(page, at) {
            return Err(Error::Invalid("page patch: no match at offset"));
        }
        let window = &mut page[at..at + self.find.len()];
        for (i, (p, &r)) in window.iter_mut().zip(self.replace).enumerate() {
            let m = self.replace_mask(i);
            *p = (*p & !m) | (r & m);
        }
        Ok(())
    }

    /// The site's bytes as they would be after applying it over `original`
    /// (same length as the site). Used by the host tests to compare with a
    /// historical exact replacement.
    #[cfg(test)]
    fn applied(&self, original: &[u8]) -> Result<std::vec::Vec<u8>> {
        let mut out = original.to_vec();
        self.apply_at(&mut out, 0)?;
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIND: [u8; 6] = [0xe8, 0x11, 0x22, 0x33, 0x44, 0xc3];
    const MASK: [u8; 6] = [0xff, 0, 0, 0, 0, 0xff];
    const REPLACE: [u8; 6] = [0x90, 0x90, 0, 0, 0, 0xc3];
    const RMASK: [u8; 6] = [0xff, 0xff, 0, 0, 0, 0];

    fn site() -> Site<'static> {
        Site { find: &FIND, find_mask: &MASK, replace: &REPLACE, replace_mask: &RMASK }
    }

    #[test]
    fn wildcard_match_and_partial_write() {
        let mut page = [0u8; 64];
        page[10..16].copy_from_slice(&[0xe8, 0xaa, 0xbb, 0xcc, 0xdd, 0xc3]);
        let s = site();
        assert_eq!(s.find_in(&page).unwrap(), Match::One(10));
        s.apply_at(&mut page, 10).unwrap();
        assert_eq!(page[10..16], [0x90, 0x90, 0xbb, 0xcc, 0xdd, 0xc3]);
        // Applied once, the site no longer matches: idempotent per page.
        assert_eq!(s.find_in(&page).unwrap(), Match::None);
        assert!(s.apply_at(&mut page, 10).is_err());
    }

    #[test]
    fn exact_bytes_outside_the_mask() {
        let mut page = [0u8; 64];
        page[0..6].copy_from_slice(&[0xe8, 1, 2, 3, 4, 0xc2]);
        assert_eq!(site().find_in(&page).unwrap(), Match::None);
    }

    #[test]
    fn ambiguous_sites_are_left_alone() {
        let mut page = [0u8; 64];
        page[0..6].copy_from_slice(&FIND);
        page[30..36].copy_from_slice(&[0xe8, 9, 9, 9, 9, 0xc3]);
        assert_eq!(site().find_in(&page).unwrap(), Match::Many);
    }

    #[test]
    fn page_edges() {
        let mut page = [0u8; 64];
        page[58..64].copy_from_slice(&FIND);
        assert_eq!(site().find_in(&page).unwrap(), Match::One(58));
        // A site cut by the page end is not a match.
        let mut short = [0u8; 63];
        short[58..63].copy_from_slice(&FIND[..5]);
        assert_eq!(site().find_in(&short).unwrap(), Match::None);
        assert_eq!(site().find_in(&FIND[..3]).unwrap(), Match::None);
        let mut exact = FIND;
        assert_eq!(site().find_in(&exact).unwrap(), Match::One(0));
        assert!(site().apply_at(&mut exact, 1).is_err());
        assert!(site().apply_at(&mut exact, usize::MAX).is_err());
    }

    #[test]
    fn empty_masks_are_exact() {
        let s = Site { find: &FIND, find_mask: &[], replace: &REPLACE, replace_mask: &[] };
        let mut page = [0u8; 16];
        page[2..8].copy_from_slice(&FIND);
        assert_eq!(s.find_in(&page).unwrap(), Match::One(2));
        s.apply_at(&mut page, 2).unwrap();
        assert_eq!(page[2..8], REPLACE);
        page[2..8].copy_from_slice(&[0xe8, 0, 0x22, 0x33, 0x44, 0xc3]);
        assert_eq!(s.find_in(&page).unwrap(), Match::None);
    }

    #[test]
    fn malformed_sites() {
        let bad = [
            Site { find: &[], find_mask: &[], replace: &[], replace_mask: &[] },
            Site { find: &FIND, find_mask: &[], replace: &REPLACE[..5], replace_mask: &[] },
            Site { find: &FIND, find_mask: &MASK[..5], replace: &REPLACE, replace_mask: &[] },
            Site { find: &FIND, find_mask: &[], replace: &REPLACE, replace_mask: &RMASK[..2] },
            Site { find: &FIND, find_mask: &[0, 0xff, 0xff, 0xff, 0xff, 0xff], replace: &REPLACE, replace_mask: &[] },
        ];
        for s in bad {
            assert!(s.check().is_err());
            assert!(s.find_in(&[0u8; 32]).is_err());
        }
    }

    #[test]
    fn applied_matches_historical_exact_patch() {
        let s = site();
        assert_eq!(s.applied(&[0xe8, 1, 2, 3, 4, 0xc3]).unwrap(), [0x90, 0x90, 2, 3, 4, 0xc3]);
    }
}
