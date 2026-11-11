//! Firmware images: the ROM patch (`*_patch_mcu_*_hdr`) and the WM RAM code
//! (`*_ram_code_*`), as downloaded by `mwx_load_firmware` / Linux
//! `mt76_connac2_load_patch` + `mt76_connac2_load_ram`.
//!
//! Parsing only: the result says what to send where. Every region is checked
//! to lie inside the image before it is handed out.

use mtk_common::bytes::{le32_at, slice_at};
use mtk_common::{Error, Reader, Result};

// Download mode bits (`DL_MODE_*`) for the MCU "init download" command.
pub const DL_MODE_ENCRYPT: u32 = 0x01;
pub const DL_MODE_KEY_IDX_MASK: u32 = 0x06;
const DL_MODE_KEY_IDX_SHIFT: u32 = 1;
pub const DL_MODE_RESET_SEC_IV: u32 = 0x08;
pub const DL_CONFIG_ENCRY_MODE_SEL: u32 = 0x40;
pub const DL_MODE_NEED_RSP: u32 = 0x8000_0000;

// RAM region feature bits.
pub const FW_FEATURE_SET_ENCRYPT: u8 = 0x01;
pub const FW_FEATURE_SET_KEY_IDX_MASK: u8 = 0x06;
pub const FW_FEATURE_ENCRY_MODE: u8 = 0x10;
pub const FW_FEATURE_OVERRIDE_ADDR: u8 = 0x20;
pub const FW_FEATURE_NON_DL: u8 = 0x40;

pub const FW_START_OVERRIDE: u32 = 0x01;

const PATCH_SEC_NOT_SUPPORT: u32 = 0xffff_ffff;
const PATCH_SEC_TYPE_MASK: u32 = 0x0000_ffff;
const PATCH_SEC_TYPE_INFO: u32 = 0x2;
const PATCH_SEC_ENC_TYPE_MASK: u32 = 0xff00_0000;
const PATCH_SEC_ENC_TYPE_PLAIN: u32 = 0x00 << 24;
const PATCH_SEC_ENC_TYPE_AES: u32 = 0x01 << 24;
const PATCH_SEC_ENC_TYPE_SCRAMBLE: u32 = 0x02 << 24;

/// One block to download: `data` goes to `addr` with download mode `mode`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Region<'a> {
    pub addr: u32,
    pub mode: u32,
    /// Offset of `data` in the image.
    pub offset: usize,
    pub data: &'a [u8],
    /// RAM regions only: feature bits.
    pub feature: u8,
    /// False for RAM regions marked "not downloaded" (their bytes are skipped).
    pub download: bool,
}

fn be32_at(data: &[u8], offset: usize) -> Result<u32> {
    le32_at(data, offset).map(u32::swap_bytes)
}

/// Download mode for a patch section's `sec_key_idx` word.
pub fn patch_data_mode(info: u32) -> u32 {
    let mut mode = DL_MODE_NEED_RSP;
    if info == PATCH_SEC_NOT_SUPPORT {
        return mode;
    }
    match info & PATCH_SEC_ENC_TYPE_MASK {
        PATCH_SEC_ENC_TYPE_PLAIN => {}
        PATCH_SEC_ENC_TYPE_AES => {
            mode |= DL_MODE_ENCRYPT;
            mode |= (info << DL_MODE_KEY_IDX_SHIFT) & DL_MODE_KEY_IDX_MASK;
            mode |= DL_MODE_RESET_SEC_IV;
        }
        PATCH_SEC_ENC_TYPE_SCRAMBLE => {
            mode |= DL_MODE_ENCRYPT | DL_CONFIG_ENCRY_MODE_SEL | DL_MODE_RESET_SEC_IV;
        }
        // Unknown encryption: sent as plain, as mwx and mt76 do.
        _ => {}
    }
    mode
}

