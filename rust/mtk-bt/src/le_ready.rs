//! Observe real USB HCI traffic and repair the legacy LE event mask.
//! bluetoothd can wait for DLE/PHY events even when it selected mask 0x1f.
//! No command response, negotiated value, or connection event is fabricated.
use mtk_common::{Error, Reader, Result};

pub const OPCODES: [u16; 4] = [0x2001, 0x2022, 0x2032, 0x2030];
pub const SUBEVENTS: [u8; 4] = [0x07, 0x0c, 0x14, 0x04];
pub const READY_MASK: u64 = (1 << (0x07 - 1)) | (1 << (0x0c - 1));

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Command {
    pub count: u32,
    pub usb_errors: u32,
    pub last_usb_status: u32,
    pub status_events: u32,
    pub complete_events: u32,
    pub last_status: u32,
    pub last_kind: u32,
    pub last_handle: u32,
    pub arg0: u32,
    pub arg1: u32,
    pub serial: u32,
    pub response_serial: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Meta {
    pub count: u32,
    pub last_handle: u32,
    /// 0xffffffff for subevents without a status field.
    pub last_status: u32,
    pub value0: u32,
    pub value1: u32,
    pub value2: u32,
    pub value3: u32,
    pub serial: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Diagnostics {
    pub controller_id: u64,
    pub mask_before: u64,
    pub mask_after: u64,
    pub mask_rewrites: u32,
    pub mask_write_errors: u32,
    pub malformed: u32,
    pub serial: u32,
    pub commands: [Command; 4],
    pub meta: [Meta; 4],
}

impl Diagnostics {
    pub const fn new() -> Self {
        Self {
            controller_id: 0,
            mask_before: 0,
            mask_after: 0,
            mask_rewrites: 0,
            mask_write_errors: 0,
            malformed: 0,
            serial: 0,
            commands: [Command {
                count: 0,
                usb_errors: 0,
                last_usb_status: 0,
                status_events: 0,
                complete_events: 0,
                last_status: 0,
                last_kind: 0,
                last_handle: 0,
                arg0: 0,
                arg1: 0,
                serial: 0,
                response_serial: 0,
            }; 4],
            meta: [Meta {
                count: 0,
                last_handle: 0,
                last_status: 0,
                value0: 0,
                value1: 0,
                value2: 0,
                value3: 0,
                serial: 0,
            }; 4],
        }
    }

    /// Returns index + 1, with bit 8 set when mask bytes 3..11 need writing.
    pub fn command(&mut self, id: u64, bytes: &mut [u8], fix: bool) -> Result<u32> {
        let mut r = Reader::new(bytes);
        let op = r.le16()?;
        let length = r.u8()? as usize;
        if length != r.remaining() {
            return Err(Error::Invalid("HCI command length"));
        }
        let Some(index) = OPCODES.iter().position(|&v| v == op) else {
            return Ok(0);
        };
        let expected = [8, 6, 7, 2][index];
        if length != expected {
            return Err(Error::Invalid("LE command length"));
        }
        let mut c = Command::default();
        let mut rewrite = false;
        if index == 0 {
            let mask = u64::from_le_bytes(r.array()?);
            self.mask_before = mask;
            self.mask_after = if fix && mask != 0 {
                mask | READY_MASK
            } else {
                mask
            };
            rewrite = mask != self.mask_after;
        } else {
            c.last_handle = r.le16()? as u32;
            match index {
                1 => {
                    c.arg0 = r.le16()? as u32;
                    c.arg1 = r.le16()? as u32;
                }
                2 => {
                    let all = r.u8()? as u32;
                    let tx = r.u8()? as u32;
                    let rx = r.u8()? as u32;
                    c.arg0 = all | (tx << 8) | (rx << 16);
                    c.arg1 = r.le16()? as u32;
                }
                _ => (),
            }
        }
        self.controller_id = id;
        self.serial = self.serial.wrapping_add(1);
        let old = &mut self.commands[index];
        old.count = old.count.wrapping_add(1);
        old.last_handle = c.last_handle;
        old.arg0 = c.arg0;
        old.arg1 = c.arg1;
        old.serial = self.serial;
        if rewrite {
            bytes[3..11].copy_from_slice(&self.mask_after.to_le_bytes());
        }
        Ok((index as u32 + 1) | if rewrite { 0x100 } else { 0 })
    }

    pub fn mask_written(&mut self, ok: bool) {
        if ok {
            self.mask_rewrites = self.mask_rewrites.wrapping_add(1);
        } else {
            self.mask_write_errors = self.mask_write_errors.wrapping_add(1);
            self.mask_after = self.mask_before;
        }
    }

    pub fn usb_status(&mut self, command: u32, status: u32) {
        if let Some(c) = command
            .checked_sub(1)
            .and_then(|i| self.commands.get_mut(i as usize))
        {
            c.last_usb_status = status;
            if status != 0 {
                c.usb_errors = c.usb_errors.wrapping_add(1);
            }
        }
    }

    /// Validate the whole batch before observing any event. USB has no H4 byte.
    pub fn events(&mut self, id: u64, bytes: &[u8]) -> Result<bool> {
        let mut framing = Reader::new(bytes);
        while !framing.is_empty() {
            framing.u8()?;
            let len = framing.u8()? as usize;
            framing.skip(len)?;
        }
        let mut r = Reader::new(bytes);
        let mut changed = false;
        while !r.is_empty() {
            let kind = r.u8()?;
            let len = r.u8()? as usize;
            let mut p = Reader::new(r.take(len)?);
            let mut response = None;
            if kind == 0x0f && len == 4 {
                let status = p.u8()? as u32;
                p.u8()?;
                response = Some((p.le16()?, status));
            } else if kind == 0x0e && len >= 4 {
                p.u8()?;
                let op = p.le16()?;
                response = Some((op, p.u8()? as u32));
            }
            if let Some((op, status)) = response {
                if let Some(i) = OPCODES.iter().position(|&v| v == op) {
                    self.serial = self.serial.wrapping_add(1);
                    let c = &mut self.commands[i];
                    if kind == 0x0f {
                        c.status_events = c.status_events.wrapping_add(1);
                    } else {
                        c.complete_events = c.complete_events.wrapping_add(1);
                    }
                    c.last_status = status;
                    c.last_kind = kind as u32;
                    c.response_serial = self.serial;
                    // LE Read PHY CC returns the real handle and TX/RX PHY.
                    if i == 3 && status == 0 && p.remaining() == 4 {
                        c.last_handle = p.le16()? as u32;
                        c.arg0 = p.u8()? as u32;
                        c.arg1 = p.u8()? as u32;
                    }
                    changed = true;
                }
            } else if kind == 0x3e && len >= 1 {
                let sub = p.u8()?;
                if let Some(i) = SUBEVENTS.iter().position(|&v| v == sub) {
                    if len != [11, 6, 4, 12][i] {
                        self.malformed = self.malformed.wrapping_add(1);
                        changed = true;
                        continue;
                    }
                    let mut event = Meta::default();
                    event.last_status = if i == 1 || i == 3 {
                        p.u8()? as u32
                    } else {
                        u32::MAX
                    };
                    event.last_handle = p.le16()? as u32;
                    match i {
                        0 => {
                            event.value0 = p.le16()? as u32;
                            event.value1 = p.le16()? as u32;
                            event.value2 = p.le16()? as u32;
                            event.value3 = p.le16()? as u32;
                        }
                        1 => {
                            event.value0 = p.u8()? as u32;
                            event.value1 = p.u8()? as u32;
                        }
                        2 => event.value0 = p.u8()? as u32,
                        _ => {
                            event.value0 = p.le32()?;
                            event.value1 = p.le32()?;
                        }
                    }
                    self.serial = self.serial.wrapping_add(1);
                    event.serial = self.serial;
                    event.count = self.meta[i].count.wrapping_add(1);
                    self.meta[i] = event;
                    changed = true;
                }
            }
        }
        if changed {
            self.controller_id = id;
        }
        Ok(changed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mask_preserves_every_bit_and_zero_disable() {
        for fix in [false, true] {
            for mask in [0, 0x1f, READY_MASK, 0xbf3ff3f86fa7f, u64::MAX, 1 << 63] {
                let mut d = Diagnostics::new();
                let mut b = [0; 11];
                b[..3].copy_from_slice(&[1, 0x20, 8]);
                b[3..].copy_from_slice(&mask.to_le_bytes());
                let token = d.command(5, &mut b, fix).unwrap();
                let expected = if fix && mask != 0 {
                    mask | READY_MASK
                } else {
                    mask
                };
                assert_eq!(&b[3..], &expected.to_le_bytes());
                assert_eq!(token & 0x100 != 0, expected != mask);
                if token & 0x100 != 0 {
                    d.mask_written(true);
                }
                assert_eq!(d.mask_rewrites, (expected != mask) as u32);
                assert_eq!(d.mask_before, mask);
                assert_eq!(d.mask_after, expected);
            }
        }
    }

    #[test]
    fn records_commands_status_complete_and_wire_order() {
        let mut d = Diagnostics::new();
        assert_eq!(
            d.command(7, &mut [0x22, 0x20, 6, 1, 2, 0xfb, 0, 0x48, 8], true)
                .unwrap(),
            2
        );
        assert_eq!(d.commands[1].last_handle, 0x201);
        assert_eq!((d.commands[1].arg0, d.commands[1].arg1), (251, 2120));
        assert_eq!(
            d.command(7, &mut [0x32, 0x20, 7, 1, 2, 0, 2, 2, 0, 0], true)
                .unwrap(),
            3
        );
        d.usb_status(3, 0xe00002ed);
        assert_eq!(d.commands[2].usb_errors, 1);
        assert!(
            d.events(
                7,
                &[0x0f, 4, 0x0c, 1, 0x32, 0x20, 0x0e, 4, 1, 0x22, 0x20, 0]
            )
            .unwrap()
        );
        assert_eq!(d.commands[2].last_status, 0x0c);
        assert_eq!(d.commands[2].status_events, 1);
        assert_eq!(d.commands[1].complete_events, 1);
        assert!(d.commands[2].response_serial < d.commands[1].response_serial);
        assert!(
            d.events(7, &[0x0e, 8, 1, 0x30, 0x20, 0, 1, 2, 2, 1])
                .unwrap()
        );
        assert_eq!(
            (
                d.commands[3].last_handle,
                d.commands[3].arg0,
                d.commands[3].arg1
            ),
            (0x201, 2, 1)
        );
    }

    #[test]
    fn real_subevents_and_early_autonomous_negotiation() {
        let mut d = Diagnostics::new();
        let events = [
            0x3e, 11, 7, 1, 2, 0xfb, 0, 0x48, 8, 0xfb, 0, 0x48, 8, 0x3e, 6, 12, 0, 1, 2, 2, 2,
            0x3e, 4, 20, 1, 2, 1, 0x3e, 12, 4, 0, 1, 2, 0xfd, 0x79, 1, 0xf7, 0xf9, 0, 0, 0,
        ];
        assert!(d.events(7, &events).unwrap());
        assert_eq!(d.meta.map(|v| v.count), [1; 4]);
        assert_eq!(d.meta[0].value0, 251);
        assert_eq!(d.meta[0].last_status, u32::MAX);
        assert_eq!(d.meta[1].last_status, 0);
        assert_eq!(d.meta[2].value0, 1);
        assert_eq!(d.meta[3].value0, 0xf70179fd);
        assert!(d.meta[1].serial < d.meta[3].serial);
        assert_eq!(d.commands.map(|v| v.count), [0; 4]);
    }

    #[test]
    fn malformed_batches_never_read_or_modify_outside_declared_payload() {
        let mut d = Diagnostics::new();
        let mut command = [1, 0x20, 8, 0x1f, 0, 0, 0, 0, 0, 0, 0];
        for end in 0..command.len() {
            let old = command;
            assert!(d.command(7, &mut command[..end], true).is_err());
            assert_eq!(command, old);
        }
        let before = d;
        assert!(d.events(7, &[0x0e, 4, 1, 1, 0x20, 0, 0x3e, 6, 12]).is_err());
        assert_eq!(d, before);
        assert!(d.events(7, &[0x3e, 3, 12, 0, 1]).unwrap());
        assert_eq!(d.malformed, 1);
        assert_eq!(d.meta[1].count, 0);
        // Opcode-looking bytes in an advertisement are not a response.
        assert!(
            !d.events(7, &[0x3e, 7, 2, 0x0f, 4, 0, 1, 0x32, 0x20])
                .unwrap()
        );
        assert_eq!(d.commands[2].status_events, 0);
        d.mask_before = 0x1f;
        d.mask_after = 0x85f;
        d.mask_written(false);
        assert_eq!(d.mask_after, 0x1f);
        assert_eq!(d.mask_write_errors, 1);
    }
}
