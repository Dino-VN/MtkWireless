//! WPA3-Personal SAE (IEEE 802.11-2020 12.4), group 19 (P-256) with
//! hunting-and-pecking PWE derivation. No password identifiers, no H2E,
//! no SAE-PK, no anti-clogging token elements (the raw token after the
//! group field, as H&P uses, is supported).
//!
//! The station side, in frame order:
//! [`Sae::init`] derives the PWE, [`Sae::commit`] takes two random scalars
//! from the caller (kernel RNG), [`Sae::write_commit`] builds the Commit
//! body, [`Sae::process_commit`] checks the AP's Commit and derives
//! KCK/PMK, [`Sae::write_confirm`] / [`Sae::check_confirm`] exchange the
//! Confirm messages. Frame bodies are what follows the 6-byte fixed fields
//! of the Authentication frame (algorithm, sequence, status).

pub mod p256;
pub mod sha256;

use mtk_common::{Error, Reader, Result, Writer};
use p256::{BYTES, Fe, Limbs, N, P, Point};

pub const GROUP: u16 = 19;
/// Authentication algorithm number for SAE.
pub const AUTH_ALG: u16 = 3;
pub const PMK_LEN: usize = 32;
pub const PMKID_LEN: usize = 16;
pub const KCK_LEN: usize = 32;
pub const MAX_TOKEN: usize = 128;
pub const COMMIT_LEN: usize = 2 + BYTES + 2 * BYTES;
pub const CONFIRM_LEN: usize = 2 + KCK_LEN;
/// hostapd/IEEE minimum hunting-and-pecking iterations (dragonfly k).
const HNP_MIN_ITERATIONS: u8 = 40;
const HNP_MAX_ITERATIONS: u8 = 200;

/// Status codes the caller may see in the AP's Authentication frames.
pub const STATUS_ANTI_CLOGGING_TOKEN_REQUIRED: u16 = 76;
pub const STATUS_FINITE_CYCLIC_GROUP_NOT_SUPPORTED: u16 = 77;
pub const STATUS_SAE_HASH_TO_ELEMENT: u16 = 126;

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Empty = 0,
    /// PWE derived.
    Ready,
    /// Own commit scalar and element chosen.
    Committed,
    /// Peer commit accepted, KCK/PMK derived.
    Confirmed,
    /// Peer confirm verified.
    Accepted,
}

/// SAE context. Plain data, no pointers, so the C side can keep it in an
/// opaque buffer and clear it with `explicit_bzero`.
#[repr(C)]
pub struct Sae {
    state: State,
    token_len: u32,
    pwe_x: Fe,
    pwe_y: Fe,
    rand: Limbs,
    scalar: Limbs,
    peer_scalar: Limbs,
    element: [u8; 2 * BYTES],
    peer_element: [u8; 2 * BYTES],
    kck: [u8; KCK_LEN],
    pmk: [u8; PMK_LEN],
    pmkid: [u8; PMKID_LEN],
    token: [u8; MAX_TOKEN],
}

const ZERO_LIMBS: Limbs = [0; 4];
const ONE_LIMBS: Limbs = [1, 0, 0, 0];

/// 1 < v < r
fn valid_scalar(v: &Limbs) -> bool {
    p256::less_than(&ONE_LIMBS, v) && p256::less_than(v, &N)
}

impl Sae {
    pub const fn empty() -> Sae {
        Sae {
            state: State::Empty,
            token_len: 0,
            pwe_x: Fe::ZERO,
            pwe_y: Fe::ZERO,
            rand: ZERO_LIMBS,
            scalar: ZERO_LIMBS,
            peer_scalar: ZERO_LIMBS,
            element: [0; 2 * BYTES],
            peer_element: [0; 2 * BYTES],
            kck: [0; KCK_LEN],
            pmk: [0; PMK_LEN],
            pmkid: [0; PMKID_LEN],
            token: [0; MAX_TOKEN],
        }
    }

    pub fn state(&self) -> State {
        self.state
    }

    /// Zeroes every secret and returns to [`State::Empty`].
    pub fn clear(&mut self) {
        // Volatile writes so the clear survives optimization.
        let p = self as *mut Sae as *mut u8;
        for i in 0..core::mem::size_of::<Sae>() {
            // SAFETY: `p` covers `self`; all-zero bytes are a valid `Sae`
            // (State::Empty is 0, the rest is integers).
            unsafe { core::ptr::write_volatile(p.add(i), 0) };
        }
    }

