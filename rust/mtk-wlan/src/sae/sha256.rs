//! SHA-256, HMAC-SHA-256 and the IEEE 802.11 KDF (12.7.1.6.2), for SAE.

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const H0: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

pub const DIGEST_LEN: usize = 32;
const BLOCK_LEN: usize = 64;

#[derive(Clone)]
pub struct Sha256 {
    h: [u32; 8],
    buf: [u8; BLOCK_LEN],
    used: usize,
    total: u64,
}

impl Default for Sha256 {
    fn default() -> Self {
        Self::new()
    }
}

impl Sha256 {
    pub const fn new() -> Self {
        Self {
            h: H0,
            buf: [0; BLOCK_LEN],
            used: 0,
            total: 0,
        }
    }

    fn compress(&mut self) {
        let mut w = [0u32; 64];
        for (i, word) in self.buf.as_chunks::<4>().0.iter().enumerate() {
            w[i] = u32::from_be_bytes(*word);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.h;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = h
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (s, v) in self.h.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *s = s.wrapping_add(v);
        }
    }

    pub fn update(&mut self, mut data: &[u8]) {
        self.total = self.total.wrapping_add(data.len() as u64);
        while !data.is_empty() {
            let n = (BLOCK_LEN - self.used).min(data.len());
            self.buf[self.used..self.used + n].copy_from_slice(&data[..n]);
            self.used += n;
            data = &data[n..];
            if self.used == BLOCK_LEN {
                self.compress();
                self.used = 0;
            }
        }
    }

    pub fn finish(mut self) -> [u8; DIGEST_LEN] {
        let bits = self.total.wrapping_mul(8);
        self.update(&[0x80]);
        while self.used != BLOCK_LEN - 8 {
            self.update(&[0]);
        }
        self.update(&bits.to_be_bytes());
        let mut out = [0u8; DIGEST_LEN];
        for (o, h) in out.as_chunks_mut::<4>().0.iter_mut().zip(self.h) {
            *o = h.to_be_bytes();
        }
        out
    }
}

/// HMAC-SHA-256 over the concatenation of `parts`.
pub fn hmac(key: &[u8], parts: &[&[u8]]) -> [u8; DIGEST_LEN] {
    let mut k = [0u8; BLOCK_LEN];
    if key.len() > BLOCK_LEN {
        let mut h = Sha256::new();
        h.update(key);
        k[..DIGEST_LEN].copy_from_slice(&h.finish());
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let mut pad = [0u8; BLOCK_LEN];
    for (p, b) in pad.iter_mut().zip(&k) {
        *p = b ^ 0x36;
    }
    let mut inner = Sha256::new();
    inner.update(&pad);
    for part in parts {
        inner.update(part);
    }
    let digest = inner.finish();
    for (p, b) in pad.iter_mut().zip(&k) {
        *p = b ^ 0x5c;
    }
    let mut outer = Sha256::new();
    outer.update(&pad);
    outer.update(&digest);
    k.fill(0);
    outer.finish()
}

/// KDF-SHA-256-`8 * out.len()` (IEEE 802.11-2020 12.7.1.6.2, hostapd's
/// `sha256_prf_bits` for whole bytes): HMAC blocks over
/// `i (LE16) || label || context || length in bits (LE16)`.
pub fn kdf(key: &[u8], label: &[u8], context: &[u8], out: &mut [u8]) {
    let bits = ((out.len() * 8) as u16).to_le_bytes();
    for (i, chunk) in out.chunks_mut(DIGEST_LEN).enumerate() {
        let counter = ((i + 1) as u16).to_le_bytes();
        let block = hmac(key, &[&counter, label, context, &bits]);
        chunk.copy_from_slice(&block[..chunk.len()]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> [u8; 32] {
        let mut out = [0u8; 32];
        for (i, o) in out.iter_mut().enumerate() {
            *o = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap();
        }
        out
    }

    #[test]
    fn sha256_vectors() {
        let mut h = Sha256::new();
        h.update(b"abc");
        assert_eq!(
            h.finish(),
            hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        );
        let mut h = Sha256::new();
        h.update(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
        assert_eq!(
            h.finish(),
            hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
        );
        let mut h = Sha256::new();
        for _ in 0..1000 {
            h.update(&[b'a'; 1000]);
        }
        assert_eq!(
            h.finish(),
            hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
        );
    }

    #[test]
    fn hmac_rfc4231() {
        // Test cases 1, 2 and 6 of RFC 4231.
        assert_eq!(
            hmac(&[0x0b; 20], &[b"Hi There"]),
            hex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")
        );
        assert_eq!(
            hmac(b"Jefe", &[b"what do ya want ", b"for nothing?"]),
            hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")
        );
        assert_eq!(
            hmac(
                &[0xaa; 131],
                &[b"Test Using Larger Than Block-Size Key - Hash Key First"]
            ),
            hex("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54")
        );
    }
}
