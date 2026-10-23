//! Runtime checks run once at kext start (phase 0 gate).

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::alloc::Layout;
use core::fmt::Write;
use core::hint::black_box;

use mtk_common::{Error, Reader};
use mtk_kernel::klog;
use mtk_kernel::log::LineBuf;

const HEAP: u32 = 1 << 0;
const ALIGNED_HEAP: u32 = 1 << 1;
const FORMAT: u32 = 1 << 2;
const WIDE_MATH: u32 = 1 << 3;
const BOUNDS: u32 = 1 << 4;
const COMPONENT: u32 = 1 << 5;

type Check = (u32, &'static str, fn() -> bool);

pub fn run() -> u32 {
    let checks: [Check; 6] = [
        (HEAP, "heap", heap),
        (ALIGNED_HEAP, "aligned heap", aligned_heap),
        (FORMAT, "format", format),
        (WIDE_MATH, "u128 math", wide_math),
        (BOUNDS, "bounds", bounds),
        (COMPONENT, "component", component),
    ];
    let mut failed = 0;
    for (bit, name, check) in checks {
        if !check() {
            klog!("self-test {name}: FAILED");
            failed |= bit;
        }
    }
    klog!("self-test done, failed mask 0x{failed:x}");
    failed
}

fn heap() -> bool {
    let mut v: Vec<u32> = Vec::new();
    for i in 0..1000 {
        v.push(i);
    }
    let big = alloc::vec![0xa5u8; 64 * 1024].into_boxed_slice();
    let boxed = Box::new(black_box(7u64));
    v.iter().map(|&x| x as u64).sum::<u64>() == 499_500
        && big.iter().all(|&b| b == 0xa5)
        && *boxed == 7
}

fn aligned_heap() -> bool {
    let Ok(layout) = Layout::from_size_align(256, 64) else {
        return false;
    };
    // SAFETY: non-zero size; freed with the same layout.
    unsafe {
        let p = alloc::alloc::alloc(layout);
        if p.is_null() {
            return false;
        }
        let ok = (p as usize) % 64 == 0;
        p.write_bytes(0, 256);
        alloc::alloc::dealloc(p, layout);
        ok
    }
}

fn format() -> bool {
    let mut line = LineBuf::new();
    let _ = write!(
        line,
        "{:04x}-{}-{:?}",
        black_box(0x7961u32),
        black_box(-3i8),
        "ok"
    );
    line.as_bytes() == b"7961--3-\"ok\""
}

fn wide_math() -> bool {
    let a = black_box(0x1234_5678_9abc_def0_u128);
    let b = black_box(0x1_0000_0001_u128);
    let q = a / b;
    let r = a % b;
    q * b + r == a && (black_box(u64::MAX) as u128) * 3 == 0x2_ffff_ffff_ffff_fffd
}

fn bounds() -> bool {
    let data = black_box([1u8, 2, 3]);
    let mut r = Reader::new(&data);
    r.le16() == Ok(0x0201) && r.le16() == Err(Error::Truncated { offset: 2, need: 2 })
}

#[cfg(feature = "wifi")]
fn component() -> bool {
    use mtk_wlan::chip::{PCI_VENDOR_MEDIATEK, lookup_pci};
    let desc = lookup_pci(PCI_VENDOR_MEDIATEK, black_box(0x0616));
    desc.is_some_and(|d| d.chip.firmware().ram == "mwx-mt7922_ram_code_1")
}

#[cfg(feature = "bt")]
fn component() -> bool {
    use mtk_bt::chip::{Chip, FirmwareName};
    Chip::from_dev_id(black_box(0x7961))
        .is_some_and(|c| FirmwareName::new(c, 1, 0).as_str() == "BT_RAM_CODE_MT7961_1_2_hdr.bin")
}

#[cfg(not(any(feature = "wifi", feature = "bt")))]
fn component() -> bool {
    true
}

#[cfg(test)]
mod tests {
    #[test]
    fn selftest_passes_on_host() {
        assert_eq!(super::run(), 0);
    }
}
