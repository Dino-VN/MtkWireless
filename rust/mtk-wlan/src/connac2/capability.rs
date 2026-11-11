//! Raw fields in the MCU's `MT_NIC_CAP_PHY` payload (Linux
//! `mt76_connac_phy_cap`). These firmware-reported bytes are diagnostics, not
//! proof of independent radios or simultaneous RF operation.

use mtk_common::{Reader, Result};

/// C ABI: `mtk_wlan_phy_caps` in `kext/Wifi/mtk_wlan.h`.
/// Keep all fields as raw bytes, including the nominal boolean fields.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct PhyCaps {
    pub ht: u8,
    pub vht: u8,
    pub band_5ghz: u8,
    pub max_bw: u8,
    pub nss: u8,
    pub dbdc: u8,
    pub tx_ldpc: u8,
    pub rx_ldpc: u8,
    pub tx_stbc: u8,
    pub rx_stbc: u8,
    pub hw_path: u8,
    pub he: u8,
}

impl PhyCaps {
    /// Parse the payload, without its TLV header. Require the complete known
    /// 12-byte prefix; allow trailing extension bytes, as the HAL did before.
    /// Do not clamp NSS, normalize flags, or infer a number of radios here.
    pub fn parse(data: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(data);
        Ok(Self {
            ht: reader.u8()?,
            vht: reader.u8()?,
            band_5ghz: reader.u8()?,
            max_bw: reader.u8()?,
            nss: reader.u8()?,
            dbdc: reader.u8()?,
            tx_ldpc: reader.u8()?,
            rx_ldpc: reader.u8()?,
            tx_stbc: reader.u8()?,
            rx_stbc: reader.u8()?,
            hw_path: reader.u8()?,
            he: reader.u8()?,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn preserves_every_raw_field() {
        let bytes = [
            0x80, 0x81, 0x82, 0x83, 0xff, 0x85, 0x86, 0x87, 0x88, 0x89, 0xfe, 0x8b,
        ];
        assert_eq!(
            PhyCaps::parse(&bytes),
            Ok(PhyCaps {
                ht: 0x80,
                vht: 0x81,
                band_5ghz: 0x82,
                max_bw: 0x83,
                nss: 0xff,
                dbdc: 0x85,
                tx_ldpc: 0x86,
                rx_ldpc: 0x87,
                tx_stbc: 0x88,
                rx_stbc: 0x89,
                hw_path: 0xfe,
                he: 0x8b,
            })
        );
        assert_eq!(PhyCaps::parse(&[0; 12]), Ok(PhyCaps::default()));
    }

    #[test]
    fn requires_all_twelve_bytes_but_allows_extensions() {
        let bytes = [0xff; 32];
        for len in 0..12 {
            assert!(PhyCaps::parse(&bytes[..len]).is_err(), "length {len}");
        }
        let expected = PhyCaps::parse(&bytes[..12]).unwrap();
        for len in 12..=bytes.len() {
            assert_eq!(PhyCaps::parse(&bytes[..len]), Ok(expected));
        }
    }

    #[test]
    fn c_layout() {
        use core::mem::{align_of, offset_of, size_of};
        assert_eq!(size_of::<PhyCaps>(), 12);
        assert_eq!(align_of::<PhyCaps>(), 1);
        assert_eq!(
            [
                offset_of!(PhyCaps, ht),
                offset_of!(PhyCaps, vht),
                offset_of!(PhyCaps, band_5ghz),
                offset_of!(PhyCaps, max_bw),
                offset_of!(PhyCaps, nss),
                offset_of!(PhyCaps, dbdc),
                offset_of!(PhyCaps, tx_ldpc),
                offset_of!(PhyCaps, rx_ldpc),
                offset_of!(PhyCaps, tx_stbc),
                offset_of!(PhyCaps, rx_stbc),
                offset_of!(PhyCaps, hw_path),
                offset_of!(PhyCaps, he),
            ],
            [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
        );
    }
}