/// Download mode for a RAM region's feature set.
pub fn ram_data_mode(feature: u8) -> u32 {
    let mut mode = DL_MODE_NEED_RSP;
    if feature & FW_FEATURE_SET_ENCRYPT != 0 {
        mode |= DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV;
    }
    if feature & FW_FEATURE_ENCRY_MODE != 0 {
        mode |= DL_CONFIG_ENCRY_MODE_SEL;
    }
    // FW_FEATURE_SET_KEY_IDX_MASK == DL_MODE_KEY_IDX_MASK
    mode | (feature & FW_FEATURE_SET_KEY_IDX_MASK) as u32
}

/// ROM patch image.
///
/// ```text
/// 0x00 build_date[16] platform[4] hw_sw_ver patch_ver checksum reserved   (BE)
/// 0x20 desc: patch_ver subsys feature n_region crc reserved[11]           (BE)
/// 0x60 sections, 64 bytes: type offs size addr len sec_key_idx align ...  (BE)
/// ```
#[derive(Debug, Clone, Copy)]
pub struct PatchImage<'a> {
    data: &'a [u8],
    pub build_date: [u8; 16],
    pub hw_sw_ver: u32,
    pub patch_ver: u32,
    pub regions: u32,
}

const PATCH_HEADER_SIZE: usize = 96;
const PATCH_SECTION_SIZE: usize = 64;

impl<'a> PatchImage<'a> {
    pub fn parse(data: &'a [u8]) -> Result<Self> {
        let header = slice_at(data, 0, PATCH_HEADER_SIZE)?;
        let regions = be32_at(header, 32 + 12)?;
        let table = (regions as usize)
            .checked_mul(PATCH_SECTION_SIZE)
            .ok_or(Error::Invalid("patch region count"))?;
        slice_at(data, PATCH_HEADER_SIZE, table)?;
        let mut build_date = [0; 16];
        build_date.copy_from_slice(&header[..16]);
        Ok(Self {
            data,
            build_date,
            hw_sw_ver: be32_at(header, 20)?,
            patch_ver: be32_at(header, 24)?,
            regions,
        })
    }

    pub fn region(&self, index: u32) -> Result<Region<'a>> {
        if index >= self.regions {
            return Err(Error::Invalid("patch region index"));
        }
        let sec = slice_at(
            self.data,
            PATCH_HEADER_SIZE + index as usize * PATCH_SECTION_SIZE,
            PATCH_SECTION_SIZE,
        )?;
        if be32_at(sec, 0)? & PATCH_SEC_TYPE_MASK != PATCH_SEC_TYPE_INFO {
            return Err(Error::Invalid("patch region type"));
        }
        let offset = be32_at(sec, 4)? as usize;
        let len = be32_at(sec, 16)? as usize;
        Ok(Region {
            addr: be32_at(sec, 12)?,
            mode: patch_data_mode(be32_at(sec, 20)?),
            offset,
            data: slice_at(self.data, offset, len)?,
            feature: 0,
            download: true,
        })
    }
}

/// Firmware-reported CNM offload flag (`MT792x_FW_CAP_CNM`, Linux v6.12).
/// It is not evidence of independent radios or simultaneous RF operation.
pub const FW_CAP_CNM: u8 = 0x80;

/// Optional release-feature record, separate from the download plan.
/// C ABI: `mtk_wlan_fw_features` in `kext/Wifi/mtk_wlan.h`.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct FwFeatures {
    pub present: bool,
    pub segment: u8,
    pub flags: u8,
}

/// WM RAM code image: region payloads back to back from offset 0, optional
/// release metadata, the region table (40 bytes each), then a 36-byte trailer
/// at the very end:
///
/// ```text
/// trailer: chip_id eco_code n_region format_ver format_flag reserved[2]
///          fw_ver[10] build_date[15] crc
/// region:  decomp_crc decomp_len decomp_blk_sz reserved[4] addr len
///          feature_set reserved[15]                                   (LE)
/// ```
#[derive(Debug, Clone, Copy)]
pub struct RamImage<'a> {
    data: &'a [u8],
    payload_len: usize,
    table: usize,
    pub chip_id: u8,
    pub eco_code: u8,
    pub regions: u8,
    pub fw_ver: [u8; 10],
    pub build_date: [u8; 15],
}

