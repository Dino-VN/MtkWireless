//! C ABI of the Wi-Fi core. Header: `kext/Wifi/mtk_wlan.h`.
//!
//! Functions return 0 or an errno value (`EINVAL`), like the mwx HAL.

use core::ffi::c_char;

use mtk_kernel::klog;
use mtk_wlan::connac2::capability::PhyCaps;
use mtk_wlan::connac2::fw::{FwFeatures, PatchImage, RamImage, Region};

const EINVAL: i32 = 22;

/// # Safety
/// `data` points to `len` bytes; `out` is writable and does not overlap it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_scan_ssids_parse(
    data: *const u8,
    len: usize,
    out: *mut mtk_wlan::scan::ScanSsids,
) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return EINVAL;
    };
    *out = mtk_wlan::scan::ScanSsids::default();
    match mtk_wlan::scan::ScanSsids::parse_wcl(unsafe { image(data, len) }) {
        Ok(targets) => {
            *out = targets;
            0
        }
        Err(_) => EINVAL,
    }
}

/// # Safety
/// `data` points to `len` bytes; `out` is writable and does not overlap it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_eapol_observe(
    data: *const u8,
    len: usize,
    out: *mut mtk_wlan::join::EapolObservation,
) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return EINVAL;
    };
    *out = mtk_wlan::join::EapolObservation::default();
    match mtk_wlan::join::EapolObservation::parse(unsafe { image(data, len) }) {
        Ok(observation) => {
            *out = observation;
            0
        }
        Err(_) => EINVAL,
    }
}

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

/// Read optional diagnostic firmware features without changing RAM loading.
/// A missing feature is success with `present = false`; malformed data is
/// EINVAL. The output is cleared before parsing, including on error.
///
/// # Safety
/// `data` points to `len` bytes; `out` is writable and does not overlap it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_ram_features(
    data: *const u8,
    len: usize,
    out: *mut FwFeatures,
) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return EINVAL;
    };
    *out = FwFeatures::default();
    match RamImage::parse(unsafe { image(data, len) }).and_then(|img| img.features()) {
        Ok(features) => {
            *out = features;
            0
        }
        Err(_) => EINVAL,
    }
}

