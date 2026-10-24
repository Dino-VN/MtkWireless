//! NIST P-256 (SAE group 19): field arithmetic in Montgomery form on four
//! 64-bit limbs, complete projective point formulas (Renes-Costello-Batina
//! 2016, a = -3). No heap, no division, no floating point: kext safe.
//!
//! Scalar multiplication is double-and-add-always over all 256 bits with a
//! mask select; field exponents are public constants. This is not audited
//! constant-time code.

pub type Limbs = [u64; 4];
pub const BYTES: usize = 32;

/// Field prime p, little-endian limbs.
pub const P: Limbs = [
    0xffff_ffff_ffff_ffff,
    0x0000_0000_ffff_ffff,
    0x0000_0000_0000_0000,
    0xffff_ffff_0000_0001,
];
/// Group order r.
pub const N: Limbs = [
    0xf3b9_cac2_fc63_2551,
    0xbce6_faad_a717_9e84,
    0xffff_ffff_ffff_ffff,
    0xffff_ffff_0000_0000,
];
const B: Limbs = [
    0x3bce_3c3e_27d2_604b,
    0x651d_06b0_cc53_b0f6,
    0xb3eb_bd55_7698_86bc,
    0x5ac6_35d8_aa3a_93e7,
];

const fn adc(a: u64, b: u64, carry: u64) -> (u64, u64) {
    let t = a as u128 + b as u128 + carry as u128;
    (t as u64, (t >> 64) as u64)
}

/// a - b - borrow, borrow in and out is 0 or 1.
const fn sbb(a: u64, b: u64, borrow: u64) -> (u64, u64) {
    let t = (a as u128).wrapping_sub(b as u128 + borrow as u128);
    (t as u64, (t >> 127) as u64)
}

/// a + b * c + carry.
const fn mac(a: u64, b: u64, c: u64, carry: u64) -> (u64, u64) {
    let t = a as u128 + (b as u128) * (c as u128) + carry as u128;
    (t as u64, (t >> 64) as u64)
}

/// `mask` is all ones (take `a`) or zero (take `b`).
const fn select(mask: u64, a: &Limbs, b: &Limbs) -> Limbs {
    [
        (a[0] & mask) | (b[0] & !mask),
        (a[1] & mask) | (b[1] & !mask),
        (a[2] & mask) | (b[2] & !mask),
        (a[3] & mask) | (b[3] & !mask),
    ]
}

const fn add_raw(a: &Limbs, b: &Limbs) -> (Limbs, u64) {
    let (r0, c) = adc(a[0], b[0], 0);
    let (r1, c) = adc(a[1], b[1], c);
    let (r2, c) = adc(a[2], b[2], c);
    let (r3, c) = adc(a[3], b[3], c);
    ([r0, r1, r2, r3], c)
}

const fn sub_raw(a: &Limbs, b: &Limbs) -> (Limbs, u64) {
    let (r0, w) = sbb(a[0], b[0], 0);
    let (r1, w) = sbb(a[1], b[1], w);
    let (r2, w) = sbb(a[2], b[2], w);
    let (r3, w) = sbb(a[3], b[3], w);
    ([r0, r1, r2, r3], w)
}

/// (a + b) mod m for a, b < m.
pub const fn add_mod(a: &Limbs, b: &Limbs, m: &Limbs) -> Limbs {
    let (s, carry) = add_raw(a, b);
    let (t, borrow) = sub_raw(&s, m);
    // Keep s only when the sum fit and was below m.
    let keep_s = (carry ^ 1) & borrow;
    select(keep_s.wrapping_neg(), &s, &t)
}

/// (a - b) mod m for a, b < m.
pub const fn sub_mod(a: &Limbs, b: &Limbs, m: &Limbs) -> Limbs {
    let (d, borrow) = sub_raw(a, b);
    let mask = borrow.wrapping_neg();
    let fix = [m[0] & mask, m[1] & mask, m[2] & mask, m[3] & mask];
    add_raw(&d, &fix).0
}

/// a < b
pub const fn less_than(a: &Limbs, b: &Limbs) -> bool {
    sub_raw(a, b).1 == 1
}

pub const fn is_zero(a: &Limbs) -> bool {
    (a[0] | a[1] | a[2] | a[3]) == 0
}

const fn neg_inv(m0: u64) -> u64 {
    // Newton iteration for m0^-1 mod 2^64; m0 is odd.
    let mut x: u64 = 1;
    let mut i = 0;
    while i < 6 {
        x = x.wrapping_mul(2u64.wrapping_sub(m0.wrapping_mul(x)));
        i += 1;
    }
    x.wrapping_neg()
}

