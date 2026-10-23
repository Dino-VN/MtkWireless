//! WFDMA ring bookkeeping that depends on values read from the hardware.
//!
//! A device that fell off the bus reads as 0xffffffff, and a confused DMA
//! engine can report any index or length; none of these may index a ring
//! slot or size a buffer unchecked.

pub const DMA_CTL_SD_LEN_MASK: u32 = 0x0000_3fff;
pub const DMA_CTL_SD_LEN0_SHIFT: u32 = 16;
pub const DMA_CTL_LAST_SEC0: u32 = 1 << 30;
pub const DMA_CTL_DMA_DONE: u32 = 1 << 31;

/// A ring index read from `MT_DMA_DMA_IDX`, if it names a slot.
pub fn hw_index(raw: u32, count: u32) -> Option<u32> {
    (raw < count).then_some(raw)
}

pub fn ring_next(index: u32, count: u32) -> u32 {
    if count == 0 { 0 } else { (index + 1) % count }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RxSegment {
    pub len: u32,
    /// Last segment of the frame (`LAST_SEC0`).
    pub last: bool,
}

/// Length and end-of-frame flag of a completed RX descriptor, if the length
/// fits the buffer that was posted (`capacity` bytes).
pub fn rx_segment(ctrl: u32, capacity: u32) -> Option<RxSegment> {
    let len = (ctrl >> DMA_CTL_SD_LEN0_SHIFT) & DMA_CTL_SD_LEN_MASK;
    (len <= capacity).then_some(RxSegment {
        len,
        last: ctrl & DMA_CTL_LAST_SEC0 != 0,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn indices() {
        assert_eq!(hw_index(5, 256), Some(5));
        assert_eq!(hw_index(256, 256), None);
        assert_eq!(hw_index(0xffff_ffff, 256), None, "device gone");
        assert_eq!(hw_index(0, 0), None);
        assert_eq!(ring_next(255, 256), 0);
        assert_eq!(ring_next(3, 0), 0);
    }

    #[test]
    fn segments() {
        let ctrl = |len: u32, last: bool| {
            (len << 16) | if last { DMA_CTL_LAST_SEC0 } else { 0 } | DMA_CTL_DMA_DONE
        };
        assert_eq!(
            rx_segment(ctrl(1500, true), 2048),
            Some(RxSegment {
                len: 1500,
                last: true
            })
        );
        assert_eq!(
            rx_segment(ctrl(2048, false), 2048),
            Some(RxSegment {
                len: 2048,
                last: false
            })
        );
        assert_eq!(rx_segment(ctrl(0x3fff, true), 2048), None);
        assert_eq!(rx_segment(0xffff_ffff, 2048), None);
    }
}