    /// Derives the PWE by hunting and pecking (12.4.4.2.2) from both MAC
    /// addresses and the password.
    pub fn init(&mut self, own: &[u8; 6], peer: &[u8; 6], password: &[u8]) -> Result<()> {
        self.clear();
        if password.is_empty() || password.len() > 256 {
            return Err(Error::Invalid("SAE password length"));
        }
        if own == peer {
            return Err(Error::Invalid("SAE peer address"));
        }
        // Max(STA-A, STA-B) || Min(STA-A, STA-B)
        let mut addrs = [0u8; 12];
        let (hi, lo) = if own > peer { (own, peer) } else { (peer, own) };
        addrs[..6].copy_from_slice(hi);
        addrs[6..].copy_from_slice(lo);

        let prime = p256::to_be(&P);
        let mut found = 0u64; // all ones once found
        let mut x = Fe::ZERO;
        let mut odd = 0u64;
        let mut counter: u8 = 1;
        loop {
            let mut seed = sha256::hmac(&addrs, &[password, &[counter]]);
            let mut value = [0u8; BYTES];
            sha256::kdf(&seed, b"SAE Hunting and Pecking", &prime, &mut value);
            // pwd-value >= p is skipped; every candidate still runs the
            // same quadratic residue test.
            let cand = Fe::from_bytes(&value);
            let in_range = cand.is_some() as u64;
            let cand = cand.unwrap_or(Fe::ONE);
            let qr = Fe::curve_rhs(cand).is_square() as u64;
            let take = (in_range & qr).wrapping_neg() & !found;
            x = Fe::select(take, cand, x);
            odd = (take & u64::from(seed[BYTES - 1] & 1)) | (!take & odd);
            found |= take;
            seed.fill(0);
            value.fill(0);
            if counter >= HNP_MIN_ITERATIONS && found != 0 {
                break;
            }
            if counter == HNP_MAX_ITERATIONS {
                return Err(Error::Invalid("SAE PWE not found"));
            }
            counter += 1;
        }
        let y = Fe::curve_rhs(x).sqrt().ok_or(Error::Invalid("SAE PWE y"))?;
        let flip = (y.is_odd() as u64 ^ odd).wrapping_neg();
        self.pwe_x = x;
        self.pwe_y = Fe::select(flip, y.neg(), y);
        self.state = State::Ready;
        Ok(())
    }

    /// Chooses the commit scalar and element from two secrets in [2, r-1]
    /// supplied big-endian by the caller's RNG. Fails with
    /// `Invalid("SAE random")` when the caller must draw again.
    pub fn commit(&mut self, rand: &[u8; BYTES], mask: &[u8; BYTES]) -> Result<()> {
        if self.state == State::Empty {
            return Err(Error::Invalid("SAE state"));
        }
        let r = p256::from_be(rand);
        let m = p256::from_be(mask);
        if !valid_scalar(&r) || !valid_scalar(&m) {
            return Err(Error::Invalid("SAE random"));
        }
        let scalar = p256::add_mod(&r, &m, &N);
        if !valid_scalar(&scalar) {
            return Err(Error::Invalid("SAE random"));
        }
        let pwe = Point::from_affine(self.pwe_x, self.pwe_y);
        let (ex, ey) = pwe
            .mul(&m)
            .neg()
            .to_affine()
            .ok_or(Error::Invalid("SAE random"))?;
        self.rand = r;
        self.scalar = scalar;
        self.element[..BYTES].copy_from_slice(&ex.to_bytes());
        self.element[BYTES..].copy_from_slice(&ey.to_bytes());
        self.kck.fill(0);
        self.pmk.fill(0);
        self.pmkid.fill(0);
        self.state = State::Committed;
        Ok(())
    }

    /// Commit body: group || anti-clogging token (if set) || scalar ||
    /// element. Returns the length written.
    pub fn write_commit(&self, out: &mut [u8]) -> Result<usize> {
        if self.state == State::Empty || self.state == State::Ready {
            return Err(Error::Invalid("SAE state"));
        }
        let token = self
            .token
            .get(..self.token_len as usize)
            .ok_or(Error::Invalid("SAE token"))?;
        let mut w = Writer::new(out);
        w.le16(GROUP)?;
        w.put(token)?;
        w.put(&p256::to_be(&self.scalar))?;
        w.put(&self.element)?;
        Ok(w.len())
    }

