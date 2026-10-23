//! MediaTek Bluetooth ROM patch image (`BT_RAM_CODE_MT79xx_*_hdr.bin`).
//!
//! Layout (all little-endian), as in Linux `btmtk_setup_firmware_79xx`:
//!
//! ```text
//! 0x00  patch header (32): datetime[16] platform[4] hwver swver magic
//! 0x20  global descriptor (64): patch_ver sub_sys feature_opt section_num ...
//! 0x60  section maps, 64 bytes each:
//!       +0 sectype  +4 secoffset  +8 secsize  +12 bin_info_spec (52 bytes,
//!       dlsize at +16), the 52 bytes after the common part are sent to the
//!       controller as the section's WMT download header
//! ```

use mtk_common::bytes::{le16_at, le32_at, slice_at};
use mtk_common::{Error, Result};

pub const HEADER_SIZE: usize = 32;
pub const GLOBAL_DESC_SIZE: usize = 64;
pub const SECTION_MAP_SIZE: usize = 64;
const SECTION_MAP_COMMON_SIZE: usize = 12;
/// Bytes of the section map sent before the section data.
pub const SECTION_HEADER_SIZE: usize = 52;
const SECTIONS_OFFSET: usize = HEADER_SIZE + GLOBAL_DESC_SIZE;

#[derive(Debug, Clone, Copy)]
pub struct PatchImage<'a> {
    data: &'a [u8],
    pub hw_ver: u16,
    pub sw_ver: u16,
    pub build_time: [u8; 16],
    pub section_count: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Section<'a> {
    pub index: u32,
    /// Section map bytes sent with the WMT "patch download" header command.
    pub header: &'a [u8],
    /// Data to download; empty when the section has nothing to send.
    pub data: &'a [u8],
}

impl<'a> PatchImage<'a> {
    pub fn parse(data: &'a [u8]) -> Result<Self> {
        if data.len() < SECTIONS_OFFSET {
            return Err(Error::Truncated {
                offset: 0,
                need: SECTIONS_OFFSET,
            });
        }
        let mut build_time = [0; 16];
        build_time.copy_from_slice(&data[..16]);
        Ok(Self {
            data,
            hw_ver: le16_at(data, 20)?,
            sw_ver: le16_at(data, 22)?,
            build_time,
            section_count: le32_at(data, HEADER_SIZE + 12)?,
        })
    }

    /// Sections in file order. Iteration stops after the first error.
    pub fn sections(&self) -> Sections<'a> {
        Sections {
            data: self.data,
            next: 0,
            count: self.section_count,
            failed: false,
        }
    }

    pub fn len(&self) -> usize {
        self.data.len()
    }

    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }
}

pub struct Sections<'a> {
    data: &'a [u8],
    next: u32,
    count: u32,
    failed: bool,
}

impl<'a> Iterator for Sections<'a> {
    type Item = Result<Section<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.failed || self.next >= self.count {
            return None;
        }
        let index = self.next;
        self.next += 1;
        let section = section_at(self.data, index);
        self.failed = section.is_err();
        Some(section)
    }
}

fn section_at(data: &[u8], index: u32) -> Result<Section<'_>> {
    let map_offset = (index as usize)
        .checked_mul(SECTION_MAP_SIZE)
        .and_then(|o| o.checked_add(SECTIONS_OFFSET))
        .ok_or(Error::Invalid("section index overflows"))?;
    let map = slice_at(data, map_offset, SECTION_MAP_SIZE)?;
    let offset = le32_at(map, 4)? as usize;
    let dl_size = le32_at(map, SECTION_MAP_COMMON_SIZE + 4)? as usize;
    Ok(Section {
        index,
        header: &map[SECTION_MAP_COMMON_SIZE..SECTION_MAP_COMMON_SIZE + SECTION_HEADER_SIZE],
        data: slice_at(data, offset, dl_size)?,
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// Builds an image with the given section payloads placed after the maps.
    pub fn image(sections: &[&[u8]]) -> Vec<u8> {
        let mut out = vec![0u8; SECTIONS_OFFSET + SECTION_MAP_SIZE * sections.len()];
        out[..16].copy_from_slice(b"20260101000000\0\0");
        out[20..22].copy_from_slice(&0x8a61u16.to_le_bytes());
        out[22..24].copy_from_slice(&0x0001u16.to_le_bytes());
        out[HEADER_SIZE + 12..HEADER_SIZE + 16]
            .copy_from_slice(&(sections.len() as u32).to_le_bytes());
        for (i, payload) in sections.iter().enumerate() {
            let map = SECTIONS_OFFSET + SECTION_MAP_SIZE * i;
            let offset = out.len() as u32;
            out[map + 4..map + 8].copy_from_slice(&offset.to_le_bytes());
            out[map + 16..map + 20].copy_from_slice(&(payload.len() as u32).to_le_bytes());
            out[map + 12] = 0xA0 + i as u8; // marker in the sent header
            out.extend_from_slice(payload);
        }
        out
    }

    #[test]
    fn parses_header_and_sections() {
        let data = image(&[&[1, 2, 3], &[], &[9; 600]]);
        let img = PatchImage::parse(&data).unwrap();
        assert_eq!((img.hw_ver, img.sw_ver, img.section_count), (0x8a61, 1, 3));
        let sections: Vec<_> = img.sections().map(Result::unwrap).collect();
        assert_eq!(sections.len(), 3);
        assert_eq!(sections[0].data, &[1, 2, 3]);
        assert_eq!(sections[0].header.len(), SECTION_HEADER_SIZE);
        assert_eq!(sections[0].header[0], 0xA0);
        assert!(sections[1].data.is_empty());
        assert_eq!(sections[2].data.len(), 600);
    }

    #[test]
    fn rejects_out_of_range_sections() {
        let mut data = image(&[&[1, 2, 3]]);
        let map = SECTIONS_OFFSET;
        data[map + 16..map + 20].copy_from_slice(&u32::MAX.to_le_bytes());
        let img = PatchImage::parse(&data).unwrap();
        let mut it = img.sections();
        assert!(it.next().unwrap().is_err());
        assert!(it.next().is_none());

        // More sections announced than maps present.
        let mut data = image(&[&[1]]);
        data[HEADER_SIZE + 12..HEADER_SIZE + 16].copy_from_slice(&1000u32.to_le_bytes());
        let img = PatchImage::parse(&data).unwrap();
        assert_eq!(img.sections().filter(|s| s.is_err()).count(), 1);
        assert!(PatchImage::parse(&data[..95]).is_err());
    }

    #[test]
    fn huge_section_count_does_not_overflow() {
        let mut data = image(&[]);
        data[HEADER_SIZE + 12..HEADER_SIZE + 16].copy_from_slice(&u32::MAX.to_le_bytes());
        let img = PatchImage::parse(&data).unwrap();
        assert!(img.sections().next().unwrap().is_err());
    }
}
