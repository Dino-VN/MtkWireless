//! Rewrites of traffic between bluetoothd and the controller (applied by the
//! kext's IOUSBHostFamily routes). Each function only looks at the bytes it
//! is given and reports what it changed.

/// `bmRequestType` of HCI commands on EP0: class request, host to device.
pub fn is_hci_command_request(bm_request_type: u8) -> bool {
    bm_request_type & 0x80 == 0
}

pub const OP_READ_CLOCK: u16 = 0x1407;

/// HCI_Read_Clock(handle, Which_Clock = 0) → Which_Clock = 1.
///
/// bluetoothd reads the BT clock with Which_Clock = 0. For the local clock
/// MediaTek returns Connection_Handle 0 instead of the handle that was passed,
/// so bluetoothd cannot match the result to the connection, and AAC-ELD to
/// AirPods (timestamped with that clock) never starts. The piconet clock of
/// the connection returns the handle; as central it is our native clock.
///
/// `cmd` is the whole 6-byte command. Returns true when it was rewritten.
pub fn fix_read_clock(cmd: &mut [u8]) -> bool {
    match cmd {
        [lo, hi, 3, _, _, which @ 0] if u16::from_le_bytes([*lo, *hi]) == OP_READ_CLOCK => {
            *which = 1;
            true
        }
        _ => false,
    }
}

/// Largest bulk OUT transfer considered for the ACL rewrite (one AAC-ELD
/// frame is ~966 bytes).
pub const ACL_MAX_TRANSFER: u32 = 1100;

/// Whether a transfer on this endpoint may carry ACL data to rewrite: bulk
/// OUT, at least an ACL header, not larger than [`ACL_MAX_TRANSFER`].
pub fn is_acl_out_candidate(ep_attributes: u8, ep_address: u8, length: u32) -> bool {
    ep_attributes & 0x03 == 0x02
        && ep_address & 0x80 == 0
        && (4..=ACL_MAX_TRANSFER).contains(&length)
}

/// ACL packet boundary flag 0b10 (first automatically flushable) → 0b00
/// (first non-flushable).
///
/// bluetoothd sends A2DP media as flushable fragments. MediaTek flushes them
/// when a large L2CAP frame does not make it in one baseband packet, so
/// headphones get broken frames and pause. Linux always sends ACL data to
/// these controllers as non-flushable.
///
/// `header` is the first four bytes of a transfer of `length` bytes. Returns
/// the new value of byte 1 when the transfer is such an ACL packet.
pub fn acl_non_flushable(header: [u8; 4], length: u32) -> Option<u8> {
    let handle = u16::from_le_bytes([header[0], header[1]]);
    let data_len = u16::from_le_bytes([header[2], header[3]]) as u32;
    let is_acl = (handle & 0x0fff) < 0x0f00 && data_len + 4 == length;
    (is_acl && (handle >> 12) & 3 == 2).then_some(header[1] & !0x30)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_clock_only_local_clock_requests() {
        let mut cmd = [0x07, 0x14, 3, 0x0b, 0x00, 0];
        assert!(fix_read_clock(&mut cmd));
        assert_eq!(cmd, [0x07, 0x14, 3, 0x0b, 0x00, 1]);
        assert!(!fix_read_clock(&mut cmd), "already piconet clock");
        let mut other = [0x07, 0x14, 4, 0x0b, 0x00, 0];
        assert!(!fix_read_clock(&mut other));
        let mut other = [0x08, 0x14, 3, 0x0b, 0x00, 0];
        assert!(!fix_read_clock(&mut other));
        assert!(!fix_read_clock(&mut [0x07, 0x14, 3, 0x0b, 0x00]));
    }

    #[test]
    fn acl_flag_rewrite() {
        // handle 0x00b, PB 0b10, 966 bytes of data
        let hdr = [0x0b, 0x20, 0xc6, 0x03];
        assert_eq!(acl_non_flushable(hdr, 970), Some(0x00));
        assert_eq!(acl_non_flushable(hdr, 969), None, "length mismatch");
        assert_eq!(
            acl_non_flushable([0x0b, 0x10, 0xc6, 0x03], 970),
            None,
            "continuation"
        );
        assert_eq!(
            acl_non_flushable([0x0b, 0x00, 0xc6, 0x03], 970),
            None,
            "already 0b00"
        );
        assert_eq!(
            acl_non_flushable([0x00, 0x2f, 0x00, 0x00], 4),
            None,
            "handle out of range"
        );
        // broadcast flags are kept
        assert_eq!(acl_non_flushable([0x0b, 0x60, 0x00, 0x00], 4), Some(0x40));
    }

    #[test]
    fn endpoint_filter() {
        assert!(is_acl_out_candidate(0x02, 0x02, 970));
        assert!(!is_acl_out_candidate(0x02, 0x82, 970), "IN");
        assert!(!is_acl_out_candidate(0x01, 0x03, 970), "isochronous");
        assert!(!is_acl_out_candidate(0x02, 0x02, 3));
        assert!(!is_acl_out_candidate(0x02, 0x02, 1101));
    }
}