/// R^2 mod m, R = 2^256, m > 2^255.
const fn r2(m: &Limbs) -> Limbs {
    let mut r = sub_raw(&[0; 4], m).0; // 2^256 - m = R mod m
    let mut i = 0;
    while i < 256 {
        r = add_mod(&r, &r, m);
        i += 1;
    }
    r
}

/// Montgomery product a * b * R^-1 mod m (CIOS), a, b < m.
const fn mont_mul(a: &Limbs, b: &Limbs, m: &Limbs, minv: u64) -> Limbs {
    let mut t = [0u64; 6];
    let mut i = 0;
    while i < 4 {
        let mut c = 0;
        let mut j = 0;
        while j < 4 {
            let (v, cc) = mac(t[j], a[j], b[i], c);
            t[j] = v;
            c = cc;
            j += 1;
        }
        let (v, cc) = adc(t[4], c, 0);
        t[4] = v;
        t[5] = cc;
        let q = t[0].wrapping_mul(minv);
        let (_, mut c) = mac(t[0], q, m[0], 0);
        let mut j = 1;
        while j < 4 {
            let (v, cc) = mac(t[j], q, m[j], c);
            t[j - 1] = v;
            c = cc;
            j += 1;
        }
        let (v, cc) = adc(t[4], c, 0);
        t[3] = v;
        t[4] = t[5] + cc;
        t[5] = 0;
        i += 1;
    }
    let r = [t[0], t[1], t[2], t[3]];
    let (s, borrow) = sub_raw(&r, m);
    let keep_r = (t[4] ^ 1) & borrow;
    select(keep_r.wrapping_neg(), &r, &s)
}

pub const fn from_be(bytes: &[u8; BYTES]) -> Limbs {
    let mut out = [0u64; 4];
    let mut i = 0;
    while i < 4 {
        let mut v = 0u64;
        let mut j = 0;
        while j < 8 {
            v = (v << 8) | bytes[(3 - i) * 8 + j] as u64;
            j += 1;
        }
        out[i] = v;
        i += 1;
    }
    out
}

pub fn to_be(limbs: &Limbs) -> [u8; BYTES] {
    let mut out = [0u8; BYTES];
    for (i, chunk) in out.as_chunks_mut::<8>().0.iter_mut().enumerate() {
        *chunk = limbs[3 - i].to_be_bytes();
    }
    out
}

const P_INV: u64 = neg_inv(P[0]);
const P_R2: Limbs = r2(&P);

/// Field element mod p in Montgomery form.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fe(Limbs);

// By-value field operations, deliberately not the `core::ops` traits.
#[allow(clippy::should_implement_trait)]
impl Fe {
    pub const ZERO: Fe = Fe([0; 4]);
    pub const ONE: Fe = Fe(mont_mul(&[1, 0, 0, 0], &P_R2, &P, P_INV));
    const B: Fe = Fe(mont_mul(&B, &P_R2, &P, P_INV));

    /// `None` unless the big-endian value is below p.
    pub fn from_bytes(bytes: &[u8; BYTES]) -> Option<Fe> {
        let v = from_be(bytes);
        if !less_than(&v, &P) {
            return None;
        }
        Some(Fe(mont_mul(&v, &P_R2, &P, P_INV)))
    }

    pub fn to_bytes(self) -> [u8; BYTES] {
        to_be(&mont_mul(&self.0, &[1, 0, 0, 0], &P, P_INV))
    }

    pub fn add(self, o: Fe) -> Fe {
        Fe(add_mod(&self.0, &o.0, &P))
    }
    pub fn sub(self, o: Fe) -> Fe {
        Fe(sub_mod(&self.0, &o.0, &P))
    }
    pub fn neg(self) -> Fe {
        Fe(sub_mod(&[0; 4], &self.0, &P))
    }
    pub fn mul(self, o: Fe) -> Fe {
        Fe(mont_mul(&self.0, &o.0, &P, P_INV))
    }
    pub fn square(self) -> Fe {
        self.mul(self)
    }
    pub fn is_zero(self) -> bool {
        is_zero(&self.0)
    }
    /// Parity of the canonical (non-Montgomery) value.
    pub fn is_odd(self) -> bool {
        mont_mul(&self.0, &[1, 0, 0, 0], &P, P_INV)[0] & 1 == 1
    }
    pub fn select(mask: u64, a: Fe, b: Fe) -> Fe {
        Fe(select(mask, &a.0, &b.0))
    }

