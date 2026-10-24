//! Targets in Tahoe's driver-facing WCL scan request (0x1550 bytes).
use mtk_common::{Error, Reader, Result};

pub const MAX_SSIDS: usize = 10;
const LIST_COUNT: usize = 0x1318;
const ENTRY_SIZE: usize = 56;

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct ScanSsids {
    pub count: u8,
    pub lengths: [u8; MAX_SSIDS],
    pub ssids: [[u8; 32]; MAX_SSIDS],
    pub wildcard: bool,
}

impl ScanSsids {
    fn append(&mut self, ssid: &[u8]) -> Result<()> {
        if ssid.len() > 32 {
            return Err(Error::Invalid("scan SSID length"));
        }
        if ssid.is_empty() {
            self.wildcard = true;
            return Ok(());
        }
        // Legacy and list forms may name the same target.
        for i in 0..usize::from(self.count) {
            if self.lengths[i] as usize == ssid.len()
                && self.ssids[i].get(..ssid.len()) == Some(ssid)
            {
                return Ok(());
            }
        }
        let i = usize::from(self.count);
        let dst = self
            .ssids
            .get_mut(i)
            .ok_or(Error::Invalid("too many scan SSIDs"))?;
        dst.get_mut(..ssid.len())
            .ok_or(Error::Invalid("scan SSID length"))?
            .copy_from_slice(ssid);
        *self
            .lengths
            .get_mut(i)
            .ok_or(Error::Invalid("too many scan SSIDs"))? = ssid.len() as u8;
        self.count += 1;
        Ok(())
    }

    pub fn parse_wcl(data: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(data);
        reader.seek(0x1c)?;
        let length = reader.le32()? as usize;
        let legacy = reader.take(32)?;
        let mut out = Self::default();
        if length > 0 {
            out.append(
                legacy
                    .get(..length)
                    .ok_or(Error::Invalid("scan SSID length"))?,
            )?;
        }
        // Short legacy requests have no list tail. Native WCL always supplies
        // the full buffer; reject a partial tail instead of treating it as empty.
        if data.len() > LIST_COUNT {
            reader.seek(LIST_COUNT)?;
            let count = reader.le32()? as usize;
            if count > MAX_SSIDS {
                return Err(Error::Invalid("scan SSID count"));
            }
            for _ in 0..count {
                let mut entry = Reader::new(reader.take(ENTRY_SIZE)?);
                let version = entry.le32()?;
                let length = entry.le32()? as usize;
                // Apple80211 appends a zero-filled wildcard entry.
                if version != 1 && !(version == 0 && length == 0) {
                    return Err(Error::Invalid("scan SSID version"));
                }
                let bytes = entry.take(32)?;
                out.append(
                    bytes
                        .get(..length)
                        .ok_or(Error::Invalid("scan SSID length"))?,
                )?;
            }
        }
        if out.count == 0 {
            out.wildcard = true;
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mtk_common::Writer;

    fn put32(data: &mut [u8], offset: usize, value: u32) {
        Writer::new(&mut data[offset..]).le32(value).unwrap();
    }
    fn entry(data: &mut [u8], index: usize, ssid: &[u8]) {
        let offset = LIST_COUNT + 4 + index * ENTRY_SIZE;
        let mut writer = Writer::new(&mut data[offset..]);
        writer.le32(1).unwrap();
        writer.le32(ssid.len() as u32).unwrap();
        writer.put(ssid).unwrap();
    }

    #[test]
    fn hotspot_list_and_wildcard() {
        let mut data = [0; 0x1550];
        put32(&mut data, LIST_COUNT, 2);
        entry(&mut data, 0, b"Dino");
        let out = ScanSsids::parse_wcl(&data).unwrap();
        assert_eq!(out.count, 1);
        assert_eq!(out.lengths[0], 4);
        assert_eq!(&out.ssids[0][..4], b"Dino");
        assert!(out.wildcard);
    }
    #[test]
    fn all_targets_survive_and_legacy_is_preserved() {
        let mut data = [0; 0x1550];
        put32(&mut data, LIST_COUNT, 10);
        for i in 0..10 {
            entry(&mut data, i, &[i as u8; 32]);
        }
        let out = ScanSsids::parse_wcl(&data).unwrap();
        assert_eq!(out.count, 10);
        assert!(!out.wildcard);
        for i in 0..10 {
            assert_eq!(out.ssids[i], [i as u8; 32]);
        }
        put32(&mut data, 0x1c, 32); // same first target: deduplicated
        assert_eq!(ScanSsids::parse_wcl(&data).unwrap().count, 10);
        data[0x20] = 99; // an eleventh distinct target is rejected
        assert!(ScanSsids::parse_wcl(&data).is_err());
        let mut legacy = [0; 0x58];
        put32(&mut legacy, 0x1c, 4);
        legacy[0x20..0x24].copy_from_slice(b"Dino");
        let out = ScanSsids::parse_wcl(&legacy).unwrap();
        assert_eq!(out.count, 1);
        assert!(!out.wildcard);
        put32(&mut legacy, 0x1c, 0);
        assert!(ScanSsids::parse_wcl(&legacy).unwrap().wildcard);
    }
    #[test]
    fn rejects_bad_counts_lengths_versions_and_truncations() {
        let mut data = [0; 0x1550];
        put32(&mut data, LIST_COUNT, 2);
        entry(&mut data, 0, b"Dino");
        // Every truncation within the advertised tail is rejected.
        for n in LIST_COUNT + 1..LIST_COUNT + 4 + 2 * ENTRY_SIZE {
            assert!(ScanSsids::parse_wcl(&data[..n]).is_err(), "{n}");
        }
        put32(&mut data, LIST_COUNT, u32::MAX);
        assert!(ScanSsids::parse_wcl(&data).is_err());
        put32(&mut data, LIST_COUNT, 1);
        put32(&mut data, LIST_COUNT + 8, 33);
        assert!(ScanSsids::parse_wcl(&data).is_err());
        put32(&mut data, LIST_COUNT + 8, 4);
        put32(&mut data, LIST_COUNT + 4, 2);
        assert!(ScanSsids::parse_wcl(&data).is_err());
        put32(&mut data, 0x1c, u32::MAX);
        assert!(ScanSsids::parse_wcl(&data).is_err());
        assert!(ScanSsids::parse_wcl(&[]).is_err());
    }
    #[test]
    fn c_layout() {
        assert_eq!(core::mem::size_of::<ScanSsids>(), 332);
        assert_eq!(core::mem::offset_of!(ScanSsids, ssids), 11);
        assert_eq!(core::mem::offset_of!(ScanSsids, wildcard), 331);
    }
}