/// Copy the raw fields of a PHY capability TLV's payload. This does not infer
/// simultaneous RF capability. On error the output is cleared.
///
/// # Safety
/// `data` points to `len` bytes; `out` is writable and does not overlap it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_phy_caps_parse(
    data: *const u8,
    len: usize,
    out: *mut PhyCaps,
) -> i32 {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return EINVAL;
    };
    *out = PhyCaps::default();
    match PhyCaps::parse(unsafe { image(data, len) }) {
        Ok(caps) => {
            *out = caps;
            0
        }
        Err(_) => EINVAL,
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

/// Raw TXS fields only: this record has no DMA-token lifetime identity.
///
/// # Safety
/// `record` points to `len` readable bytes; `out` is writable when nonnull.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_wlan_txs_parse(
    record: *const u8,
    len: usize,
    out: *mut mtk_wlan::connac2::event::TxStatus,
) -> bool {
    let Some(out) = (unsafe { out.as_mut() }) else {
        return false;
    };
    *out = Default::default();
    match mtk_wlan::connac2::event::parse_txs(unsafe { image(record, len) }) {
        Ok(status) => {
            *out = status;
            true
        }
        Err(_) => false,
    }
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

#[cfg(test)]
mod capability_tests {
    use super::*;

    const FEATURES: FwFeatures = FwFeatures {
        present: true,
        segment: 2,
        flags: 0xc0,
    };

    fn feature_metadata() -> Vec<u8> {
        let mut data = vec![b'#'; 16];
        data.extend_from_slice(&[20, 0, 0, 0, 16, 0, 0, 4, 2, 0xc0]);
        data.extend_from_slice(&[0; 14]);
        data
    }

    fn ram_image(metadata: &[u8]) -> Vec<u8> {
        let mut data = vec![0xaa; 3];
        data.extend_from_slice(metadata);
        let mut region = [0; 40];
        region[16..20].copy_from_slice(&0x9000_0000u32.to_le_bytes());
        region[20..24].copy_from_slice(&3u32.to_le_bytes());
        region[24] = mtk_wlan::connac2::fw::FW_FEATURE_OVERRIDE_ADDR;
        data.extend_from_slice(&region);
        let mut trailer = [0; 36];
        trailer[2] = 1;
        data.extend_from_slice(&trailer);
        data
    }

    #[test]
    fn c_abi_layouts_and_function_signatures() {
        use core::mem::{align_of, offset_of, size_of};
        assert_eq!(size_of::<FwFeatures>(), 3);
        assert_eq!(align_of::<FwFeatures>(), 1);
        assert_eq!(offset_of!(FwFeatures, present), 0);
        assert_eq!(offset_of!(FwFeatures, segment), 1);
        assert_eq!(offset_of!(FwFeatures, flags), 2);
        assert_eq!(size_of::<PhyCaps>(), 12);
        assert_eq!(align_of::<PhyCaps>(), 1);
        assert_eq!(offset_of!(PhyCaps, nss), 4);
        assert_eq!(offset_of!(PhyCaps, dbdc), 5);
        assert_eq!(offset_of!(PhyCaps, hw_path), 10);
        assert_eq!(offset_of!(PhyCaps, he), 11);
        assert_eq!(mtk_wlan::connac2::fw::FW_CAP_CNM, 0x80);
        let _: unsafe extern "C" fn(*const u8, usize, *mut FwFeatures) -> i32 =
            mtk_wlan_ram_features;
        let _: unsafe extern "C" fn(*const u8, usize, *mut PhyCaps) -> i32 =
            mtk_wlan_phy_caps_parse;

        // The separate diagnostic API must not grow/change the download ABI.
        assert_eq!(size_of::<MtkWlanRamInfo>(), 40);
        assert_eq!(offset_of!(MtkWlanRamInfo, chip_id), 0);
        assert_eq!(offset_of!(MtkWlanRamInfo, eco_code), 1);
        assert_eq!(offset_of!(MtkWlanRamInfo, regions), 2);
        assert_eq!(offset_of!(MtkWlanRamInfo, fw_ver), 3);
        assert_eq!(offset_of!(MtkWlanRamInfo, build_date), 14);
        assert_eq!(offset_of!(MtkWlanRamInfo, override_addr), 32);
        assert_eq!(offset_of!(MtkWlanRamInfo, start_option), 36);
    }

    #[test]
    fn ram_features_ffi_reads_shipped_images() {
        for name in ["mwx-mt7922_ram_code_1", "mwx-mt7961_ram_code_1"] {
            let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../firmware/wifi")
                .join(name);
            let data = std::fs::read(path).unwrap();
            let mut out = FwFeatures::default();
            assert_eq!(
                unsafe { mtk_wlan_ram_features(data.as_ptr(), data.len(), &mut out) },
                0
            );
            assert_eq!(out, FEATURES, "fixture {name}");
        }
    }

    #[test]
    fn ram_features_ffi_distinguishes_present_and_absent() {
        let metadata = feature_metadata();
        let data = ram_image(&metadata);
        let mut out = FwFeatures::default();
        assert_eq!(
            unsafe { mtk_wlan_ram_features(data.as_ptr(), data.len(), &mut out) },
            0
        );
        assert_eq!(out, FEATURES);
        let mut no_feature = metadata.clone();
        no_feature[23] = 0x99;
        for metadata in [vec![], no_feature] {
            let data = ram_image(&metadata);
            out = FEATURES;
            assert_eq!(
                unsafe { mtk_wlan_ram_features(data.as_ptr(), data.len(), &mut out) },
                0
            );
            assert_eq!(out, FwFeatures::default());
        }
        let mut zero_flags = metadata;
        zero_flags[25] = 0;
        let data = ram_image(&zero_flags);
        assert_eq!(
            unsafe { mtk_wlan_ram_features(data.as_ptr(), data.len(), &mut out) },
            0
        );
        assert_eq!(
            out,
            FwFeatures {
                flags: 0,
                ..FEATURES
            }
        );
    }

    #[test]
    fn ram_features_ffi_clears_output_on_all_errors() {
        let metadata = feature_metadata();
        let good = ram_image(&metadata);
        let mut out = FEATURES;
        for len in [0, 1, usize::MAX] {
            assert_eq!(
                unsafe { mtk_wlan_ram_features(core::ptr::null(), len, &mut out) },
                EINVAL
            );
            assert_eq!(out, FwFeatures::default());
            out = FEATURES;
        }
        assert_eq!(
            unsafe { mtk_wlan_ram_features(good.as_ptr(), good.len(), core::ptr::null_mut()) },
            EINVAL
        );
        let mut failures = vec![vec![], vec![0; 35]];
        // The descriptor table stays intact, so these are metadata errors,
        // not merely whole-image truncations caught by RamImage::parse.
        for len in 1..metadata.len() {
            failures.push(ram_image(&metadata[..len]));
        }
        let mut bad = metadata.clone();
        bad[16..18].copy_from_slice(&u16::MAX.to_le_bytes());
        failures.push(ram_image(&bad));
        let mut bad = metadata.clone();
        bad[22] = 1; // inner padding cannot consume bytes from the table
        failures.push(ram_image(&bad));
        let mut bad = metadata.clone();
        bad.extend_from_slice(&metadata[20..]); // duplicate feature
        bad[16] = 40;
        failures.push(ram_image(&bad));
        let mut bad = metadata.clone();
        bad.push(0); // malformed record after an otherwise valid feature
        bad[16] = 21;
        failures.push(ram_image(&bad));
        let mut bad = metadata;
        bad.push(0); // an oversized but in-bounds feature payload
        bad[16] = 21;
        bad[20] = 17;
        failures.push(ram_image(&bad));
        let mut bad = good.clone();
        let table = bad.len() - 36 - 40;
        bad[table + 20..table + 24].copy_from_slice(&u32::MAX.to_le_bytes());
        failures.push(bad);
        let mut bad = good;
        let trailer = bad.len() - 36;
        bad[trailer + 2] = u8::MAX;
        failures.push(bad);
        for (i, data) in failures.iter().enumerate() {
            out = FEATURES;
            assert_eq!(
                unsafe { mtk_wlan_ram_features(data.as_ptr(), data.len(), &mut out) },
                EINVAL,
                "case {i}"
            );
            assert_eq!(out, FwFeatures::default(), "case {i}");
        }
    }

    #[test]
    fn diagnostic_errors_do_not_change_ram_download_ffi() {
        let data = ram_image(&[0x12]);
        let mut features = FEATURES;
        assert_eq!(
            unsafe { mtk_wlan_ram_features(data.as_ptr(), data.len(), &mut features) },
            EINVAL
        );
        assert_eq!(features, FwFeatures::default());
        let mut info = MtkWlanRamInfo {
            chip_id: 0,
            eco_code: 0,
            regions: 0,
            fw_ver: [0; 11],
            build_date: [0; 16],
            override_addr: 0,
            start_option: 0,
        };
        assert_eq!(
            unsafe { mtk_wlan_ram_parse(data.as_ptr(), data.len(), &mut info) },
            0
        );
        assert_eq!(info.regions, 1);
        assert_eq!(info.override_addr, 0x9000_0000);
        assert_eq!(info.start_option, mtk_wlan::connac2::fw::FW_START_OVERRIDE);
        let mut region = MtkWlanFwRegion {
            addr: 0,
            len: 0,
            mode: 0,
            offset: 0,
            feature: 0,
            download: false,
        };
        assert_eq!(
            unsafe { mtk_wlan_ram_region(data.as_ptr(), data.len(), 0, &mut region) },
            0
        );
        assert_eq!(
            (region.addr, region.offset, region.len),
            (0x9000_0000, 0, 3)
        );
        assert!(region.download);
    }

    #[test]
    fn phy_caps_ffi_preserves_raw_bytes_and_clears_errors() {
        let bytes = [
            0x80, 0x81, 0x82, 0x83, 0xff, 0x85, 0x86, 0x87, 0x88, 0x89, 0xfe, 0x8b,
        ];
        let expected = PhyCaps::parse(&bytes).unwrap();
        let mut extended = bytes.to_vec();
        extended.extend_from_slice(&[0xaa; 20]);
        let mut out = PhyCaps::default();
        for data in [bytes.as_slice(), extended.as_slice()] {
            assert_eq!(
                unsafe { mtk_wlan_phy_caps_parse(data.as_ptr(), data.len(), &mut out) },
                0
            );
            assert_eq!(out, expected);
        }
        for len in 0..12 {
            out = expected;
            assert_eq!(
                unsafe { mtk_wlan_phy_caps_parse(bytes.as_ptr(), len, &mut out) },
                EINVAL
            );
            assert_eq!(out, PhyCaps::default());
        }
        for len in [0, 12, usize::MAX] {
            out = expected;
            assert_eq!(
                unsafe { mtk_wlan_phy_caps_parse(core::ptr::null(), len, &mut out) },
                EINVAL
            );
            assert_eq!(out, PhyCaps::default());
        }
        assert_eq!(
            unsafe { mtk_wlan_phy_caps_parse(bytes.as_ptr(), bytes.len(), core::ptr::null_mut()) },
            EINVAL
        );
    }
}

#[cfg(test)]
mod join_tests {
    use super::*;
    #[test]
    fn observation_ffi_clears_output_on_invalid_input_and_matches_c_layout() {
        use mtk_wlan::join::EapolObservation;
        assert_eq!(core::mem::size_of::<EapolObservation>(), 16);
        assert_eq!(core::mem::offset_of!(EapolObservation, info), 12);
        let mut out = EapolObservation {
            receiver: [9; 6],
            ..Default::default()
        };
        assert_eq!(
            unsafe { mtk_wlan_eapol_observe(core::ptr::null(), 10, &mut out) },
            EINVAL
        );
        assert_eq!(out, EapolObservation::default());
        assert_eq!(
            unsafe { mtk_wlan_eapol_observe(core::ptr::null(), 0, core::ptr::null_mut()) },
            EINVAL
        );
    }
}
