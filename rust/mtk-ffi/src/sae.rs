//! C ABI of the SAE (WPA3-Personal) core, `mtk_wlan_sae_*` in
//! `kext/Wifi/mtk_wlan.h`. The context lives in C memory
//! (`struct mtk_wlan_sae`, zeroed before `mtk_wlan_sae_init`); frame
//! bodies are what follows the Authentication frame's fixed fields.

use mtk_kernel::klog;
use mtk_wlan::sae::{CONFIRM_LEN, PMK_LEN, PMKID_LEN, Sae};

const EINVAL: i32 = 22;
const EAGAIN: i32 = 35; // Darwin errno
const EBADMSG: i32 = 94;
const EAUTH: i32 = 80;

pub const CTX_SIZE: usize = 512;

#[repr(C, align(8))]
pub struct MtkWlanSae {
    opaque: [u8; CTX_SIZE],
}

const _: () = assert!(core::mem::size_of::<Sae>() <= CTX_SIZE);
const _: () = assert!(core::mem::align_of::<Sae>() <= 8);

/// # Safety
/// `ctx` is NULL or points to a context zeroed by C or set up by
/// `mtk_wlan_sae_init`.
unsafe fn sae<'a>(ctx: *mut MtkWlanSae) -> Option<&'a mut Sae> {
    // SAFETY: size and alignment are asserted above; all-zero bytes are a
    // valid `Sae`, and only this module writes the storage.
    unsafe { ctx.cast::<Sae>().as_mut() }
}

unsafe fn bytes<'a>(data: *const u8, len: usize) -> Option<&'a [u8]> {
    if data.is_null() {
        return None;
    }
    // SAFETY: the caller passes a buffer of `len` bytes.
    Some(unsafe { core::slice::from_raw_parts(data, len) })
}

unsafe fn array<'a, const N: usize>(data: *const u8) -> Option<&'a [u8; N]> {
    // SAFETY: the caller passes N readable bytes.
    unsafe { data.cast::<[u8; N]>().as_ref() }
}

/// Clears the context and derives the PWE from both MAC addresses and the
/// password. Takes up to ~40 HMACs and modular exponentiations.
///
/// # Safety
/// `ctx` is writable `struct mtk_wlan_sae`; `own`, `peer` point to 6 bytes,
/// `password` to `password_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_init(
    ctx: *mut MtkWlanSae,
    own: *const u8,
    peer: *const u8,
    password: *const u8,
    password_len: usize,
) -> i32 {
    if ctx.is_null() {
        return EINVAL;
    }
    // SAFETY: `ctx` is writable storage of the right size and alignment.
    unsafe { ctx.cast::<Sae>().write(Sae::empty()) };
    let (Some(sae), Some(own), Some(peer), Some(pw)) = (
        unsafe { sae(ctx) },
        unsafe { array::<6>(own) },
        unsafe { array::<6>(peer) },
        unsafe { bytes(password, password_len) },
    ) else {
        return EINVAL;
    };
    match sae.init(own, peer, pw) {
        Ok(()) => 0,
        Err(e) => {
            klog!("SAE init rejected: {e:?}");
            EINVAL
        }
    }
}

/// Picks the commit scalar/element from two 32-byte random values.
/// `EAGAIN`: a value was out of range, draw new random bytes and retry.
///
/// # Safety
/// `ctx` was initialized; `rand` and `mask` point to 32 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_commit(
    ctx: *mut MtkWlanSae,
    rand: *const u8,
    mask: *const u8,
) -> i32 {
    let (Some(sae), Some(rand), Some(mask)) =
        (unsafe { sae(ctx) }, unsafe { array::<32>(rand) }, unsafe {
            array::<32>(mask)
        })
    else {
        return EINVAL;
    };
    match sae.commit(rand, mask) {
        Ok(()) => 0,
        Err(mtk_common::Error::Invalid("SAE random")) => EAGAIN,
        Err(e) => {
            klog!("SAE commit rejected: {e:?}");
            EINVAL
        }
    }
}

/// Writes the Commit body (group, token, scalar, element).
///
/// # Safety
/// `out` has `out_len` writable bytes; `written` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_write_commit(
    ctx: *mut MtkWlanSae,
    out: *mut u8,
    out_len: usize,
    written: *mut usize,
) -> i32 {
    let (Some(sae), Some(written)) = (unsafe { sae(ctx) }, unsafe { written.as_mut() }) else {
        return EINVAL;
    };
    if out.is_null() {
        return EINVAL;
    }
    // SAFETY: the caller passes `out_len` writable bytes.
    let out = unsafe { core::slice::from_raw_parts_mut(out, out_len) };
    match sae.write_commit(out) {
        Ok(n) => {
            *written = n;
            0
        }
        Err(e) => {
            klog!("SAE commit write failed: {e:?}");
            EINVAL
        }
    }
}

