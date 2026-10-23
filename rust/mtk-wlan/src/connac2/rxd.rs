//! RX descriptor (RXD) in front of every received 802.11 frame
//! (`mt7921_mac_fill_rx`).
//!
//! ```text
//! dword 0..5   fixed part (rxd0..rxd5)
//! group 4      4 dwords   (frame control, seq/qos)
//! group 1      4 dwords   (IV)
//! group 2      2 dwords   (timestamp)
//! group 3      2 dwords   P-RXV: v0 rate, v1 RCPI per chain
//! group 5     18 dwords   C-RXV; with it, the RCPI word is group5[0]
//!              (6 dwords into the group 3+5 area)
//! then 2 * hdr_offset bytes of padding, then the 802.11 header
//! ```
//!
//! The descriptor comes from the hardware. The C version read the group 3/5
//! words before checking that the buffer held them; here the whole layout is
//! checked first.

use mtk_common::bytes::le32_at;

const RXD1_WLAN_IDX_MASK: u32 = 0x0000_03ff;
const RXD1_GROUP_1: u32 = 1 << 11;
const RXD1_GROUP_2: u32 = 1 << 12;
const RXD1_GROUP_3: u32 = 1 << 13;
const RXD1_GROUP_4: u32 = 1 << 14;
const RXD1_GROUP_5: u32 = 1 << 15;
const RXD1_SEC_MODE_MASK: u32 = 0x001f_0000;
const RXD1_CM: u32 = 1 << 23;
const RXD1_CLM: u32 = 1 << 24;
const RXD1_ICV_ERR: u32 = 1 << 25;
const RXD1_TKIP_MIC_ERR: u32 = 1 << 26;
const RXD1_FCS_ERR: u32 = 1 << 27;
const RXD1_BAND_IDX: u32 = 1 << 28;
const RXD2_HDR_TRANS: u32 = 1 << 13;
const RXD2_HDR_OFFSET_MASK: u32 = 0x0000_c000;
const RXD2_HDR_OFFSET_SHIFT: u32 = 14;
const RXD2_AMSDU_ERR: u32 = 1 << 23;
const RXD2_MAX_LEN_ERROR: u32 = 1 << 24;
const RXD3_CH_NUM_MASK: u32 = 0x0000_ff00;
const RXD3_CH_NUM_SHIFT: u32 = 8;
const RXD3_ADDR_TYPE_MASK: u32 = 0x0003_0000;
const RXD3_U2M: u32 = 1 << 16;

/// `sizeof(struct ieee80211_frame_min)`: fc, duration, addr1, addr2.
const FRAME_MIN: usize = 16;
const MAX_CHAINS: usize = 4;

/// Why a frame is dropped. Values are the C ABI codes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Drop {
    ShortRxd = 1,
    BandIdx = 2,
    AmsduErr = 3,
    HdrTrans = 4,
    /// ICV or CCMP/BIP/WPI MIC error.
    Icv = 5,
    Fcs = 6,
    /// TKIP MIC failure: report to net80211 (countermeasures).
    TkipMic = 7,
    MaxLen = 8,
    ShortGroups = 9,
    Short80211 = 10,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RxInfo {
    pub rxd: [u32; 5],
    /// Bytes before the 802.11 header (descriptor + padding).
    pub hdr_gap: usize,
    pub channel: u8,
    pub unicast: bool,
    pub wlan_idx: u16,
    /// Decrypted by the hardware (IV/MIC removed).
    pub hw_decrypted: bool,
    /// P-RXV word 0 (rate) when group 3 is present, else 0.
    pub rate: u32,
    /// Strongest chain (dBm) when group 3 is present.
    pub rssi: Option<i8>,
}

fn rcpi_to_rssi(word: u32, shift: u32) -> i8 {
    ((((word >> shift) & 0xff) as i32 - 220) / 2) as i8
}

/// Strongest negative chain signal among `num_streams` chains enabled in
/// `antenna_mask`; -128 when none qualifies.
pub fn signal(rcpi: u32, num_streams: u8, antenna_mask: u32) -> i8 {
    let mut signal = i8::MIN;
    for i in 0..(num_streams as usize).min(MAX_CHAINS) {
        if antenna_mask & (1 << i) == 0 {
            continue;
        }
        let chain = rcpi_to_rssi(rcpi, 8 * i as u32);
        if chain < 0 {
            signal = signal.max(chain);
        }
    }
    signal
}