    /// self^e for a public exponent.
    fn pow(self, e: &Limbs) -> Fe {
        let mut r = Fe::ONE;
        for i in (0..256).rev() {
            r = r.square();
            if (e[i / 64] >> (i % 64)) & 1 == 1 {
                r = r.mul(self);
            }
        }
        r
    }

    /// Inverse by Fermat; zero maps to zero.
    pub fn invert(self) -> Fe {
        const E: Limbs = sub_raw(&P, &[2, 0, 0, 0]).0;
        self.pow(&E)
    }

    /// Euler's criterion: true for a non-zero square.
    pub fn is_square(self) -> bool {
        // (p - 1) / 2
        const E: Limbs = shr1(&sub_raw(&P, &[1, 0, 0, 0]).0);
        self.pow(&E) == Fe::ONE
    }

    /// A square root (p = 3 mod 4), or `None`.
    pub fn sqrt(self) -> Option<Fe> {
        // (p + 1) / 4
        const E: Limbs = shr1(&shr1(&add_raw(&P, &[1, 0, 0, 0]).0));
        let r = self.pow(&E);
        if r.square() == self { Some(r) } else { None }
    }

    /// x^3 - 3x + b
    pub fn curve_rhs(x: Fe) -> Fe {
        let three_x = x.add(x).add(x);
        x.square().mul(x).sub(three_x).add(Fe::B)
    }
}

const fn shr1(a: &Limbs) -> Limbs {
    [
        (a[0] >> 1) | (a[1] << 63),
        (a[1] >> 1) | (a[2] << 63),
        (a[2] >> 1) | (a[3] << 63),
        a[3] >> 1,
    ]
}

/// Projective point (X : Y : Z), x = X/Z, y = Y/Z; identity is (0 : 1 : 0).
#[derive(Clone, Copy, Debug)]
pub struct Point {
    x: Fe,
    y: Fe,
    z: Fe,
}

impl Point {
    pub const IDENTITY: Point = Point {
        x: Fe::ZERO,
        y: Fe::ONE,
        z: Fe::ZERO,
    };

    /// Affine point; the caller checks it is on the curve.
    pub fn from_affine(x: Fe, y: Fe) -> Point {
        Point { x, y, z: Fe::ONE }
    }

    /// Parses x || y (big-endian), rejecting values >= p and points off
    /// the curve.
    pub fn from_bytes(x: &[u8; BYTES], y: &[u8; BYTES]) -> Option<Point> {
        let x = Fe::from_bytes(x)?;
        let y = Fe::from_bytes(y)?;
        if y.square() != Fe::curve_rhs(x) {
            return None;
        }
        Some(Point::from_affine(x, y))
    }

    pub fn is_identity(&self) -> bool {
        self.z.is_zero()
    }

    /// Affine (x, y), or `None` for the identity.
    pub fn to_affine(&self) -> Option<(Fe, Fe)> {
        if self.is_identity() {
            return None;
        }
        let zi = self.z.invert();
        Some((self.x.mul(zi), self.y.mul(zi)))
    }

    pub fn neg(&self) -> Point {
        Point {
            x: self.x,
            y: self.y.neg(),
            z: self.z,
        }
    }

    fn select(mask: u64, a: &Point, b: &Point) -> Point {
        Point {
            x: Fe::select(mask, a.x, b.x),
            y: Fe::select(mask, a.y, b.y),
            z: Fe::select(mask, a.z, b.z),
        }
    }

    /// Complete addition, also valid for doubling and the identity
    /// (Renes-Costello-Batina, Algorithm 4).
    pub fn add(&self, q: &Point) -> Point {
        let (x1, y1, z1) = (self.x, self.y, self.z);
        let (x2, y2, z2) = (q.x, q.y, q.z);
        let b = Fe::B;
        let mut t0 = x1.mul(x2);
        let mut t1 = y1.mul(y2);
        let mut t2 = z1.mul(z2);
        let mut t3 = x1.add(y1);
        let mut t4 = x2.add(y2);
        t3 = t3.mul(t4);
        t4 = t0.add(t1);
        t3 = t3.sub(t4);
        t4 = y1.add(z1);
        let mut x3 = y2.add(z2);
        t4 = t4.mul(x3);
        x3 = t1.add(t2);
        t4 = t4.sub(x3);
        x3 = x1.add(z1);
        let mut y3 = x2.add(z2);
        x3 = x3.mul(y3);
        y3 = t0.add(t2);
        y3 = x3.sub(y3);
        let mut z3 = b.mul(t2);
        x3 = y3.sub(z3);
        z3 = x3.add(x3);
        x3 = x3.add(z3);
        z3 = t1.sub(x3);
        x3 = t1.add(x3);
        y3 = b.mul(y3);
        t1 = t2.add(t2);
        t2 = t1.add(t2);
        y3 = y3.sub(t2);
        y3 = y3.sub(t0);
        t1 = y3.add(y3);
        y3 = t1.add(y3);
        t1 = t0.add(t0);
        t0 = t1.add(t0);
        t0 = t0.sub(t2);
        t1 = t4.mul(y3);
        t2 = t0.mul(y3);
        y3 = x3.mul(z3);
        y3 = y3.add(t2);
        x3 = t3.mul(x3);
        x3 = x3.sub(t1);
        z3 = t4.mul(z3);
        t1 = t3.mul(t0);
        z3 = z3.add(t1);
        Point {
            x: x3,
            y: y3,
            z: z3,
        }
    }

