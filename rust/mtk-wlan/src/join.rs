//! Key-free diagnostic view before STA address/replay filtering.
use mtk_common::{Error, Reader, Result};

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct EapolObservation {
    pub receiver: [u8; 6],
    pub transmitter: [u8; 6],
    pub info: u16,
    pub data_length: u16,
}

impl EapolObservation {
    /// Only unfragmented, plaintext FromDS data with a complete EAPOL-Key
    /// body. No payload search, decryption, aggregation, or acceptance change.
    pub fn parse(frame: &[u8]) -> Result<Self> {
        let mut r = Reader::new(frame);
        let fc = r.le16()?;
        if fc & 0x00ff != 0x0008 && fc & 0x00ff != 0x0088 {
            return Err(Error::Invalid("not data or QoS data"));
        }
        if fc & 0x4700 != 0x0200 {
            return Err(Error::Invalid("not plaintext unfragmented FromDS"));
        }
        r.skip(2)?; // duration
        let receiver = r.array()?;
        let transmitter = r.array()?;
        r.skip(6)?; // source
        if r.le16()? & 15 != 0 {
            return Err(Error::Invalid("fragment"));
        }
        if fc & 0x0080 != 0 {
            if r.le16()? & 0x0080 != 0 {
                return Err(Error::Invalid("A-MSDU"));
            }
            if fc & 0x8000 != 0 {
                r.skip(4)?; // HT control
            }
        }
        if r.take(8)? != [0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e] {
            return Err(Error::Invalid("not EAPOL SNAP"));
        }
        let version = r.u8()?;
        if !(1..=3).contains(&version) || r.u8()? != 3 {
            return Err(Error::Invalid("not EAPOL-Key"));
        }
        let body_length = usize::from(r.be16()?);
        let mut body = Reader::new(r.take(body_length)?);
        let desc = body.u8()?;
        if desc != 2 && desc != 254 {
            return Err(Error::Invalid("not RSN/WPA key descriptor"));
        }
        let info = body.be16()?;
        body.skip(2 + 8 + 32 + 16 + 8 + 8 + 16)?;
        let data_length = body.be16()?;
        body.skip(usize::from(data_length))?;
        if !body.is_empty() {
            return Err(Error::Invalid("key data length differs from body"));
        }
        Ok(Self {
            receiver,
            transmitter,
            info,
            data_length,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;
    fn frame(qos: bool, info: u16, data_length: usize) -> Vec<u8> {
        let mut f = Vec::from(if qos { [0x88, 2] } else { [8, 2] });
        f.extend_from_slice(&[0; 2]);
        f.extend_from_slice(&[0xea, 0xab, 0x1b, 0x10, 0x25, 0xa2]);
        f.extend_from_slice(&[0x52, 0xb8, 0xa8, 0x1f, 2, 0xb7]);
        f.extend_from_slice(&[0; 8]);
        if qos {
            f.extend_from_slice(&[0; 2]);
        }
        f.extend_from_slice(&[0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e, 2, 3]);
        f.extend_from_slice(&((95 + data_length) as u16).to_be_bytes());
        f.push(2);
        f.extend_from_slice(&info.to_be_bytes());
        f.extend_from_slice(&[0; 90]);
        f.extend_from_slice(&(data_length as u16).to_be_bytes());
        f.resize(f.len() + data_length, 0);
        f
    }
    #[test]
    fn observes_m1_m3_and_addresses_without_key_material() {
        assert_eq!(core::mem::size_of::<EapolObservation>(), 16);
        for qos in [false, true] {
            for (info, n) in [(0x008a, 0), (0x13ca, 56)] {
                let f = frame(qos, info, n);
                let out = EapolObservation::parse(&f).unwrap();
                assert_eq!(out.receiver, [0xea, 0xab, 0x1b, 0x10, 0x25, 0xa2]);
                assert_eq!(out.transmitter, [0x52, 0xb8, 0xa8, 0x1f, 2, 0xb7]);
                assert_eq!(out.info, info);
                assert_eq!(out.data_length, n as u16);
                for end in 0..f.len() {
                    assert!(EapolObservation::parse(&f[..end]).is_err());
                }
            }
        }
    }
    #[test]
    fn rejects_wrong_direction_encryption_fragments_aggregation_and_lengths() {
        let f = frame(true, 0x008a, 8);
        for (offset, value) in [
            (0, 0x80),
            (1, 1),
            (1, 0x42),
            (1, 6),
            (24, 0x80),
            (22, 1),
            (26, 0),
            (35, 0),
            (37, 0),
            (38, 1),
            (131, 9),
        ] {
            let mut bad = f.clone();
            bad[offset] = value;
            assert!(EapolObservation::parse(&bad).is_err(), "offset {offset}");
        }
        let mut ht = f.clone();
        ht[1] |= 0x80;
        ht.splice(26..26, [0; 4]);
        assert!(EapolObservation::parse(&ht).is_ok());
    }
}
