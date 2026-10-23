//! C ABI of the Wi-Fi core. Header: `kext/Wifi/mtk_wlan.h`.
//!
//! Functions return 0 or an errno value (`EINVAL`), like the mwx HAL.

use core::ffi::c_char;

use mtk_kernel::klog;
use mtk_wlan::connac2::fw::{PatchImage, RamImage, Region};

const EINVAL: i32 = 22;

#[repr(C)]
pub struct MtkWlanFwRegion {
    pub addr: u32,
    pub len: u32,
    pub mode: u32,
    pub offset: usize,
    pub feature: u8,
    pub download: bool,
}

#[repr(C)]
pub struct MtkWlanPatchInfo {
    pub hw_sw_ver: u32,
    pub patch_ver: u32,
    pub regions: u32,
    pub build_date: [c_char; 17],
}

#[repr(C)]
pub struct MtkWlanRamInfo {
    pub chip_id: u8,
    pub eco_code: u8,
    pub regions: u8,
    pub fw_ver: [c_char; 11],
    pub build_date: [c_char; 16],
    pub override_addr: u32,
    pub start_option: u32,
}

unsafe fn image<'a>(data: *const u8, len: usize) -> &'a [u8] {
    if data.is_null() || len == 0 {
        &[]
    } else {
        // SAFETY: the caller passes a buffer of `len` bytes.
        unsafe { core::slice::from_raw_parts(data, len) }
    }
}

/// Copies ASCII text, stops at NUL, always terminates.
fn text(dst: &mut [c_char], src: &[u8]) {
    let n = src
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(src.len())
        .min(dst.len() - 1);
    for (d, &s) in dst.iter_mut().zip(&src[..n]) {
        *d = if s.is_ascii_graphic() || s == b' ' {
            s as c_char
        } else {
            b'?' as c_char
        };
    }
    dst[n] = 0;
}

fn region_out(r: &Region<'_>, out: &mut MtkWlanFwRegion) {
    *out = MtkWlanFwRegion {
        addr: r.addr,
        len: r.data.len() as u32,
        mode: r.mode,
        offset: r.offset,
        feature: r.feature,
        download: r.download,
    };
}

/// # Safety
/// `data` points to `len` bytes; `info` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_patch_parse(
    data: *const u8,
    len: usize,
    info: *mut MtkWlanPatchInfo,
) -> i32 {
    let Some(info) = (unsafe { info.as_mut() }) else {
        return EINVAL;
    };
    match PatchImage::parse(unsafe { image(data, len) }) {
        Ok(img) => {
            info.hw_sw_ver = img.hw_sw_ver;
            info.patch_ver = img.patch_ver;
            info.regions = img.regions;
            text(&mut info.build_date, &img.build_date);
            0
        }
        Err(e) => {
            klog!("ROM patch rejected: {e:?}");
            EINVAL
        }
    }
}

/// # Safety
/// `data` points to `len` bytes; `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_patch_region(
    data: *const u8,
    len: usize,
    index: u32,
    out: *mut MtkWlanFwRegion,
) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return EINVAL;
    };
    match PatchImage::parse(unsafe { image(data, len) }).and_then(|img| img.region(index)) {
        Ok(r) => {
            region_out(&r, out);
            0
        }
        Err(e) => {
            klog!("ROM patch region {index} rejected: {e:?}");
            EINVAL
        }
    }
}

/// # Safety
/// `data` points to `len` bytes; `info` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_ram_parse(
    data: *const u8,
    len: usize,
    info: *mut MtkWlanRamInfo,
) -> i32 {
    let Some(info) = (unsafe { info.as_mut() }) else {
        return EINVAL;
    };
    let parsed = RamImage::parse(unsafe { image(data, len) })
        .and_then(|img| Ok((img, img.override_addr()?, img.start_option()?)));
    match parsed {
        Ok((img, override_addr, start_option)) => {
            info.chip_id = img.chip_id;
            info.eco_code = img.eco_code;
            info.regions = img.regions;
            text(&mut info.fw_ver, &img.fw_ver);
            text(&mut info.build_date, &img.build_date);
            info.override_addr = override_addr;
            info.start_option = start_option;
            0
        }
        Err(e) => {
            klog!("RAM image rejected: {e:?}");
            EINVAL
        }
    }
}

/// # Safety
/// `data` points to `len` bytes; `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_ram_region(
    data: *const u8,
    len: usize,
    index: u32,
    out: *mut MtkWlanFwRegion,
) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return EINVAL;
    };
    let Ok(index) = u8::try_from(index) else {
        return EINVAL;
    };
    match RamImage::parse(unsafe { image(data, len) }).and_then(|img| img.region(index)) {
        Ok(r) => {
            region_out(&r, out);
            0
        }
        Err(e) => {
            klog!("RAM region {index} rejected: {e:?}");
            EINVAL
        }
    }
}

#[repr(C)]
pub struct MtkWlanRxInfo {
    pub rxd: [u32; 5],
    pub hdr_gap: usize,
    pub rate: u32,
    pub wlan_idx: u16,
    pub channel: u8,
    pub unicast: bool,
    pub hw_decrypted: bool,
    pub has_rssi: bool,
    pub rssi: i8,
}