/// Keeps the anti-clogging token of an AP Commit with status 76.
///
/// # Safety
/// `body` points to `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_set_token(
    ctx: *mut MtkWlanSae,
    body: *const u8,
    len: usize,
) -> i32 {
    let (Some(sae), Some(body)) = (unsafe { sae(ctx) }, unsafe { bytes(body, len) }) else {
        return EINVAL;
    };
    match sae.set_token(body) {
        Ok(()) => 0,
        Err(e) => {
            klog!("SAE token rejected: {e:?}");
            EBADMSG
        }
    }
}

/// Checks the AP's Commit body (status 0) and derives KCK/PMK.
///
/// # Safety
/// `body` points to `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_process_commit(
    ctx: *mut MtkWlanSae,
    body: *const u8,
    len: usize,
) -> i32 {
    let (Some(sae), Some(body)) = (unsafe { sae(ctx) }, unsafe { bytes(body, len) }) else {
        return EINVAL;
    };
    match sae.process_commit(body) {
        Ok(()) => 0,
        Err(e) => {
            klog!("SAE peer commit rejected: {e:?}");
            EBADMSG
        }
    }
}

/// Writes the Confirm body (send-confirm, confirm).
///
/// # Safety
/// `out` has `out_len` writable bytes; `written` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_write_confirm(
    ctx: *mut MtkWlanSae,
    send_confirm: u16,
    out: *mut u8,
    out_len: usize,
    written: *mut usize,
) -> i32 {
    let (Some(sae), Some(written)) = (unsafe { sae(ctx) }, unsafe { written.as_mut() }) else {
        return EINVAL;
    };
    if out.is_null() || out_len < CONFIRM_LEN {
        return EINVAL;
    }
    // SAFETY: the caller passes `out_len` writable bytes.
    let out = unsafe { core::slice::from_raw_parts_mut(out, out_len) };
    match sae.write_confirm(send_confirm, out) {
        Ok(n) => {
            *written = n;
            0
        }
        Err(e) => {
            klog!("SAE confirm write failed: {e:?}");
            EINVAL
        }
    }
}

/// Verifies the AP's Confirm body. `EAUTH`: wrong password (or attack).
///
/// # Safety
/// `body` points to `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_check_confirm(
    ctx: *mut MtkWlanSae,
    body: *const u8,
    len: usize,
) -> i32 {
    let (Some(sae), Some(body)) = (unsafe { sae(ctx) }, unsafe { bytes(body, len) }) else {
        return EINVAL;
    };
    match sae.check_confirm(body) {
        Ok(()) => 0,
        Err(mtk_common::Error::Invalid("SAE confirm mismatch")) => EAUTH,
        Err(e) => {
            klog!("SAE peer confirm rejected: {e:?}");
            EBADMSG
        }
    }
}

/// Copies PMK (32 bytes) and PMKID (16 bytes) after a verified Confirm.
///
/// # Safety
/// `pmk` has 32 and `pmkid` 16 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_keys(
    ctx: *mut MtkWlanSae,
    pmk: *mut u8,
    pmkid: *mut u8,
) -> i32 {
    let (Some(sae), Some(pmk), Some(pmkid)) = (
        unsafe { sae(ctx) },
        unsafe { pmk.cast::<[u8; PMK_LEN]>().as_mut() },
        unsafe { pmkid.cast::<[u8; PMKID_LEN]>().as_mut() },
    ) else {
        return EINVAL;
    };
    match sae.keys() {
        Ok((k, id)) => {
            pmk.copy_from_slice(k);
            pmkid.copy_from_slice(id);
            0
        }
        Err(_) => EINVAL,
    }
}