    /// Stores the token of an AP Commit with status 76
    /// (ANTI_CLOGGING_TOKEN_REQUIRED): group || token.
    pub fn set_token(&mut self, body: &[u8]) -> Result<()> {
        let mut r = Reader::new(body);
        if r.le16()? != GROUP {
            return Err(Error::Invalid("SAE group"));
        }
        let token = &body[2..];
        if token.is_empty() || token.len() > MAX_TOKEN {
            return Err(Error::Invalid("SAE token length"));
        }
        self.token[..token.len()].copy_from_slice(token);
        self.token_len = token.len() as u32;
        Ok(())
    }

    /// Checks the AP's Commit body (status 0) and derives KCK, PMK and
    /// PMKID. Trailing elements are ignored.
    pub fn process_commit(&mut self, body: &[u8]) -> Result<()> {
        if self.state != State::Committed {
            return Err(Error::Invalid("SAE state"));
        }
        let mut r = Reader::new(body);
        if r.le16()? != GROUP {
            return Err(Error::Invalid("SAE group"));
        }
        let scalar_bytes: [u8; BYTES] = r.array()?;
        let ex: [u8; BYTES] = r.array()?;
        let ey: [u8; BYTES] = r.array()?;
        let peer_scalar = p256::from_be(&scalar_bytes);
        if !valid_scalar(&peer_scalar) {
            return Err(Error::Invalid("SAE peer scalar"));
        }
        let peer_element = Point::from_bytes(&ex, &ey).ok_or(Error::Invalid("SAE peer element"))?;
        let mut element = [0u8; 2 * BYTES];
        element[..BYTES].copy_from_slice(&ex);
        element[BYTES..].copy_from_slice(&ey);
        // Reflection attack (12.4.5.4).
        if peer_scalar == self.scalar && element == self.element {
            return Err(Error::Invalid("SAE reflected commit"));
        }
        // K = rand * (peer-scalar * PWE + PEER-COMMIT-ELEMENT)
        let pwe = Point::from_affine(self.pwe_x, self.pwe_y);
        let k = pwe.mul(&peer_scalar).add(&peer_element).mul(&self.rand);
        let (kx, _) = k.to_affine().ok_or(Error::Invalid("SAE shared secret"))?;
        let mut kx = kx.to_bytes();

        // keyseed = H(<0>32, k); KCK || PMK = KDF-512(keyseed,
        // "SAE KCK and PMK", (commit-scalar + peer-commit-scalar) mod r)
        let mut keyseed = sha256::hmac(&[0u8; 32], &[&kx]);
        let context = p256::to_be(&p256::add_mod(&self.scalar, &peer_scalar, &N));
        let mut keys = [0u8; KCK_LEN + PMK_LEN];
        sha256::kdf(&keyseed, b"SAE KCK and PMK", &context, &mut keys);
        self.kck.copy_from_slice(&keys[..KCK_LEN]);
        self.pmk.copy_from_slice(&keys[KCK_LEN..]);
        self.pmkid.copy_from_slice(&context[..PMKID_LEN]);
        self.peer_scalar = peer_scalar;
        self.peer_element = element;
        self.rand = ZERO_LIMBS;
        keys.fill(0);
        keyseed.fill(0);
        kx.fill(0);
        self.state = State::Confirmed;
        Ok(())
    }

    fn confirm_hash(&self, send_confirm: u16, own_first: bool) -> [u8; KCK_LEN] {
        let scalar = p256::to_be(&self.scalar);
        let peer_scalar = p256::to_be(&self.peer_scalar);
        let sc = send_confirm.to_le_bytes();
        if own_first {
            sha256::hmac(
                &self.kck,
                &[
                    &sc,
                    &scalar,
                    &self.element,
                    &peer_scalar,
                    &self.peer_element,
                ],
            )
        } else {
            sha256::hmac(
                &self.kck,
                &[
                    &sc,
                    &peer_scalar,
                    &self.peer_element,
                    &scalar,
                    &self.element,
                ],
            )
        }
    }

    /// Confirm body: send-confirm || CN(KCK, ...).
    pub fn write_confirm(&self, send_confirm: u16, out: &mut [u8]) -> Result<usize> {
        if self.state != State::Confirmed && self.state != State::Accepted {
            return Err(Error::Invalid("SAE state"));
        }
        let mut w = Writer::new(out);
        w.le16(send_confirm)?;
        w.put(&self.confirm_hash(send_confirm, true))?;
        Ok(w.len())
    }