const RAM_TRAILER_SIZE: usize = 36;
const RAM_REGION_SIZE: usize = 40;
const RELEASE_PREFIX_SIZE: usize = 16;
const RELEASE_FEATURE_TAG: u8 = 4;
const RELEASE_FEATURE_SIZE: usize = 16;

/// Linux v6.12 `mt792x_realease_info`: the length excludes the four-byte
/// header and padding. Used for both the outer container and inner records.
fn release_record<'a>(reader: &mut Reader<'a>) -> Result<(u8, &'a [u8])> {
    let len = reader.le16()? as usize;
    let pad_len = reader.u8()? as usize;
    let tag = reader.u8()?;
    let payload = reader.take(len)?;
    reader.skip(pad_len)?;
    Ok((tag, payload))
}

impl<'a> RamImage<'a> {
    pub fn parse(data: &'a [u8]) -> Result<Self> {
        let trailer_at = data
            .len()
            .checked_sub(RAM_TRAILER_SIZE)
            .ok_or(Error::Truncated {
                offset: 0,
                need: RAM_TRAILER_SIZE,
            })?;
        let trailer = &data[trailer_at..];
        let regions = trailer[2];
        let table = trailer_at
            .checked_sub(regions as usize * RAM_REGION_SIZE)
            .ok_or(Error::Invalid("RAM region table larger than image"))?;
        let mut fw_ver = [0; 10];
        fw_ver.copy_from_slice(&trailer[7..17]);
        let mut build_date = [0; 15];
        build_date.copy_from_slice(&trailer[17..32]);
        Ok(Self {
            data,
            payload_len: table,
            table,
            chip_id: trailer[0],
            eco_code: trailer[1],
            regions,
            fw_ver,
            build_date,
        })
    }

    fn entry(&self, index: u8) -> Result<(u32, usize, u8)> {
        if index >= self.regions {
            return Err(Error::Invalid("RAM region index"));
        }
        let r = slice_at(
            self.data,
            self.table + index as usize * RAM_REGION_SIZE,
            RAM_REGION_SIZE,
        )?;
        Ok((le32_at(r, 16)?, le32_at(r, 20)? as usize, r[24]))
    }