/// Parses the RX descriptor at the start of `frame`. Returns 0, or a drop
/// reason (`mtk_wlan_rx_drop`). On a drop, `info.rxd` holds whatever of the
/// fixed part was present (for logging) and the other fields are zero.
///
/// # Safety
/// `frame` points to `len` bytes; `info` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_rx_parse(
    frame: *const u8,
    len: usize,
    num_streams: u8,
    antenna_mask: u32,
    info: *mut MtkWlanRxInfo,
) -> u32 {
    use mtk_wlan::connac2::rxd;
    let Some(info) = (unsafe { info.as_mut() }) else {
        return rxd::Drop::ShortRxd as u32;
    };
    let frame = unsafe { image(frame, len) };
    let mut words = [0u32; 5];
    for (i, w) in words.iter_mut().enumerate() {
        *w = mtk_common::bytes::le32_at(frame, i * 4).unwrap_or(0);
    }
    *info = MtkWlanRxInfo {
        rxd: words,
        hdr_gap: 0,
        rate: 0,
        wlan_idx: 0,
        channel: 0,
        unicast: false,
        hw_decrypted: false,
        has_rssi: false,
        rssi: 0,
    };
    match rxd::parse(frame, num_streams, antenna_mask) {
        Ok(rx) => {
            info.hdr_gap = rx.hdr_gap;
            info.rate = rx.rate;
            info.wlan_idx = rx.wlan_idx;
            info.channel = rx.channel;
            info.unicast = rx.unicast;
            info.hw_decrypted = rx.hw_decrypted;
            info.has_rssi = rx.rssi.is_some();
            info.rssi = rx.rssi.unwrap_or(0);
            0
        }
        Err(drop) => drop as u32,
    }
}

/// Short name of a drop reason, for logs.
#[unsafe(no_mangle)]
pub extern "C" fn mtk_wlan_rx_drop_name(reason: u32) -> *const c_char {
    let name: &core::ffi::CStr = match reason {
        0 => c"ok",
        1 => c"short-rxd",
        2 => c"band-idx",
        3 => c"amsdu-err",
        4 => c"hdr-trans",
        5 => c"icv",
        6 => c"fcs",
        7 => c"tkip-mic",
        8 => c"max-len",
        9 => c"short-groups",
        10 => c"short-80211",
        _ => c"?",
    };
    name.as_ptr()
}

/// Packet class of an RX ring buffer. Returns false for a packet shorter
/// than one dword.
///
/// # Safety
/// `packet` points to `len` bytes; outputs are writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_rx_classify(
    packet: *const u8,
    len: usize,
    kind: *mut u32,
    flag: *mut u32,
    rxd0: *mut u32,
) -> bool {
    let Some(class) = mtk_wlan::connac2::event::classify(unsafe { image(packet, len) }) else {
        return false;
    };
    unsafe {
        if let Some(k) = kind.as_mut() {
            *k = class.kind;
        }
        if let Some(f) = flag.as_mut() {
            *f = class.flag;
        }
        if let Some(r) = rxd0.as_mut() {
            *r = class.rxd0;
        }
    }
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn mtk_wlan_txs_records(len: usize) -> usize {
    mtk_wlan::connac2::event::txs_records(len)
}

#[repr(C)]
pub struct MtkWlanTxFreeIter {
    words: *const u8,
    len: usize,
    index: usize,
    count: usize,
}

#[repr(C)]
pub struct MtkWlanTxFreed {
    pub msdu: u16,
    pub status: u8,
    pub count: u16,
    pub raw: u32,
}

/// Starts iterating the MSDUs of a TXRX_NOTIFY packet. The packet must stay
/// valid while iterating.
///
/// # Safety
/// `packet` points to `len` bytes; `it` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_tx_free_begin(
    packet: *const u8,
    len: usize,
    it: *mut MtkWlanTxFreeIter,
) {
    let Some(it) = (unsafe { it.as_mut() }) else {
        return;
    };
    let (words, index, count) =
        mtk_wlan::connac2::event::TxFree::new(unsafe { image(packet, len) }).into_parts();
    *it = MtkWlanTxFreeIter {
        words: words.as_ptr(),
        len: words.len(),
        index,
        count,
    };
}

/// Next released MSDU; false at the end.
///
/// # Safety
/// `it` was set up by `mtk_wlan_tx_free_begin` and its packet is still valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_tx_free_next(
    it: *mut MtkWlanTxFreeIter,
    out: *mut MtkWlanTxFreed,
) -> bool {
    let (Some(it), Some(out)) = (unsafe { it.as_mut() }, unsafe { out.as_mut() }) else {
        return false;
    };
    let words = unsafe { image(it.words, it.len) };
    let mut iter = mtk_wlan::connac2::event::TxFree::from_parts(words, it.index, it.count);
    let next = iter.next();
    let (_, index, count) = iter.into_parts();
    it.index = index;
    it.count = count;
    match next {
        Some(f) => {
            *out = MtkWlanTxFreed {
                msdu: f.msdu,
                status: f.status,
                count: f.count,
                raw: f.raw,
            };
            true
        }
        None => false,
    }
}

/// Validates a ring index read from the hardware.
///
/// # Safety
/// `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_dma_hw_index(raw: u32, count: u32, out: *mut u32) -> bool {
    match (mtk_wlan::connac2::dma::hw_index(raw, count), unsafe {
        out.as_mut()
    }) {
        (Some(i), Some(out)) => {
            *out = i;
            true
        }
        _ => false,
    }
}

/// Length/end flag of a completed RX descriptor; false when the length does
/// not fit the posted buffer.
///
/// # Safety
/// Outputs are writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_dma_rx_segment(
    ctrl: u32,
    capacity: u32,
    len: *mut u32,
    last: *mut bool,
) -> bool {
    let (Some(len), Some(last)) = (unsafe { len.as_mut() }, unsafe { last.as_mut() }) else {
        return false;
    };
    match mtk_wlan::connac2::dma::rx_segment(ctrl, capacity) {
        Some(seg) => {
            *len = seg.len;
            *last = seg.last;
            true
        }
        None => false,
    }
}