    /// Verifies the AP's Confirm body.
    pub fn check_confirm(&mut self, body: &[u8]) -> Result<()> {
        if self.state != State::Confirmed {
            return Err(Error::Invalid("SAE state"));
        }
        let mut r = Reader::new(body);
        let sc = r.le16()?;
        let got: [u8; KCK_LEN] = r.array()?;
        let want = self.confirm_hash(sc, false);
        let diff = got.iter().zip(&want).fold(0u8, |d, (a, b)| d | (a ^ b));
        if diff != 0 {
            return Err(Error::Invalid("SAE confirm mismatch"));
        }
        self.state = State::Accepted;
        Ok(())
    }

    /// PMK and PMKID, once the peer's Confirm was verified.
    pub fn keys(&self) -> Result<(&[u8; PMK_LEN], &[u8; PMKID_LEN])> {
        if self.state != State::Accepted {
            return Err(Error::Invalid("SAE state"));
        }
        Ok((&self.pmk, &self.pmkid))
    }

    #[cfg(test)]
    fn kck(&self) -> &[u8; KCK_LEN] {
        &self.kck
    }
}

/// Known-answer test against IEEE Std 802.11-2020 Annex J.10 (the vector
/// hostapd's `sae_tests` uses), for the kext self-test on the real target.
pub fn self_test() -> bool {
    const RAND: [u8; 32] = [
        0x99, 0x24, 0x65, 0xfd, 0x3d, 0xaa, 0x3c, 0x60, 0xaa, 0x65, 0x65, 0xb7, 0xf6, 0x2a, 0x2a,
        0x7f, 0x2e, 0x12, 0xdd, 0x12, 0xf1, 0x98, 0xfa, 0xf4, 0xfb, 0xed, 0x89, 0xd7, 0xff, 0x1a,
        0xce, 0x94,
    ];
    const MASK: [u8; 32] = [
        0x95, 0x07, 0xa9, 0x0f, 0x77, 0x7a, 0x04, 0x4d, 0x6a, 0x08, 0x30, 0xb9, 0x1e, 0xa3, 0xd5,
        0xdd, 0x70, 0xbe, 0xce, 0x44, 0xe1, 0xac, 0xff, 0xb8, 0x69, 0x83, 0xb5, 0xe1, 0xbf, 0x9f,
        0xb3, 0x22,
    ];
    const PEER_COMMIT: [u8; 98] = [
        0x13, 0x00, 0x59, 0x1b, 0x96, 0xf3, 0x39, 0x7f, 0xb9, 0x45, 0x10, 0x08, 0x48, 0xe7, 0xb5,
        0x50, 0x54, 0x3b, 0x67, 0x20, 0xd8, 0x83, 0x37, 0xee, 0x93, 0xfc, 0x49, 0xfd, 0x6d, 0xf7,
        0xe0, 0x8b, 0x52, 0x23, 0xe7, 0x1b, 0x9b, 0xb0, 0x48, 0xd3, 0x87, 0x3f, 0x20, 0x55, 0x69,
        0x53, 0xa9, 0x6c, 0x91, 0x53, 0x6f, 0xd8, 0xee, 0x6c, 0xa9, 0xb4, 0xa6, 0x8a, 0x14, 0x8b,
        0x05, 0x6a, 0x90, 0x9b, 0xe0, 0x3e, 0x83, 0xae, 0x20, 0x8f, 0x60, 0xf8, 0xef, 0x55, 0x37,
        0x85, 0x80, 0x74, 0xdb, 0x06, 0x68, 0x70, 0x32, 0x39, 0x98, 0x62, 0x99, 0x9b, 0x51, 0x1e,
        0x0a, 0x15, 0x52, 0xa5, 0xfe, 0xa3, 0x17, 0xc2,
    ];
    const PMK: [u8; 32] = [
        0x4e, 0x4d, 0xfa, 0xb1, 0xa2, 0xdd, 0x8a, 0xc1, 0xa9, 0x17, 0x90, 0xf9, 0x53, 0xfa, 0xaa,
        0x45, 0x2a, 0xe5, 0xc6, 0x87, 0x3a, 0xb7, 0x5b, 0x63, 0x60, 0x5b, 0xa6, 0x63, 0xf8, 0xa7,
        0xfe, 0x59,
    ];
    const COMMIT_HEAD: [u8; 16] = [
        0x13, 0x00, 0x2e, 0x2c, 0x0f, 0x0d, 0xb5, 0x24, 0x40, 0xad, 0x14, 0x6d, 0x96, 0x71, 0x14,
        0xce,
    ];
    const ADDR1: [u8; 6] = [0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87];
    const ADDR2: [u8; 6] = [0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c];
    let mut sae = Sae::empty();
    let mut commit = [0u8; COMMIT_LEN];
    let ok = sae.init(&ADDR1, &ADDR2, b"mekmitasdigoat").is_ok()
        && sae.commit(&RAND, &MASK).is_ok()
        && sae.write_commit(&mut commit) == Ok(COMMIT_LEN)
        && commit[..COMMIT_HEAD.len()] == COMMIT_HEAD
        && sae.process_commit(&PEER_COMMIT).is_ok()
        && sae.pmk == PMK;
    sae.clear();
    ok
}