/// Zeroes the context.
///
/// # Safety
/// `ctx` is NULL or a context set up as above.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_sae_clear(ctx: *mut MtkWlanSae) {
    if let Some(sae) = unsafe { sae(ctx) } {
        sae.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use mtk_wlan::sae::COMMIT_LEN;

    fn ctx() -> MtkWlanSae {
        MtkWlanSae {
            opaque: [0; CTX_SIZE],
        }
    }

    #[test]
    fn two_stations_agree() {
        let a_mac = [2u8, 0, 0, 0, 0, 1];
        let b_mac = [2u8, 0, 0, 0, 0, 2];
        let pw = b"wpa3 password";
        let (mut a, mut b) = (ctx(), ctx());
        unsafe {
            assert_eq!(
                mtk_wlan_sae_init(
                    &mut a,
                    a_mac.as_ptr(),
                    b_mac.as_ptr(),
                    pw.as_ptr(),
                    pw.len()
                ),
                0
            );
            assert_eq!(
                mtk_wlan_sae_init(
                    &mut b,
                    b_mac.as_ptr(),
                    a_mac.as_ptr(),
                    pw.as_ptr(),
                    pw.len()
                ),
                0
            );
            // Out of range (0) asks for new random bytes.
            assert_eq!(
                mtk_wlan_sae_commit(&mut a, [0u8; 32].as_ptr(), [7u8; 32].as_ptr()),
                EAGAIN
            );
            assert_eq!(
                mtk_wlan_sae_commit(&mut a, [3u8; 32].as_ptr(), [7u8; 32].as_ptr()),
                0
            );
            assert_eq!(
                mtk_wlan_sae_commit(&mut b, [5u8; 32].as_ptr(), [9u8; 32].as_ptr()),
                0
            );
            let (mut ca, mut cb) = ([0u8; COMMIT_LEN], [0u8; COMMIT_LEN]);
            let mut n = 0;
            assert_eq!(
                mtk_wlan_sae_write_commit(&mut a, ca.as_mut_ptr(), ca.len(), &mut n),
                0
            );
            assert_eq!(n, COMMIT_LEN);
            assert_eq!(
                mtk_wlan_sae_write_commit(&mut b, cb.as_mut_ptr(), cb.len(), &mut n),
                0
            );
            // Too small an output buffer is an error, not a panic.
            assert_eq!(
                mtk_wlan_sae_write_commit(&mut b, cb.as_mut_ptr(), 10, &mut n),
                EINVAL
            );
            assert_eq!(
                mtk_wlan_sae_process_commit(&mut a, cb.as_ptr(), 10),
                EBADMSG
            );
            assert_eq!(
                mtk_wlan_sae_process_commit(&mut a, cb.as_ptr(), cb.len()),
                0
            );
            assert_eq!(
                mtk_wlan_sae_process_commit(&mut b, ca.as_ptr(), ca.len()),
                0
            );
            let (mut fa, mut fb) = ([0u8; CONFIRM_LEN], [0u8; CONFIRM_LEN]);
            assert_eq!(
                mtk_wlan_sae_write_confirm(&mut a, 1, fa.as_mut_ptr(), fa.len(), &mut n),
                0
            );
            assert_eq!(
                mtk_wlan_sae_write_confirm(&mut b, 1, fb.as_mut_ptr(), fb.len(), &mut n),
                0
            );
            let mut bad = fb;
            bad[5] ^= 1;
            assert_eq!(
                mtk_wlan_sae_check_confirm(&mut a, bad.as_ptr(), bad.len()),
                EAUTH
            );
            assert_eq!(mtk_wlan_sae_check_confirm(&mut a, fb.as_ptr(), fb.len()), 0);
            assert_eq!(mtk_wlan_sae_check_confirm(&mut b, fa.as_ptr(), fa.len()), 0);
            let (mut pa, mut ia, mut pb, mut ib) = ([0u8; 32], [0u8; 16], [0u8; 32], [0u8; 16]);
            assert_eq!(
                mtk_wlan_sae_keys(&mut a, pa.as_mut_ptr(), ia.as_mut_ptr()),
                0
            );
            assert_eq!(
                mtk_wlan_sae_keys(&mut b, pb.as_mut_ptr(), ib.as_mut_ptr()),
                0
            );
            assert_eq!((pa, ia), (pb, ib));
            mtk_wlan_sae_clear(&mut a);
            assert_eq!(
                mtk_wlan_sae_keys(&mut a, pa.as_mut_ptr(), ia.as_mut_ptr()),
                EINVAL
            );
            assert!(a.opaque.iter().all(|&x| x == 0));
        }
    }

    #[test]
    fn null_arguments() {
        unsafe {
            let mut a = ctx();
            assert_eq!(
                mtk_wlan_sae_init(
                    core::ptr::null_mut(),
                    [0; 6].as_ptr(),
                    [1; 6].as_ptr(),
                    b"x".as_ptr(),
                    1
                ),
                EINVAL
            );
            assert_eq!(
                mtk_wlan_sae_init(&mut a, core::ptr::null(), [1; 6].as_ptr(), b"x".as_ptr(), 1),
                EINVAL
            );
            assert_eq!(
                mtk_wlan_sae_process_commit(&mut a, core::ptr::null(), 0),
                EINVAL
            );
            mtk_wlan_sae_clear(core::ptr::null_mut());
        }
    }
}
