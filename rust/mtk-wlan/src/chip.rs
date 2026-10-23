//! Supported PCIe Wi-Fi chips.

pub const PCI_VENDOR_MEDIATEK: u16 = 0x14c3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Family {
    /// MT7921 / MT7922: Connac2 MCU interface (mt7921 in Linux, mwx(4) in OpenBSD).
    Connac2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Chip {
    Mt7921,
    Mt7922,
}

/// Firmware images, by the names they are embedded under.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Firmware {
    pub rom_patch: &'static str,
    pub ram: &'static str,
}

impl Chip {
    pub const fn family(self) -> Family {
        match self {
            Chip::Mt7921 | Chip::Mt7922 => Family::Connac2,
        }
    }

    pub const fn firmware(self) -> Firmware {
        match self {
            Chip::Mt7921 => Firmware {
                rom_patch: "mwx-mt7961_patch_mcu_1_2_hdr",
                ram: "mwx-mt7961_ram_code_1",
            },
            Chip::Mt7922 => Firmware {
                rom_patch: "mwx-mt7922_patch_mcu_1_1_hdr",
                ram: "mwx-mt7922_ram_code_1",
            },
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ChipDesc {
    pub pci_device: u16,
    pub chip: Chip,
    pub name: &'static str,
}

pub static CHIPS: &[ChipDesc] = &[
    ChipDesc {
        pci_device: 0x7961,
        chip: Chip::Mt7921,
        name: "MT7921",
    },
    ChipDesc {
        pci_device: 0x0608,
        chip: Chip::Mt7921,
        name: "MT7921K (RZ608)",
    },
    ChipDesc {
        pci_device: 0x7922,
        chip: Chip::Mt7922,
        name: "MT7922",
    },
    // RZ616 is an MT7922; Linux mt7921e loads the MT7922 firmware for it.
    ChipDesc {
        pci_device: 0x0616,
        chip: Chip::Mt7922,
        name: "MT7922 (RZ616)",
    },
];

pub fn lookup_pci(vendor: u16, device: u16) -> Option<&'static ChipDesc> {
    if vendor != PCI_VENDOR_MEDIATEK {
        return None;
    }
    CHIPS.iter().find(|c| c.pci_device == device)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rz616_uses_mt7922_firmware() {
        let desc = lookup_pci(PCI_VENDOR_MEDIATEK, 0x0616).unwrap();
        assert_eq!(desc.chip.firmware().ram, "mwx-mt7922_ram_code_1");
        assert_eq!(desc.chip.family(), Family::Connac2);
    }

    #[test]
    fn rejects_other_vendors_and_devices() {
        assert!(lookup_pci(0x8086, 0x7961).is_none());
        assert!(lookup_pci(PCI_VENDOR_MEDIATEK, 0x7925).is_none());
    }
}
