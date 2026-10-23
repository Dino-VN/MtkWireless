//! Chip ids read from the controller and the firmware they need.

use core::fmt::{self, Write};

/// `dev_id` register values (MTK_BT_READ_DEV_ID) of supported chips.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Chip {
    /// MT7921 family; the Bluetooth core reports 0x7961.
    Mt7961,
    Mt7922,
}

impl Chip {
    pub const fn from_dev_id(dev_id: u32) -> Option<Self> {
        match dev_id & 0xffff {
            0x7961 => Some(Chip::Mt7961),
            0x7922 => Some(Chip::Mt7922),
            _ => None,
        }
    }

    pub const fn dev_id(self) -> u16 {
        match self {
            Chip::Mt7961 => 0x7961,
            Chip::Mt7922 => 0x7922,
        }
    }
}

/// Firmware file name, e.g. `BT_RAM_CODE_MT7961_1_2_hdr.bin`.
#[derive(Clone, PartialEq, Eq)]
pub struct FirmwareName {
    buf: [u8; 48],
    len: usize,
}

impl FirmwareName {
    /// Same rule as Linux `btmtk_fw_get_filename`: the "1a" image is the
    /// MT7961 flavor bit, the last digit is `(fw_version & 0xff) + 1`.
    pub fn new(chip: Chip, fw_version: u32, fw_flavor: u32) -> Self {
        let mut name = Self {
            buf: [0; 48],
            len: 0,
        };
        let variant = if chip == Chip::Mt7961 && fw_flavor != 0 {
            "1a"
        } else {
            "1"
        };
        // Cannot fail: the longest name is 31 bytes.
        let _ = write!(
            name,
            "BT_RAM_CODE_MT{:04x}_{variant}_{:x}_hdr.bin",
            chip.dev_id(),
            (fw_version & 0xff) + 1
        );
        name
    }

    pub fn as_str(&self) -> &str {
        // Only ASCII is ever written.
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("")
    }
}

impl Write for FirmwareName {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let end = self
            .len
            .checked_add(s.len())
            .filter(|&e| e <= self.buf.len())
            .ok_or(fmt::Error)?;
        self.buf[self.len..end].copy_from_slice(s.as_bytes());
        self.len = end;
        Ok(())
    }
}

impl fmt::Debug for FirmwareName {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_match_shipped_firmware() {
        let mt7961 = Chip::from_dev_id(0x7961).unwrap();
        assert_eq!(
            FirmwareName::new(mt7961, 0x01, 0).as_str(),
            "BT_RAM_CODE_MT7961_1_2_hdr.bin"
        );
        assert_eq!(
            FirmwareName::new(mt7961, 0x01, 1).as_str(),
            "BT_RAM_CODE_MT7961_1a_2_hdr.bin"
        );
        let mt7922 = Chip::from_dev_id(0x7922).unwrap();
        assert_eq!(
            FirmwareName::new(mt7922, 0x00, 1).as_str(),
            "BT_RAM_CODE_MT7922_1_1_hdr.bin"
        );
    }

    #[test]
    fn unknown_dev_id() {
        assert_eq!(Chip::from_dev_id(0x7663), None);
    }
}
