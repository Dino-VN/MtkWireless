//! WMT: MediaTek's vendor protocol over HCI command 0xFC6F / event 0xE4.

use mtk_common::{Error, Result, Writer};

pub const HCI_OPCODE: u16 = 0xfc6f;
pub const HCI_EVENT: u8 = 0xe4;
/// Command buffer size used by Linux btmtk; the largest command is a
/// 250-byte patch chunk plus headers.
pub const COMMAND_BUF_SIZE: usize = 272;
pub const EVENT_BUF_SIZE: usize = 64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Op {
    PatchDownload = 0x01,
    Test = 0x02,
    Wakeup = 0x03,
    Hif = 0x04,
    FuncCtrl = 0x06,
    Reset = 0x07,
    Register = 0x08,
    Semaphore = 0x17,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Invalid,
    PatchUndone,
    PatchProgress,
    PatchDone,
    OnUndone,
    OnDone,
    OnProgress,
}

/// HCI command: opcode, length, then WMT header {dir=1, op, dlen, flag} and
/// payload. `dlen` counts the flag byte. Returns the command length.
pub fn build_command(buf: &mut [u8], op: Op, flag: u8, payload: &[u8]) -> Result<usize> {
    let wmt_len = 5 + payload.len();
    if wmt_len > 255 {
        return Err(Error::Invalid("WMT payload too long"));
    }
    let mut w = Writer::new(buf);
    w.le16(HCI_OPCODE)?;
    w.u8(wmt_len as u8)?;
    w.u8(1)?;
    w.u8(op as u8)?;
    w.le16(payload.len() as u16 + 1)?;
    w.u8(flag)?;
    w.put(payload)?;
    Ok(w.len())
}

/// Decodes the WMT event answering `op`.
pub fn parse_event(event: &[u8], op: Op) -> Result<Status> {
    // HCI event header (2) + WMT header (5).
    if event.len() < 7 {
        return Err(Error::Truncated { offset: 0, need: 7 });
    }
    if event[0] != HCI_EVENT {
        return Err(Error::Invalid("not a WMT event"));
    }
    if event[3] != op as u8 {
        return Err(Error::Invalid("WMT event for another op"));
    }
    let flag = event[6];
    Ok(match op {
        Op::Semaphore if flag == 2 => Status::PatchUndone,
        Op::Semaphore => Status::PatchDone,
        Op::FuncCtrl => match event.get(7..9) {
            Some(&[hi, lo]) => match u16::from_be_bytes([hi, lo]) {
                0x404 => Status::OnDone,
                0x420 => Status::OnProgress,
                _ => Status::OnUndone,
            },
            _ => Status::OnUndone,
        },
        Op::PatchDownload => match flag {
            2 => Status::PatchDone,
            1 => Status::PatchProgress,
            _ => Status::PatchUndone,
        },
        _ => Status::Invalid,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn func_ctrl_on_matches_linux_bytes() {
        let mut buf = [0; COMMAND_BUF_SIZE];
        let n = build_command(&mut buf, Op::FuncCtrl, 0, &[1]).unwrap();
        assert_eq!(
            &buf[..n],
            &[0x6f, 0xfc, 0x06, 0x01, 0x06, 0x02, 0x00, 0x00, 0x01]
        );
    }

    #[test]
    fn largest_patch_chunk_fits() {
        let mut buf = [0; COMMAND_BUF_SIZE];
        assert_eq!(
            build_command(&mut buf, Op::PatchDownload, 2, &[0; 250]),
            Ok(258)
        );
        assert!(build_command(&mut buf, Op::PatchDownload, 2, &[0; 251]).is_err());
        let mut small = [0; 8];
        assert!(build_command(&mut small, Op::FuncCtrl, 0, &[1]).is_err());
    }

    #[test]
    fn decodes_statuses() {
        let ev = |op: u8, flag: u8| [0xe4, 5, 2, op, 1, 0, flag];
        assert_eq!(
            parse_event(&ev(1, 1), Op::PatchDownload),
            Ok(Status::PatchProgress)
        );
        assert_eq!(
            parse_event(&ev(1, 2), Op::PatchDownload),
            Ok(Status::PatchDone)
        );
        assert_eq!(
            parse_event(&ev(1, 0), Op::PatchDownload),
            Ok(Status::PatchUndone)
        );
        assert_eq!(
            parse_event(&ev(0x17, 2), Op::Semaphore),
            Ok(Status::PatchUndone)
        );
        assert_eq!(parse_event(&ev(6, 0), Op::FuncCtrl), Ok(Status::OnUndone));
        let on = [0xe4, 7, 2, 6, 1, 0, 0, 0x04, 0x04];
        assert_eq!(parse_event(&on, Op::FuncCtrl), Ok(Status::OnDone));
        assert!(parse_event(&ev(6, 0), Op::PatchDownload).is_err());
        assert!(parse_event(&[0x0e, 4, 1, 0x6f, 0xfc, 0, 0], Op::FuncCtrl).is_err());
        assert!(parse_event(&[0xe4, 5, 2], Op::FuncCtrl).is_err());
    }
}
