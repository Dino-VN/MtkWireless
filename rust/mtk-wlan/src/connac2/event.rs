//! Packets on the RX rings that are not plain 802.11 frames: classification
//! (`mwx_dma_rx_process`) and TX-free reports (`mwx_mac_tx_free`).

use mtk_common::bytes::le32_at;

pub const PKT_TYPE_TXS: u32 = 0;
pub const PKT_TYPE_NORMAL: u32 = 2;
pub const PKT_TYPE_TXRX_NOTIFY: u32 = 6;
pub const PKT_TYPE_RX_EVENT: u32 = 7;
pub const PKT_TYPE_NORMAL_MCU: u32 = 8;

const RXD0_PKT_TYPE_MASK: u32 = 0xf800_0000;
const RXD0_PKT_TYPE_SHIFT: u32 = 27;
const RXD0_PKT_FLAG_MASK: u32 = 0x000f_0000;
const RXD0_PKT_FLAG_SHIFT: u32 = 16;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Class {
    /// `PKT_TYPE_*`; an RX event with flag 1 is reported as NORMAL_MCU.
    pub kind: u32,
    pub flag: u32,
    pub rxd0: u32,
}

/// Packet type from the first dword. `None` when the packet is shorter than
/// one dword (the C version read it anyway).
pub fn classify(packet: &[u8]) -> Option<Class> {
    let rxd0 = le32_at(packet, 0).ok()?;
    let mut kind = (rxd0 & RXD0_PKT_TYPE_MASK) >> RXD0_PKT_TYPE_SHIFT;
    let flag = (rxd0 & RXD0_PKT_FLAG_MASK) >> RXD0_PKT_FLAG_SHIFT;
    if kind == PKT_TYPE_RX_EVENT && flag == 0x1 {
        kind = PKT_TYPE_NORMAL_MCU;
    }
    Some(Class { kind, flag, rxd0 })
}

/// Number of 8-dword TX status records in a TXS packet of `len` bytes
/// (they start at dword 2).
pub fn txs_records(len: usize) -> usize {
    (len / 4).saturating_sub(2) / 8
}

const TX_FREE0_MSDU_CNT_MASK: u32 = 0x03ff_0000;
const TX_FREE0_MSDU_CNT_SHIFT: u32 = 16;
const TX_FREE_PAIR: u32 = 1 << 31;
const TX_FREE_MSDU_ID_MASK: u32 = 0x7fff_0000;
const TX_FREE_MSDU_ID_SHIFT: u32 = 16;
const TX_FREE_STATUS_MASK: u32 = 0x0000_6000;
const TX_FREE_COUNT_MASK: u32 = 0x0000_1fff;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TxFreed {
    /// TXWI token. Callers must still range-check it against their table.
    pub msdu: u16,
    /// 0 = acknowledged.
    pub status: u8,
    /// Number of transmissions.
    pub count: u16,
    pub raw: u32,
}

/// MSDUs released by a TXRX_NOTIFY packet: two header dwords, then one
/// dword per MSDU; PAIR (station) dwords do not count towards the MSDU count
/// in the header. Never yields words beyond the packet.
pub struct TxFree<'a> {
    words: &'a [u8],
    index: usize,
    count: usize,
}

impl<'a> TxFree<'a> {
    pub fn new(packet: &'a [u8]) -> Self {
        let empty = Self {
            words: &[],
            index: 0,
            count: 0,
        };
        let (Ok(header), Some(words)) = (le32_at(packet, 0), packet.get(8..)) else {
            return empty;
        };
        let count = ((header & TX_FREE0_MSDU_CNT_MASK) >> TX_FREE0_MSDU_CNT_SHIFT) as usize;
        if count * 4 > words.len() {
            return empty;
        }
        Self {
            words,
            index: 0,
            count,
        }
    }
}

impl<'a> TxFree<'a> {
    /// State for callers that cannot hold the iterator (C ABI):
    /// (payload words, next index, current count).
    pub fn into_parts(self) -> (&'a [u8], usize, usize) {
        (self.words, self.index, self.count)
    }

    /// Rebuilds an iterator from [`TxFree::into_parts`].
    pub fn from_parts(words: &'a [u8], index: usize, count: usize) -> Self {
        Self {
            words,
            index,
            count,
        }
    }
}

impl Iterator for TxFree<'_> {
    type Item = TxFreed;

    fn next(&mut self) -> Option<TxFreed> {
        while self.index < self.count {
            let raw = le32_at(self.words, self.index * 4).ok()?;
            self.index += 1;
            if raw & TX_FREE_PAIR != 0 {
                self.count += 1;
                continue;
            }
            return Some(TxFreed {
                msdu: ((raw & TX_FREE_MSDU_ID_MASK) >> TX_FREE_MSDU_ID_SHIFT) as u16,
                status: ((raw & TX_FREE_STATUS_MASK) >> 13) as u8,
                count: (raw & TX_FREE_COUNT_MASK) as u16,
                raw,
            });
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    fn packet(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    #[test]
    fn classifies() {
        let ev = |t: u32, f: u32| packet(&[t << 27 | f << 16]);
        assert_eq!(classify(&ev(7, 0)).unwrap().kind, PKT_TYPE_RX_EVENT);
        assert_eq!(classify(&ev(7, 1)).unwrap().kind, PKT_TYPE_NORMAL_MCU);
        assert_eq!(
            classify(&ev(2, 3)).unwrap(),
            Class {
                kind: 2,
                flag: 3,
                rxd0: 2 << 27 | 3 << 16
            }
        );
        assert_eq!(classify(&[1, 2, 3]), None);
        assert_eq!(txs_records(8 + 32 * 2 + 4), 2);
        assert_eq!(txs_records(4), 0);
    }

    #[test]
    fn tx_free_entries_and_pairs() {
        let hdr = 3 << 16;
        let msdu = |id: u32, st: u32, n: u32| id << 16 | st << 13 | n;
        let p = packet(&[
            hdr,
            0,
            TX_FREE_PAIR | 5,
            msdu(10, 0, 1),
            msdu(11, 2, 7),
            msdu(12, 0, 1),
        ]);
        let got: Vec<_> = TxFree::new(&p)
            .map(|f| (f.msdu, f.status, f.count))
            .collect();
        // The PAIR word extends the count: all three MSDUs are released.
        assert_eq!(got, [(10, 0, 1), (11, 2, 7), (12, 0, 1)]);
    }

    #[test]
    fn tx_free_bounds() {
        assert_eq!(
            TxFree::new(&packet(&[5 << 16, 0, 1])).count(),
            0,
            "count beyond payload"
        );
        assert_eq!(TxFree::new(&packet(&[0])).count(), 0);
        // PAIR words cannot extend iteration past the packet.
        let p = packet(&[2 << 16, 0, TX_FREE_PAIR, TX_FREE_PAIR]);
        assert_eq!(TxFree::new(&p).count(), 0);
    }
}