    pub fn region(&self, index: u8) -> Result<Region<'a>> {
        let mut offset = 0usize;
        for i in 0..index {
            let (_, len, _) = self.entry(i)?;
            offset = offset
                .checked_add(len)
                .ok_or(Error::Invalid("RAM region offsets"))?;
        }
        let (addr, len, feature) = self.entry(index)?;
        let end = offset
            .checked_add(len)
            .ok_or(Error::Invalid("RAM region offsets"))?;
        if end > self.payload_len {
            return Err(Error::Invalid("RAM region exceeds payload"));
        }
        Ok(Region {
            addr,
            mode: ram_data_mode(feature),
            offset,
            data: &self.data[offset..end],
            feature,
            download: feature & FW_FEATURE_NON_DL == 0,
        })
    }

    /// Optional diagnostic release features, following Linux v6.12's
    /// `mt792x_get_offload_capability` layout but with bounded readers.
    ///
    /// An empty metadata gap or a valid release without tag 4 means absent.
    /// A partial release, a non-16-byte feature payload, or duplicate feature
    /// records is malformed. Unknown records are skipped with their padding;
    /// the entire outer record is validated, even after a feature is found.
    /// Prefix/suffix bytes are opaque, never searched for feature tags.
    /// This is deliberately not called by the download parser.
    pub fn features(&self) -> Result<FwFeatures> {
        let mut image = Reader::new(self.data);
        // The region table is a hard bound, not part of the metadata. Advance
        // by every region's length, including NON_DL regions. Reader::skip
        // checks each addition and rejects a payload sum beyond the table.
        let mut metadata = Reader::new(image.take(self.table)?);
        for _ in 0..self.regions {
            let mut region = Reader::new(image.take(RAM_REGION_SIZE)?);
            region.skip(20)?;
            metadata.skip(region.le32()? as usize)?;
        }
        if metadata.is_empty() {
            return Ok(FwFeatures::default());
        }
        metadata.skip(RELEASE_PREFIX_SIZE)?;
        let (_, contents) = release_record(&mut metadata)?;
        // Ignore the suffix after the outer record/padding (the shipped
        // images use 16 '#' bytes); it is not another record stream.
        let mut records = Reader::new(contents);
        let mut features = FwFeatures::default();
        while !records.is_empty() {
            let (tag, contents) = release_record(&mut records)?;
            if tag != RELEASE_FEATURE_TAG {
                continue;
            }
            if features.present {
                return Err(Error::Invalid("duplicate RAM release feature"));
            }
            if contents.len() != RELEASE_FEATURE_SIZE {
                return Err(Error::Invalid("RAM release feature size"));
            }
            let mut payload = Reader::new(contents);
            features = FwFeatures {
                present: true,
                segment: payload.u8()?,
                flags: payload.u8()?,
            };
            payload.skip(14)?; // reserved, not required to be zero
        }
        Ok(features)
    }

    /// Start address for the "start firmware" command: the last region with
    /// the override feature, or 0.
    pub fn override_addr(&self) -> Result<u32> {
        let mut addr = 0;
        for i in 0..self.regions {
            let (a, _, feature) = self.entry(i)?;
            if feature & FW_FEATURE_OVERRIDE_ADDR != 0 {
                addr = a;
            }
        }
        Ok(addr)
    }

    pub fn start_option(&self) -> Result<u32> {
        Ok(if self.override_addr()? != 0 {
            FW_START_OVERRIDE
        } else {
            0
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    fn firmware(name: &str) -> Vec<u8> {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../firmware/wifi")
            .join(name);
        std::fs::read(path).expect("firmware/wifi present")
    }

    #[test]
    fn shipped_patches_parse() {
        for name in [
            "mwx-mt7961_patch_mcu_1_2_hdr",
            "mwx-mt7922_patch_mcu_1_1_hdr",
        ] {
            let data = firmware(name);
            let img = PatchImage::parse(&data).unwrap();
            assert!(img.regions >= 1, "{name}");
            for i in 0..img.regions {
                let r = img.region(i).unwrap();
                assert!(!r.data.is_empty());
                assert_eq!(r.mode & DL_MODE_NEED_RSP, DL_MODE_NEED_RSP);
            }
        }
    }

    #[test]
    fn shipped_ram_images_parse() {
        for name in ["mwx-mt7961_ram_code_1", "mwx-mt7922_ram_code_1"] {
            let data = firmware(name);
            let img = RamImage::parse(&data).unwrap();
            assert!(img.regions >= 2, "{name}");
            let mut total = 0;
            for i in 0..img.regions {
                let r = img.region(i).unwrap();
                assert_eq!(r.offset, total);
                total += r.data.len();
            }
            assert!(total <= data.len() - 36 - 40 * img.regions as usize);
            assert!(img.fw_ver.iter().all(|b| b.is_ascii()));
        }
    }

    fn patch(sections: &[(u32, u32, u32, u32)]) -> Vec<u8> {
        // (type, offs, addr, len)
        let mut d = vec![0u8; PATCH_HEADER_SIZE + PATCH_SECTION_SIZE * sections.len()];
        d[44..48].copy_from_slice(&(sections.len() as u32).to_be_bytes());
        for (i, &(ty, offs, addr, len)) in sections.iter().enumerate() {
            let s = PATCH_HEADER_SIZE + PATCH_SECTION_SIZE * i;
            d[s..s + 4].copy_from_slice(&ty.to_be_bytes());
            d[s + 4..s + 8].copy_from_slice(&offs.to_be_bytes());
            d[s + 12..s + 16].copy_from_slice(&addr.to_be_bytes());
            d[s + 16..s + 20].copy_from_slice(&len.to_be_bytes());
            d[s + 20..s + 24].copy_from_slice(&PATCH_SEC_NOT_SUPPORT.to_be_bytes());
        }
        d.resize(d.len() + 64, 0xAB);
        d
    }

    #[test]
    fn patch_bounds_and_type() {
        let ok = patch(&[(2, 160, 0x1000, 64)]);
        let r = PatchImage::parse(&ok).unwrap().region(0).unwrap();
        assert_eq!(
            (r.addr, r.offset, r.data.len(), r.mode),
            (0x1000, 160, 64, DL_MODE_NEED_RSP)
        );

        let past_end = patch(&[(2, 160, 0, 65)]);
        assert!(PatchImage::parse(&past_end).unwrap().region(0).is_err());
        let wild_offset = patch(&[(2, u32::MAX, 0, 1)]);
        assert!(PatchImage::parse(&wild_offset).unwrap().region(0).is_err());
        let bad_type = patch(&[(3, 160, 0, 1)]);
        assert!(PatchImage::parse(&bad_type).unwrap().region(0).is_err());
        let mut many = patch(&[]);
        many[44..48].copy_from_slice(&1000u32.to_be_bytes());
        assert!(PatchImage::parse(&many).is_err());
        assert!(PatchImage::parse(&ok[..95]).is_err());
    }

    #[test]
    fn ram_bounds_override_and_skip() {
        // payload 100 + 50 bytes, two regions, trailer.
        let mut d = vec![0u8; 150];
        for (addr, len, feature) in [
            (0x9000_0000u32, 100u32, FW_FEATURE_NON_DL),
            (
                0x9010_0000,
                50,
                FW_FEATURE_OVERRIDE_ADDR | FW_FEATURE_SET_ENCRYPT,
            ),
        ] {
            let mut r = [0u8; RAM_REGION_SIZE];
            r[16..20].copy_from_slice(&addr.to_le_bytes());
            r[20..24].copy_from_slice(&len.to_le_bytes());
            r[24] = feature;
            d.extend_from_slice(&r);
        }
        let mut t = [0u8; RAM_TRAILER_SIZE];
        t[2] = 2;
        d.extend_from_slice(&t);
        let img = RamImage::parse(&d).unwrap();
        let r0 = img.region(0).unwrap();
        assert!(!r0.download);
        let r1 = img.region(1).unwrap();
        assert_eq!((r1.offset, r1.data.len()), (100, 50));
        assert_eq!(
            r1.mode,
            DL_MODE_NEED_RSP | DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV
        );
        assert_eq!(img.override_addr(), Ok(0x9010_0000));
        assert_eq!(img.start_option(), Ok(FW_START_OVERRIDE));
        assert!(img.region(2).is_err());

        // Lengths that overrun the payload are rejected.
        let at = 150 + 40 + 20;
        d[at..at + 4].copy_from_slice(&51u32.to_le_bytes());
        assert!(RamImage::parse(&d).unwrap().region(1).is_err());
        // Table claims more regions than fit.
        let n = d.len() - RAM_TRAILER_SIZE + 2;
        d[n] = 200;
        assert!(RamImage::parse(&d).is_err());
        assert!(RamImage::parse(&d[..10]).is_err());
    }

    fn ram_with_metadata(payload: &[u8], metadata: &[u8], regions: &[(u32, u8)]) -> Vec<u8> {
        let mut data = payload.to_vec();
        data.extend_from_slice(metadata);
        for (i, &(len, feature)) in regions.iter().enumerate() {
            let mut region = [0; RAM_REGION_SIZE];
            // These are not file offsets/lengths; the metadata parser must
            // use the length at +20, not an address or decompressed length.
            region[4..8].copy_from_slice(&u32::MAX.to_le_bytes());
            region[16..20].copy_from_slice(&(0x9000_0000 + i as u32 * 0x1000).to_le_bytes());
            region[20..24].copy_from_slice(&len.to_le_bytes());
            region[24] = feature;
            data.extend_from_slice(&region);
        }
        let mut trailer = [0; RAM_TRAILER_SIZE];
        trailer[2] = regions.len().try_into().unwrap();
        data.extend_from_slice(&trailer);
        data
    }

    fn record(tag: u8, payload: &[u8], padding: &[u8]) -> Vec<u8> {
        let mut data = u16::try_from(payload.len()).unwrap().to_le_bytes().to_vec();
        data.push(padding.len().try_into().unwrap());
        data.push(tag);
        data.extend_from_slice(payload);
        data.extend_from_slice(padding);
        data
    }

    fn feature_record(segment: u8, flags: u8) -> Vec<u8> {
        let mut payload = [0; RELEASE_FEATURE_SIZE];
        payload[0] = segment;
        payload[1] = flags;
        record(RELEASE_FEATURE_TAG, &payload, &[])
    }

    fn release_metadata(records: &[u8], padding: &[u8], suffix: &[u8]) -> Vec<u8> {
        let mut data = vec![b'#'; RELEASE_PREFIX_SIZE];
        data.extend(record(0, records, padding));
        data.extend_from_slice(suffix);
        data
    }

    fn features_from_metadata(metadata: &[u8]) -> Result<FwFeatures> {
        let data = ram_with_metadata(&[0xaa; 3], metadata, &[(3, 0)]);
        RamImage::parse(&data)?.features()
    }

    #[test]
    fn shipped_ram_feature_metadata() {
        // Observations of these exact repository fixtures, not universal
        // properties of MT7921/MT7922 cards or proof of simultaneous radios.
        for (name, payload_end, table) in [
            ("mwx-mt7922_ram_code_1", 1_018_272, 1_018_416),
            ("mwx-mt7961_ram_code_1", 796_912, 797_064),
        ] {
            let data = firmware(name);
            let img = RamImage::parse(&data).unwrap();
            let total: usize = (0..img.regions)
                .map(|i| img.region(i).unwrap().data.len())
                .sum();
            assert_eq!(total, payload_end, "{name}");
            assert_eq!(img.table, table, "{name}");
            assert_eq!(&data[payload_end..payload_end + 16], &[b'#'; 16]);
            assert_eq!(&data[table - 16..table], &[b'#'; 16]);
            assert_eq!(
                img.features(),
                Ok(FwFeatures {
                    present: true,
                    segment: 2,
                    flags: 0xc0
                }),
                "{name}"
            );
        }
        assert_eq!(FW_CAP_CNM, 0x80);
    }

    #[test]
    fn ram_features_are_optional_but_zero_flags_are_present() {
        assert_eq!(features_from_metadata(&[]), Ok(FwFeatures::default()));
        for records in [vec![], record(1, b"release name", &[0, 0])] {
            let metadata = release_metadata(&records, &[], &[]);
            assert_eq!(features_from_metadata(&metadata), Ok(FwFeatures::default()));
        }
        let metadata = release_metadata(&feature_record(0, 0), &[], &[]);
        assert_eq!(
            features_from_metadata(&metadata),
            Ok(FwFeatures {
                present: true,
                segment: 0,
                flags: 0
            })
        );
        let data = ram_with_metadata(&[], &[], &[]);
        assert_eq!(
            RamImage::parse(&data).unwrap().features(),
            Ok(FwFeatures::default())
        );
    }

    #[test]
    fn ram_features_sum_all_regions_including_non_downloaded() {
        let metadata = release_metadata(&feature_record(9, 0x7f), &[], &[]);
        let data = ram_with_metadata(
            &[0x55; 13],
            &metadata,
            &[
                (4, 0),
                (7, FW_FEATURE_NON_DL),
                (2, FW_FEATURE_OVERRIDE_ADDR),
            ],
        );
        let img = RamImage::parse(&data).unwrap();
        assert_eq!(
            img.features(),
            Ok(FwFeatures {
                present: true,
                segment: 9,
                flags: 0x7f
            })
        );
        assert!(!img.region(1).unwrap().download);
        assert_eq!(img.region(2).unwrap().offset, 11);

        // Even the maximum descriptor count is bounded and advances by the
        // sum, not the first/last region alone. Zero lengths make progress.
        let mut regions = vec![(0, FW_FEATURE_NON_DL); u8::MAX as usize];
        *regions.last_mut().unwrap() = (1, FW_FEATURE_NON_DL);
        let data = ram_with_metadata(&[0], &metadata, &regions);
        assert_eq!(RamImage::parse(&data).unwrap().features(), img.features());
    }

    #[test]
    fn release_records_preserve_raw_bytes_and_skip_unknowns_and_padding() {
        let mut records = record(0x91, &vec![0x55; 0x102], &[0xfe; 255]);
        let mut payload = [0xa5; RELEASE_FEATURE_SIZE]; // reserved bytes need not be zero
        payload[0] = 0xff;
        payload[1] = 0x7f; // do not invent CNM or normalize unrelated bits
        records.extend(record(RELEASE_FEATURE_TAG, &payload, &[0xaa; 3]));
        records.extend(record(0x92, &[], &[])); // zero-length unknown still advances
        let metadata = release_metadata(&records, &[0xfd; 255], &[b'#'; 16]);
        assert_eq!(
            features_from_metadata(&metadata),
            Ok(FwFeatures {
                present: true,
                segment: 0xff,
                flags: 0x7f
            })
        );
    }

    #[test]
    fn feature_tags_are_not_searched_for_outside_the_inner_record_stream() {
        let fake = feature_record(2, 0xc0);
        let records = record(0x81, &fake, &fake);
        let mut metadata = release_metadata(&records, &fake, &fake);
        metadata[..16].copy_from_slice(&fake[..16]); // prefix is opaque, not a magic signature
        metadata[19] = RELEASE_FEATURE_TAG; // the outer tag is not an inner feature
        let data = ram_with_metadata(&fake, &metadata, &[(fake.len() as u32, 0)]);
        assert_eq!(
            RamImage::parse(&data).unwrap().features(),
            Ok(FwFeatures::default())
        );

        // A valid-looking feature outside the outer len cannot be consumed.
        let metadata = release_metadata(&[], &[], &fake);
        assert_eq!(features_from_metadata(&metadata), Ok(FwFeatures::default()));
    }

    #[test]
    fn feature_payload_must_have_the_documented_size() {
        for len in (0..RELEASE_FEATURE_SIZE).chain([17, 32, 256]) {
            let records = record(RELEASE_FEATURE_TAG, &vec![0; len], &[]);
            assert!(
                features_from_metadata(&release_metadata(&records, &[], &[])).is_err(),
                "{len}"
            );
        }
    }

    #[test]
    fn duplicate_features_and_malformed_following_records_are_rejected() {
        for second in [feature_record(0, 0), feature_record(2, 0xc0), vec![0, 0, 0]] {
            let mut records = feature_record(0, 0);
            records.extend(second);
            let metadata = release_metadata(&records, &[], &[]);
            assert!(features_from_metadata(&metadata).is_err());
        }
    }

    #[test]
    fn every_release_truncation_including_padding_is_rejected() {
        let records = record(RELEASE_FEATURE_TAG, &[0; RELEASE_FEATURE_SIZE], &[1, 2, 3]);
        let metadata = release_metadata(&records, &[4, 5], &[]);
        // Rebuild an intact descriptor table after every truncated metadata
        // gap. Neither a payload nor padding may borrow bytes from that table.
        for len in 1..metadata.len() {
            assert!(features_from_metadata(&metadata[..len]).is_err(), "{len}");
        }
        assert!(features_from_metadata(&metadata).unwrap().present);
    }

    #[test]
    fn release_lengths_are_bounded_by_both_outer_payload_and_region_table() {
        let good = release_metadata(&feature_record(2, 0xc0), &[], &[]);
        for (offset, len) in [
            (16, 19u16),
            (16, 21),
            (16, u16::MAX),
            (20, 17),
            (20, u16::MAX),
        ] {
            let mut metadata = good.clone();
            metadata[offset..offset + 2].copy_from_slice(&len.to_le_bytes());
            assert!(
                features_from_metadata(&metadata).is_err(),
                "offset {offset} length {len}"
            );
        }
        // Declared outer padding must also fit before the descriptor table.
        let mut metadata = good.clone();
        metadata[18] = 1;
        assert!(features_from_metadata(&metadata).is_err());

        // A suffix provides ample bytes, but none belong to the inner record.
        let mut metadata = good;
        metadata.extend_from_slice(&[0; 255]);
        metadata[22] = 255;
        assert!(features_from_metadata(&metadata).is_err());
        metadata[22] = 0;
        metadata[20..22].copy_from_slice(&17u16.to_le_bytes());
        assert!(features_from_metadata(&metadata).is_err());

        // A partial header and an unknown tag with an oversized length must
        // fail too; absence is only returned for a well-formed record stream.
        for records in [
            vec![0],
            vec![0, 0],
            vec![0, 0, 0],
            vec![0xff, 0xff, 0, 0x99],
        ] {
            let metadata = release_metadata(&records, &[], &[]);
            assert!(features_from_metadata(&metadata).is_err());
        }
    }

    #[test]
    fn ram_feature_region_lengths_and_offsets_never_wrap_or_cross_the_table() {
        let metadata = release_metadata(&feature_record(2, 0xc0), &[], &[]);
        for regions in [
            vec![(u32::MAX, 0)],
            vec![(u32::MAX, 0), (1, 0)],
            vec![(1, 0), (u32::MAX, FW_FEATURE_NON_DL)],
            vec![(u32::MAX, 0); u8::MAX as usize],
            vec![(2 + metadata.len() as u32, 0)],
        ] {
            let data = ram_with_metadata(&[0], &metadata, &regions);
            assert!(RamImage::parse(&data).unwrap().features().is_err());
        }
        // At the exact table boundary there is no metadata. Do not scan the
        // region's payload for an otherwise valid release-feature record.
        let data = ram_with_metadata(&[0], &metadata, &[(1 + metadata.len() as u32, 0)]);
        assert_eq!(
            RamImage::parse(&data).unwrap().features(),
            Ok(FwFeatures::default())
        );
    }

    #[test]
    fn malformed_metadata_does_not_change_the_download_plan() {
        let payload = [1, 2, 3];
        let data = ram_with_metadata(
            &payload,
            &[0x12], // truncated metadata, but the download region is intact
            &[(3, FW_FEATURE_NON_DL | FW_FEATURE_OVERRIDE_ADDR)],
        );
        let img = RamImage::parse(&data).unwrap();
        assert!(img.features().is_err());
        let region = img.region(0).unwrap();
        assert_eq!(region.data, payload);
        assert_eq!(region.offset, 0);
        assert!(!region.download);
        assert_eq!(img.override_addr(), Ok(0x9000_0000));
        assert_eq!(img.start_option(), Ok(FW_START_OVERRIDE));
    }

    #[test]
    fn data_modes() {
        assert_eq!(patch_data_mode(PATCH_SEC_NOT_SUPPORT), DL_MODE_NEED_RSP);
        assert_eq!(
            patch_data_mode(PATCH_SEC_ENC_TYPE_AES | 1),
            DL_MODE_NEED_RSP | DL_MODE_ENCRYPT | 2 | DL_MODE_RESET_SEC_IV
        );
        assert_eq!(
            patch_data_mode(PATCH_SEC_ENC_TYPE_SCRAMBLE),
            DL_MODE_NEED_RSP | DL_MODE_ENCRYPT | DL_CONFIG_ENCRY_MODE_SEL | DL_MODE_RESET_SEC_IV
        );
        assert_eq!(
            ram_data_mode(FW_FEATURE_SET_KEY_IDX_MASK),
            DL_MODE_NEED_RSP | 6
        );
    }
}