    /// k * self for a 256-bit scalar (big-endian limbs order as `Limbs`).
    pub fn mul(&self, k: &Limbs) -> Point {
        let mut r = Point::IDENTITY;
        for i in (0..256).rev() {
            r = r.add(&r);
            let t = r.add(self);
            let bit = (k[i / 64] >> (i % 64)) & 1;
            r = Point::select(bit.wrapping_neg(), &t, &r);
        }
        r
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex32(s: &str) -> [u8; 32] {
        let mut out = [0u8; 32];
        for (i, o) in out.iter_mut().enumerate() {
            *o = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap();
        }
        out
    }

    fn generator() -> Point {
        Point::from_bytes(
            &hex32("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"),
            &hex32("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"),
        )
        .unwrap()
    }

    #[test]
    fn constants() {
        assert_eq!(P_INV, 1);
        assert_eq!(Fe::ONE.to_bytes()[31], 1);
        let b = Fe::B.to_bytes();
        assert_eq!(from_be(&b), B);
    }

    #[test]
    fn field_inverse_and_sqrt() {
        let a = Fe::from_bytes(&hex32(
            "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
        ))
        .unwrap();
        assert_eq!(a.mul(a.invert()), Fe::ONE);
        let s = a.square();
        assert!(s.is_square());
        let r = s.sqrt().unwrap();
        assert!(r == a || r == a.neg());
        // -1 is not a square mod p (p = 3 mod 4).
        assert!(!Fe::ONE.neg().is_square());
        assert!(Fe::ONE.neg().sqrt().is_none());
    }

    #[test]
    fn rejects_out_of_range_and_off_curve() {
        assert!(Fe::from_bytes(&to_be(&P)).is_none());
        let g = generator();
        let (x, y) = g.to_affine().unwrap();
        let mut bad = y.to_bytes();
        bad[31] ^= 1;
        assert!(Point::from_bytes(&x.to_bytes(), &bad).is_none());
    }

    #[test]
    fn scalar_multiplication() {
        let g = generator();
        // 2G and 3G from SEC test data.
        let (x2, y2) = g.add(&g).to_affine().unwrap();
        assert_eq!(
            x2.to_bytes(),
            hex32("7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978")
        );
        assert_eq!(
            y2.to_bytes(),
            hex32("07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1")
        );
        let (x3, _) = g.mul(&[3, 0, 0, 0]).to_affine().unwrap();
        assert_eq!(
            x3.to_bytes(),
            hex32("5ecbe4d1a6330a44c8f7ef951d4bf165e6c6b721efada985fb41661bc6e7fd6c")
        );
        // n * G is the identity, (n - 1) * G is -G.
        assert!(g.mul(&N).is_identity());
        let (xm, ym) = g.mul(&sub_raw(&N, &[1, 0, 0, 0]).0).to_affine().unwrap();
        let (xg, yg) = g.to_affine().unwrap();
        assert_eq!(xm, xg);
        assert_eq!(ym, yg.neg());
        // identity handling
        assert!(Point::IDENTITY.add(&Point::IDENTITY).is_identity());
        assert!(g.add(&g.neg()).is_identity());
        let (xi, _) = Point::IDENTITY.add(&g).to_affine().unwrap();
        assert_eq!(xi, xg);
    }

    #[test]
    fn scalar_ops_mod_n() {
        let a = sub_raw(&N, &[1, 0, 0, 0]).0;
        assert_eq!(add_mod(&a, &[2, 0, 0, 0], &N), [1, 0, 0, 0]);
        assert_eq!(sub_mod(&[1, 0, 0, 0], &[2, 0, 0, 0], &N), a);
    }
}
