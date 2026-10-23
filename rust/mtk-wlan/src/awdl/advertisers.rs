//! Tahoe's native AWDL_ADVERTISERS control record (`AwdlAdvertisers.hpp`).

use super::bytes::{be32, le16, le32, mac};

/// Packed, 44 bytes; field +20 is big-endian on the wire.
#[repr(C, packed)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct AdvertiserRecord {
    pub aw_counter: u16,
    pub down_counter: u16,
    pub sync_contention: u16,
    pub af_period: u16,
    pub raw8: u16,
    pub raw10: u16,
    pub aw_period: u16,
    pub rssi: i8,
    pub reserved15: u8,
    pub presence: u8,
    pub reserved17: u8,
    pub raw18: u8,
    pub reserved19: u8,
    pub fw_af_rx_time: u32,
    pub peer_self_metric: u32,
    pub root_metric: u32,
    pub root_master: [u8; 6],
    pub immediate_peer: [u8; 6],
}

const _: () = assert!(core::mem::size_of::<AdvertiserRecord>() == 44);

pub fn decode_advertiser_record(raw: &[u8]) -> Option<AdvertiserRecord> {
    if raw.len() != 44 {
        return None;
    }
    Some(AdvertiserRecord {
        aw_counter: le16(raw, 0),
        down_counter: le16(raw, 2),
        sync_contention: le16(raw, 4),
        af_period: le16(raw, 6),
        raw8: le16(raw, 8),
        raw10: le16(raw, 10),
        aw_period: le16(raw, 12),
        rssi: raw[14] as i8,
        reserved15: raw[15],
        presence: raw[16],
        reserved17: raw[17],
        raw18: raw[18],
        reserved19: raw[19],
        fw_af_rx_time: be32(raw, 20),
        peer_self_metric: le32(raw, 24),
        root_metric: le32(raw, 28),
        root_master: mac(raw, 32),
        immediate_peer: mac(raw, 38),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_fields_and_endianness() {
        let mut raw = [0u8; 44];
        raw[0] = 0x34;
        raw[1] = 0x12;
        raw[14] = 0xF6;
        raw[16] = 4;
        raw[20..24].copy_from_slice(&0x0102_0304u32.to_be_bytes());
        raw[24..28].copy_from_slice(&7u32.to_le_bytes());
        raw[32..38].copy_from_slice(&[2, 3, 4, 5, 6, 7]);
        let r = decode_advertiser_record(&raw).unwrap();
        assert_eq!({ r.aw_counter }, 0x1234);
        assert_eq!({ r.rssi }, -10);
        assert_eq!({ r.fw_af_rx_time }, 0x0102_0304);
        assert_eq!({ r.peer_self_metric }, 7);
        assert_eq!({ r.root_master }, [2, 3, 4, 5, 6, 7]);
        assert!(decode_advertiser_record(&raw[..43]).is_none());
    }
}