#[cfg(test)]
mod tests {
    use super::*;

    fn unhex(s: &str) -> std::vec::Vec<u8> {
        let s: std::string::String = s.split_whitespace().collect();
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    fn arr32(s: &str) -> [u8; 32] {
        unhex(s).try_into().unwrap()
    }

    // IEEE Std 802.11-2020 Annex J.10, as in hostapd
    // src/common/common_module_tests.c (sae_tests).
    const ADDR1: [u8; 6] = [0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87];
    const ADDR2: [u8; 6] = [0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c];
    const PW: &[u8] = b"mekmitasdigoat";
    const RAND: &str = "992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94";
    const MASK: &str = "9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322";
    const LOCAL_COMMIT: &str =
        "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c65
        d5ad9e00829707aa36ba8b859738fc961d08243505f47c035376d7ac4bc8d7b95083bf43827d0fc31ed778dd3671
        fd21a46d1091d64b6f9a1e1272621325dbe1";
    const PEER_COMMIT: &str = "1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b5223
        e71b9bb048d3873f20556953a96c91536fd8ee6ca9b4a68a148b056a909be03e83ae208f60f8ef5537858074db06
        687032399862999b511e0a1552a5fea317c2";
    const KCK: &str = "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a";
    const PMK: &str = "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59";
    const PMKID: &str = "8747a600eea3f9f22475df58ca1e5498";

    fn committed(
        own: &[u8; 6],
        peer: &[u8; 6],
        pw: &[u8],
        rand: &[u8; 32],
        mask: &[u8; 32],
    ) -> Sae {
        let mut sae = Sae::empty();
        sae.init(own, peer, pw).unwrap();
        sae.commit(rand, mask).unwrap();
        sae
    }

    #[test]
    fn annex_j10_vector() {
        let mut sae = committed(&ADDR1, &ADDR2, PW, &arr32(RAND), &arr32(MASK));
        let mut buf = [0u8; 256];
        let n = sae.write_commit(&mut buf).unwrap();
        assert_eq!(&buf[..n], unhex(LOCAL_COMMIT).as_slice());

        sae.process_commit(&unhex(PEER_COMMIT)).unwrap();
        assert_eq!(sae.kck(), &arr32(KCK));
        assert_eq!(sae.pmk, arr32(PMK));
        assert_eq!(sae.pmkid.as_slice(), unhex(PMKID).as_slice());
    }

    fn exchange(pw_a: &[u8], pw_b: &[u8]) -> (Sae, Sae, bool) {
        let a_addr = [0x02, 0, 0, 0, 0, 1];
        let b_addr = [0x02, 0, 0, 0, 0, 2];
        let mut a = committed(&a_addr, &b_addr, pw_a, &arr32(RAND), &arr32(MASK));
        let mut rb = arr32(RAND);
        rb[31] ^= 0x55;
        let mut mb = arr32(MASK);
        mb[17] ^= 0xa5;
        let mut b = committed(&b_addr, &a_addr, pw_b, &rb, &mb);
        let mut ca = [0u8; COMMIT_LEN];
        let mut cb = [0u8; COMMIT_LEN];
        a.write_commit(&mut ca).unwrap();
        b.write_commit(&mut cb).unwrap();
        a.process_commit(&cb).unwrap();
        b.process_commit(&ca).unwrap();
        let mut fa = [0u8; CONFIRM_LEN];
        let mut fb = [0u8; CONFIRM_LEN];
        a.write_confirm(1, &mut fa).unwrap();
        b.write_confirm(1, &mut fb).unwrap();
        let ok = a.check_confirm(&fb).is_ok() && b.check_confirm(&fa).is_ok();
        (a, b, ok)
    }

    #[test]
    fn known_answer_self_test() {
        assert!(self_test());
    }

    #[test]
    fn full_exchange() {
        let (a, b, ok) = exchange(b"correct horse", b"correct horse");
        assert!(ok);
        let (pa, ia) = a.keys().unwrap();
        let (pb, ib) = b.keys().unwrap();
        assert_eq!(pa, pb);
        assert_eq!(ia, ib);
    }

    #[test]
    fn wrong_password_fails_confirm() {
        let (a, _, ok) = exchange(b"correct horse", b"battery staple");
        assert!(!ok);
        assert!(a.keys().is_err());
    }

    #[test]
    fn rejects_bad_commits() {
        let fresh = || committed(&ADDR1, &ADDR2, PW, &arr32(RAND), &arr32(MASK));
        let peer = unhex(PEER_COMMIT);

        // truncated, wrong group
        assert!(fresh().process_commit(&peer[..COMMIT_LEN - 1]).is_err());
        let mut bad = peer.clone();
        bad[0] = 20;
        assert!(fresh().process_commit(&bad).is_err());
        // scalar 0, 1 and >= r
        for s in [
            [0u8; 32],
            {
                let mut one = [0u8; 32];
                one[31] = 1;
                one
            },
            p256::to_be(&N),
        ] {
            let mut bad = peer.clone();
            bad[2..34].copy_from_slice(&s);
            assert!(fresh().process_commit(&bad).is_err());
        }
        // element off the curve
        let mut bad = peer.clone();
        bad[97] ^= 1;
        assert!(fresh().process_commit(&bad).is_err());
        // our own commit reflected back
        let mut sae = fresh();
        let mut own = [0u8; COMMIT_LEN];
        sae.write_commit(&mut own).unwrap();
        assert!(sae.process_commit(&own).is_err());
        // trailing elements are ignored
        let mut long = peer.clone();
        long.extend_from_slice(&[0xff, 0x03, 0x5c, 1, 2]);
        assert!(fresh().process_commit(&long).is_ok());
    }

    #[test]
    fn rejects_bad_random_and_state() {
        let mut sae = Sae::empty();
        assert!(sae.commit(&arr32(RAND), &arr32(MASK)).is_err());
        assert!(sae.init(&ADDR1, &ADDR1, PW).is_err());
        assert!(sae.init(&ADDR1, &ADDR2, b"").is_err());
        sae.init(&ADDR1, &ADDR2, PW).unwrap();
        let mut one = [0u8; 32];
        one[31] = 1;
        assert!(sae.commit(&one, &arr32(MASK)).is_err());
        assert!(sae.commit(&p256::to_be(&N), &arr32(MASK)).is_err());
        // confirm before the peer's commit
        let mut out = [0u8; CONFIRM_LEN];
        assert!(sae.write_confirm(1, &mut out).is_err());
    }

    #[test]
    fn anti_clogging_token() {
        let mut sae = committed(&ADDR1, &ADDR2, PW, &arr32(RAND), &arr32(MASK));
        assert!(sae.set_token(&[0x13, 0x00]).is_err());
        assert!(sae.set_token(&[0x14, 0x00, 1]).is_err());
        sae.set_token(&[0x13, 0x00, 0xaa, 0xbb, 0xcc]).unwrap();
        let mut buf = [0u8; 256];
        let n = sae.write_commit(&mut buf).unwrap();
        assert_eq!(n, COMMIT_LEN + 3);
        assert_eq!(&buf[..5], &[0x13, 0x00, 0xaa, 0xbb, 0xcc]);
        assert_eq!(&buf[5..n], &unhex(LOCAL_COMMIT)[2..]);
    }

    #[test]
    fn clear_zeroes_secrets() {
        let mut sae = committed(&ADDR1, &ADDR2, PW, &arr32(RAND), &arr32(MASK));
        sae.clear();
        assert_eq!(sae.state(), State::Empty);
        let p = &sae as *const Sae as *const u8;
        let bytes = unsafe { core::slice::from_raw_parts(p, core::mem::size_of::<Sae>()) };
        assert!(bytes.iter().all(|&b| b == 0));
    }
}