pub fn parse(frame: &[u8], num_streams: u8, antenna_mask: u32) -> Result<RxInfo, Drop> {
    let dword = |i: usize| le32_at(frame, i * 4).map_err(|_| Drop::ShortGroups);
    if frame.len() < 6 * 4 {
        return Err(Drop::ShortRxd);
    }
    let rxd = [dword(0)?, dword(1)?, dword(2)?, dword(3)?, dword(4)?];
    let [_, rxd1, rxd2, rxd3, _] = rxd;

    if rxd1 & RXD1_BAND_IDX != 0 {
        return Err(Drop::BandIdx);
    }
    if rxd2 & RXD2_AMSDU_ERR != 0 {
        return Err(Drop::AmsduErr);
    }
    if rxd2 & RXD2_HDR_TRANS != 0 {
        return Err(Drop::HdrTrans);
    }
    if rxd1 & RXD1_ICV_ERR != 0 {
        return Err(Drop::Icv);
    }
    if rxd1 & RXD1_FCS_ERR != 0 {
        return Err(Drop::Fcs);
    }
    if rxd1 & RXD1_TKIP_MIC_ERR != 0 {
        return Err(Drop::TkipMic);
    }
    let hw_decrypted = rxd1 & RXD1_SEC_MODE_MASK != 0 && rxd1 & (RXD1_CLM | RXD1_CM) == 0;
    let remove_pad = ((rxd2 & RXD2_HDR_OFFSET_MASK) >> RXD2_HDR_OFFSET_SHIFT) as usize;
    if rxd2 & RXD2_MAX_LEN_ERROR != 0 {
        return Err(Drop::MaxLen);
    }

    let mut dwords = 6;
    for (group, size) in [(RXD1_GROUP_4, 4), (RXD1_GROUP_1, 4), (RXD1_GROUP_2, 2)] {
        if rxd1 & group != 0 {
            dwords += size;
        }
    }
    let mut rate = 0;
    let mut rssi = None;
    if rxd1 & RXD1_GROUP_3 != 0 {
        let prxv = dwords;
        dwords += 2;
        let mut rcpi_at = prxv + 1;
        if rxd1 & RXD1_GROUP_5 != 0 {
            rcpi_at = dwords + 6;
            dwords += 18;
        }
        if frame.len() < dwords * 4 {
            return Err(Drop::ShortGroups);
        }
        rate = dword(prxv)?;
        rssi = Some(signal(dword(rcpi_at)?, num_streams, antenna_mask));
    }
    if frame.len() < dwords * 4 {
        return Err(Drop::ShortGroups);
    }

    let hdr_gap = dwords * 4 + 2 * remove_pad;
    if frame.len() < hdr_gap + FRAME_MIN {
        return Err(Drop::Short80211);
    }
    Ok(RxInfo {
        rxd,
        hdr_gap,
        channel: ((rxd3 & RXD3_CH_NUM_MASK) >> RXD3_CH_NUM_SHIFT) as u8,
        unicast: rxd3 & RXD3_ADDR_TYPE_MASK == RXD3_U2M,
        wlan_idx: (rxd1 & RXD1_WLAN_IDX_MASK) as u16,
        hw_decrypted,
        rate,
        rssi,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    fn frame(rxd1: u32, rxd2: u32, rxd3: u32, extra_dwords: &[u32], payload: usize) -> Vec<u8> {
        let mut d = Vec::new();
        for w in [0x1234u32, rxd1, rxd2, rxd3, 0x55, 0] {
            d.extend_from_slice(&w.to_le_bytes());
        }
        for w in extra_dwords {
            d.extend_from_slice(&w.to_le_bytes());
        }
        d.resize(d.len() + payload, 0xEE);
        d
    }

    // RCPI 160, 140, 220, 0 → -30, -40, 0, -110 dBm
    const RCPI: u32 = 160 | 140 << 8 | 220 << 16;

    #[test]
    fn plain_frame() {
        let f = frame(5, 0, 36 << 8 | RXD3_U2M, &[], 24);
        let rx = parse(&f, 2, 3).unwrap();
        assert_eq!(
            (rx.hdr_gap, rx.channel, rx.unicast, rx.wlan_idx),
            (24, 36, true, 5)
        );
        assert_eq!((rx.rate, rx.rssi, rx.hw_decrypted), (0, None, false));
    }

    #[test]
    fn groups_and_padding() {
        let g = RXD1_GROUP_4 | RXD1_GROUP_1 | RXD1_GROUP_2 | RXD1_GROUP_3;
        let mut extra = vec![0u32; 10];
        extra.extend([0xAB, RCPI]);
        let f = frame(g | 1 << 16, 1 << 14, 0, &extra, 18);
        let rx = parse(&f, 2, 3).unwrap();
        assert_eq!(rx.hdr_gap, (6 + 12) * 4 + 2);
        assert_eq!((rx.rate, rx.rssi), (0xAB, Some(-30)));
        assert!(rx.hw_decrypted);
    }

    #[test]
    fn group5_moves_rcpi() {
        let g = RXD1_GROUP_3 | RXD1_GROUP_5;
        let mut extra = vec![0x77u32, 0xFFFF_FFFF];
        extra.extend([0; 6]);
        extra.push(RCPI);
        extra.extend([0; 11]);
        let f = frame(g, 0, 0, &extra, 16);
        let rx = parse(&f, 2, 3).unwrap();
        assert_eq!(
            (rx.rate, rx.rssi, rx.hdr_gap),
            (0x77, Some(-30), (6 + 20) * 4)
        );
    }

    #[test]
    fn short_groups_are_rejected_before_reading() {
        let g = RXD1_GROUP_3 | RXD1_GROUP_5;
        // Descriptor ends right after the fixed part: the C version read
        // P-RXV words past the buffer here.
        let f = frame(g, 0, 0, &[], 0);
        assert_eq!(parse(&f, 2, 3), Err(Drop::ShortGroups));
        let f = frame(RXD1_GROUP_4, 0, 0, &[0; 3], 0);
        assert_eq!(parse(&f, 2, 3), Err(Drop::ShortGroups));
        assert_eq!(parse(&f[..23], 2, 3), Err(Drop::ShortRxd));
        let f = frame(0, 3 << 14, 0, &[], 16);
        assert_eq!(parse(&f, 2, 3), Err(Drop::Short80211));
    }

    #[test]
    fn error_bits() {
        assert_eq!(
            parse(&frame(RXD1_BAND_IDX, 0, 0, &[], 16), 2, 3),
            Err(Drop::BandIdx)
        );
        assert_eq!(
            parse(&frame(0, RXD2_AMSDU_ERR, 0, &[], 16), 2, 3),
            Err(Drop::AmsduErr)
        );
        assert_eq!(
            parse(&frame(0, RXD2_HDR_TRANS, 0, &[], 16), 2, 3),
            Err(Drop::HdrTrans)
        );
        assert_eq!(
            parse(&frame(RXD1_ICV_ERR, 0, 0, &[], 16), 2, 3),
            Err(Drop::Icv)
        );
        assert_eq!(
            parse(&frame(RXD1_FCS_ERR, 0, 0, &[], 16), 2, 3),
            Err(Drop::Fcs)
        );
        assert_eq!(
            parse(&frame(RXD1_TKIP_MIC_ERR, 0, 0, &[], 16), 2, 3),
            Err(Drop::TkipMic)
        );
        assert_eq!(
            parse(&frame(0, RXD2_MAX_LEN_ERROR, 0, &[], 16), 2, 3),
            Err(Drop::MaxLen)
        );
        // hardware decryption needs a cipher and no CM/CLM
        assert!(
            !parse(&frame(1 << 16 | RXD1_CM, 0, 0, &[], 16), 2, 3)
                .unwrap()
                .hw_decrypted
        );
    }

    #[test]
    fn signal_respects_streams_and_mask() {
        assert_eq!(signal(RCPI, 2, 0b11), -30);
        assert_eq!(signal(RCPI, 2, 0b10), -40);
        assert_eq!(signal(RCPI, 1, 0b10), i8::MIN);
        // chain 2 is 0 dBm (not negative), chain 3 is -110
        assert_eq!(signal(RCPI & !0xffff, 4, 0b1111), -110);
        // a firmware-reported stream count above 4 does not read past the chains
        assert_eq!(signal(RCPI, 200, u32::MAX), -30);
    }

    #[test]
    fn fuzz_never_panics() {
        let mut seed = 0x1234_5678u32;
        for len in 0..200 {
            for _ in 0..50 {
                let f: Vec<u8> = (0..len)
                    .map(|_| {
                        seed ^= seed << 13;
                        seed ^= seed >> 17;
                        seed ^= seed << 5;
                        seed as u8
                    })
                    .collect();
                if let Ok(rx) = parse(&f, 4, 0xf) {
                    assert!(rx.hdr_gap + FRAME_MIN <= f.len());
                }
            }
        }
    }
}
