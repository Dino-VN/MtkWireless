//! Controller bring-up: chip id, ROM patch download, endpoint reset and
//! Bluetooth enable. Mirrors Linux `btmtk_usb_setup` for MT7922/MT7961.
//!
//! All USB access goes through [`Transport`], so the sequence runs unchanged
//! against the kext's USB device or a test double.

use core::fmt;

use crate::chip::{Chip, FirmwareName};
use crate::fw::{PatchImage, SECTION_HEADER_SIZE};
use crate::wmt::{self, Op, Status};

pub const MTK_BT_READ_DEV_ID: u32 = 0x7001_0200;
const REG_INITIAL_DEV_ID: u32 = 0x8000_0008;
const REG_FW_VERSION: u32 = 0x8002_1004;
const REG_FW_FLAVOR: u32 = 0x7001_0020;
pub const MTK_BT_MISC: u32 = 0x7000_2510;
pub const MTK_BT_SUBSYS_RST: u32 = 0x7000_2610;
pub const MTK_UDMA_INT_STA_BT: u32 = 0x7400_0024;
pub const MTK_UDMA_INT_STA_BT1: u32 = 0x7400_0308;
pub const MTK_BT_WDT_STATUS: u32 = 0x7400_03A0;
pub const MTK_EP_RST_OPT: u32 = 0x7401_1890;
pub const MTK_EP_RST_IN_OUT_OPT: u32 = 0x0001_0001;
const MTK_BT_RST_DONE: u32 = 0x0000_0100;
/// Older MT7663 parts answer the initial id register directly.
const DEV_ID_MT7663: u32 = 0x7663;

const PATCH_CHUNK: usize = 250;
const SEMAPHORE_RETRIES: u32 = 20;

/// An IOReturn from the USB stack.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct IoError(pub i32);

pub trait Transport {
    type Firmware: AsRef<[u8]>;

    /// Vendor IN request 0x63: `wValue = reg >> 16`, `wIndex = reg & 0xffff`.
    fn reg_read(&mut self, reg: u32) -> Result<u32, IoError>;
    /// "UHW" vendor IN request 0x01 (bmRequestType 0xDE).
    fn uhw_read(&mut self, reg: u32) -> Result<u32, IoError>;
    /// "UHW" vendor OUT request 0x02 (bmRequestType 0x5E).
    fn uhw_write(&mut self, reg: u32, value: u32) -> Result<(), IoError>;
    /// Send an HCI command on EP0 and read the WMT event that answers it.
    /// Returns the event length written to `event`.
    fn wmt(&mut self, command: &[u8], event: &mut [u8]) -> Result<usize, IoError>;
    fn sleep_ms(&mut self, ms: u32);
    /// Decompressed firmware image by file name.
    fn load_firmware(&mut self, name: &str) -> Option<Self::Firmware>;
    fn log(&mut self, args: fmt::Arguments<'_>);
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SetupInfo {
    pub chip: Chip,
    pub dev_id: u32,
    pub fw_version: u32,
    pub fw_flavor: u32,
    pub firmware: FirmwareName,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SetupError {
    Io { step: &'static str, code: i32 },
    Unsupported { dev_id: u32 },
    FirmwareMissing,
    FirmwareInvalid(mtk_common::Error),
    Wmt(mtk_common::Error),
    UnexpectedStatus { section: u32, status: Status },
    SemaphoreTimeout { section: u32 },
}

impl SetupError {
    /// Stable negative code for the C ABI.
    pub const fn code(self) -> i32 {
        match self {
            SetupError::Io { .. } => -1,
            SetupError::Unsupported { .. } => -2,
            SetupError::FirmwareMissing => -3,
            SetupError::FirmwareInvalid(_) => -4,
            SetupError::Wmt(_) => -5,
            SetupError::UnexpectedStatus { .. } => -6,
            SetupError::SemaphoreTimeout { .. } => -7,
        }
    }
}

macro_rules! tlog {
    ($t:expr, $($arg:tt)*) => { $t.log(format_args!($($arg)*)) };
}

fn io<T>(step: &'static str, r: Result<T, IoError>) -> Result<T, SetupError> {
    r.map_err(|IoError(code)| SetupError::Io { step, code })
}

/// Full bring-up. On a patch download failure the Bluetooth subsystem is reset
/// so the next attempt (or the next boot) starts clean.
pub fn setup<T: Transport>(t: &mut T) -> Result<SetupInfo, SetupError> {
    let mut dev_id = io("read initial dev id", t.reg_read(REG_INITIAL_DEV_ID))?;
    let mut fw_version = 0;
    let mut fw_flavor = 0;
    if dev_id != DEV_ID_MT7663 {
        dev_id = io("read dev id", t.reg_read(MTK_BT_READ_DEV_ID))?;
        fw_version = io("read fw version", t.reg_read(REG_FW_VERSION))?;
        fw_flavor = (io("read fw flavor", t.reg_read(REG_FW_FLAVOR))? & 0x80) >> 7;
    }
    tlog!(
        t,
        "dev_id=0x{dev_id:04x} fw_version=0x{fw_version:08x} fw_flavor={fw_flavor}"
    );

    let Some(chip) = Chip::from_dev_id(dev_id) else {
        tlog!(t, "unsupported MediaTek hardware variant 0x{dev_id:08x}");
        return Err(SetupError::Unsupported { dev_id });
    };
    let firmware = FirmwareName::new(chip, fw_version, fw_flavor);
    tlog!(t, "firmware file {}", firmware.as_str());

    if let Err(e) = download_patch(t, firmware.as_str()) {
        tlog!(t, "patch download failed: {e:?}");
        if let Err(r) = subsystem_reset(t, chip) {
            tlog!(t, "subsystem reset failed: {r:?}");
        }
        return Err(e);
    }
    io(
        "endpoint reset option",
        t.uhw_write(MTK_EP_RST_OPT, MTK_EP_RST_IN_OUT_OPT),
    )?;
    enable_bluetooth(t, true)?;
    tlog!(t, "setup complete");
    Ok(SetupInfo {
        chip,
        dev_id,
        fw_version,
        fw_flavor,
        firmware,
    })
}

/// WMT FUNC_CTRL: turn the Bluetooth function on or off.
pub fn enable_bluetooth<T: Transport>(t: &mut T, enable: bool) -> Result<(), SetupError> {
    wmt_sync(t, Op::FuncCtrl, 0, &[enable as u8]).map(|_| ())
}

fn wmt_sync<T: Transport>(
    t: &mut T,
    op: Op,
    flag: u8,
    payload: &[u8],
) -> Result<Status, SetupError> {
    let mut command = [0u8; wmt::COMMAND_BUF_SIZE];
    let len = wmt::build_command(&mut command, op, flag, payload).map_err(SetupError::Wmt)?;
    let mut event = [0u8; wmt::EVENT_BUF_SIZE];
    let got = io("WMT command", t.wmt(&command[..len], &mut event))?;
    let event = event
        .get(..got)
        .ok_or(SetupError::Wmt(mtk_common::Error::Invalid(
            "event length beyond buffer",
        )))?;
    wmt::parse_event(event, op).map_err(SetupError::Wmt)
}

fn download_patch<T: Transport>(t: &mut T, name: &str) -> Result<(), SetupError> {
    let fw = t.load_firmware(name).ok_or(SetupError::FirmwareMissing)?;
    let image = PatchImage::parse(fw.as_ref()).map_err(SetupError::FirmwareInvalid)?;
    let built = &image.build_time;
    let built = core::str::from_utf8(&built[..built.iter().position(|&b| b == 0).unwrap_or(16)])
        .unwrap_or("?");
    tlog!(
        t,
        "patch hw/sw 0x{:04x}{:04x} built {built} sections {}",
        image.hw_ver,
        image.sw_ver,
        image.section_count
    );

    for section in image.sections() {
        let section = section.map_err(SetupError::FirmwareInvalid)?;
        if section.data.is_empty() {
            continue;
        }
        if !send_section_header(t, section.index, section.header)? {
            tlog!(t, "section {} already patched", section.index);
            continue;
        }
        let mut chunks = section.data.chunks(PATCH_CHUNK).peekable();
        let mut first = true;
        while let Some(chunk) = chunks.next() {
            let flag = if first {
                1
            } else if chunks.peek().is_none() {
                3
            } else {
                2
            };
            first = false;
            let status = wmt_sync(t, Op::PatchDownload, flag, chunk)?;
            if status == Status::PatchProgress {
                return Err(SetupError::UnexpectedStatus {
                    section: section.index,
                    status,
                });
            }
        }
    }
    t.sleep_ms(120);
    Ok(())
}

/// Announces a section. Returns false when the controller reports that it is
/// already patched, true when the data must follow.
fn send_section_header<T: Transport>(
    t: &mut T,
    index: u32,
    header: &[u8],
) -> Result<bool, SetupError> {
    let mut payload = [0u8; 1 + SECTION_HEADER_SIZE];
    payload[1..].copy_from_slice(header);
    for _ in 0..SEMAPHORE_RETRIES {
        let status = wmt_sync(t, Op::PatchDownload, 0, &payload)?;
        tlog!(t, "section {index} patch status {status:?}");
        match status {
            Status::PatchUndone => return Ok(true),
            Status::PatchDone => return Ok(false),
            Status::PatchProgress => t.sleep_ms(100),
            status => {
                return Err(SetupError::UnexpectedStatus {
                    section: index,
                    status,
                });
            }
        }
    }
    Err(SetupError::SemaphoreTimeout { section: index })
}

/// Bluetooth subsystem reset (Linux `btmtk_usb_subsys_reset`).
pub fn subsystem_reset<T: Transport>(t: &mut T, chip: Chip) -> Result<(), SetupError> {
    match chip {
        Chip::Mt7922 => {
            let v = io("reset read", t.uhw_read(MTK_BT_SUBSYS_RST))?;
            io(
                "reset write",
                t.uhw_write(MTK_BT_SUBSYS_RST, v | 0x0000_2020),
            )?;
            io("reset ep opt", t.uhw_write(MTK_EP_RST_OPT, 0x0001_0001))?;
            let v = io("reset read", t.uhw_read(MTK_BT_SUBSYS_RST))?;
            io("reset write", t.uhw_write(MTK_BT_SUBSYS_RST, v | 1))?;
            t.sleep_ms(100);
        }
        Chip::Mt7961 => {
            io(
                "reset ep opt",
                t.uhw_write(MTK_EP_RST_OPT, MTK_EP_RST_IN_OUT_OPT),
            )?;
            io("read wdt", t.uhw_read(MTK_BT_WDT_STATUS))?;
            io("reset assert", t.uhw_write(MTK_BT_SUBSYS_RST, 1))?;
            io("clear udma", t.uhw_write(MTK_UDMA_INT_STA_BT, 0xff))?;
            io("read udma", t.uhw_read(MTK_UDMA_INT_STA_BT))?;
            io("clear udma1", t.uhw_write(MTK_UDMA_INT_STA_BT1, 0xff))?;
            io("read udma1", t.uhw_read(MTK_UDMA_INT_STA_BT1))?;
            t.sleep_ms(20);
            io("reset release", t.uhw_write(MTK_BT_SUBSYS_RST, 0))?;
            io("reset read", t.uhw_read(MTK_BT_SUBSYS_RST))?;
        }
    }
    for _ in 0..50 {
        if io("reset done", t.uhw_read(MTK_BT_MISC))? & MTK_BT_RST_DONE != 0 {
            break;
        }
        t.sleep_ms(20);
    }
    if chip == Chip::Mt7922 {
        io("clear udma", t.uhw_write(MTK_UDMA_INT_STA_BT, 0xff))?;
    }
    io("read dev id after reset", t.reg_read(MTK_BT_READ_DEV_ID))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fw::tests::image;
    use std::collections::VecDeque;
    use std::string::String;
    use std::vec::Vec;

    #[derive(Debug, Clone, PartialEq, Eq)]
    enum Call {
        RegRead(u32),
        UhwRead(u32),
        UhwWrite(u32, u32),
        Wmt(Vec<u8>),
        Sleep(u32),
        Load(String),
    }

    /// Controller double: answers register reads from a table and WMT
    /// commands from a script (default: "undone" for headers, "ok" otherwise).
    struct Fake {
        dev_id: u32,
        fw_version: u32,
        flavor_reg: u32,
        firmware: Option<Vec<u8>>,
        header_replies: VecDeque<u8>,
        calls: Vec<Call>,
        logs: Vec<String>,
    }

    impl Fake {
        fn new(dev_id: u32, firmware: Option<Vec<u8>>) -> Self {
            Self {
                dev_id,
                fw_version: 1,
                flavor_reg: 0,
                firmware,
                header_replies: VecDeque::new(),
                calls: Vec::new(),
                logs: Vec::new(),
            }
        }

        fn wmt_commands(&self) -> Vec<&Vec<u8>> {
            self.calls
                .iter()
                .filter_map(|c| match c {
                    Call::Wmt(c) => Some(c),
                    _ => None,
                })
                .collect()
        }
    }

    impl Transport for Fake {
        type Firmware = Vec<u8>;

        fn reg_read(&mut self, reg: u32) -> Result<u32, IoError> {
            self.calls.push(Call::RegRead(reg));
            Ok(match reg {
                REG_INITIAL_DEV_ID => 0,
                MTK_BT_READ_DEV_ID => self.dev_id,
                REG_FW_VERSION => self.fw_version,
                REG_FW_FLAVOR => self.flavor_reg,
                _ => return Err(IoError(-536870206)),
            })
        }

        fn uhw_read(&mut self, reg: u32) -> Result<u32, IoError> {
            self.calls.push(Call::UhwRead(reg));
            Ok(if reg == MTK_BT_MISC {
                MTK_BT_RST_DONE
            } else {
                0
            })
        }

        fn uhw_write(&mut self, reg: u32, value: u32) -> Result<(), IoError> {
            self.calls.push(Call::UhwWrite(reg, value));
            Ok(())
        }

        fn wmt(&mut self, command: &[u8], event: &mut [u8]) -> Result<usize, IoError> {
            self.calls.push(Call::Wmt(command.to_vec()));
            let op = command[4];
            let header = op == Op::PatchDownload as u8 && command[7] == 0;
            let flag = if header {
                self.header_replies.pop_front().unwrap_or(0)
            } else {
                0
            };
            let reply = [0xe4, 5, 2, op, 1, 0, flag];
            event[..7].copy_from_slice(&reply);
            Ok(7)
        }

        fn sleep_ms(&mut self, ms: u32) {
            self.calls.push(Call::Sleep(ms));
        }

        fn load_firmware(&mut self, name: &str) -> Option<Vec<u8>> {
            self.calls.push(Call::Load(name.into()));
            self.firmware.clone()
        }

        fn log(&mut self, args: fmt::Arguments<'_>) {
            self.logs.push(std::format!("{args}"));
        }
    }

    #[test]
    fn mt7961_downloads_every_section_in_250_byte_chunks() {
        let fw = image(&[&[7; 600], &[], &[8; 250]]);
        let mut t = Fake::new(0x7961, Some(fw));
        let info = setup(&mut t).unwrap();
        assert_eq!(info.firmware.as_str(), "BT_RAM_CODE_MT7961_1_2_hdr.bin");
        assert!(t.calls.contains(&Call::Load(info.firmware.as_str().into())));

        let cmds = t.wmt_commands();
        // section 0: header + 3 chunks (1, 2, 3); section 2: header + 1 chunk (1);
        // then FUNC_CTRL on.
        let flags: Vec<(u8, u8, usize)> = cmds.iter().map(|c| (c[4], c[7], c.len() - 8)).collect();
        assert_eq!(
            flags,
            [
                (1, 0, 53),
                (1, 1, 250),
                (1, 2, 250),
                (1, 3, 100),
                (1, 0, 53),
                (1, 1, 250),
                (6, 0, 1),
            ]
        );
        // Section header payload: a zero byte, then the map's sent part.
        assert_eq!(cmds[0][8], 0);
        assert_eq!(cmds[0][9], 0xA0);
        assert_eq!(cmds[4][9], 0xA2);
        let ep = t
            .calls
            .iter()
            .position(|c| *c == Call::UhwWrite(MTK_EP_RST_OPT, MTK_EP_RST_IN_OUT_OPT))
            .unwrap();
        let on = t
            .calls
            .iter()
            .rposition(|c| matches!(c, Call::Wmt(_)))
            .unwrap();
        assert!(ep < on, "endpoint reset option precedes FUNC_CTRL");
    }

    #[test]
    fn flavor_bit_selects_1a_image() {
        let mut t = Fake::new(0x7961, Some(image(&[&[1]])));
        t.flavor_reg = 0x80;
        assert_eq!(
            setup(&mut t).unwrap().firmware.as_str(),
            "BT_RAM_CODE_MT7961_1a_2_hdr.bin"
        );
    }

    #[test]
    fn already_patched_section_is_skipped_and_progress_retries() {
        let mut t = Fake::new(0x7922, Some(image(&[&[1; 10], &[2; 10]])));
        t.fw_version = 0;
        // Section 0: progress twice then done (skip). Section 1: undone.
        t.header_replies = [1, 1, 2, 0].into();
        let info = setup(&mut t).unwrap();
        assert_eq!(info.firmware.as_str(), "BT_RAM_CODE_MT7922_1_1_hdr.bin");
        let sleeps = t.calls.iter().filter(|c| **c == Call::Sleep(100)).count();
        assert_eq!(sleeps, 2);
        let data_chunks = t
            .wmt_commands()
            .iter()
            .filter(|c| c[4] == 1 && c[7] != 0)
            .count();
        assert_eq!(data_chunks, 1);
    }

    #[test]
    fn semaphore_timeout_resets_subsystem() {
        let mut t = Fake::new(0x7961, Some(image(&[&[1]])));
        t.header_replies = [1; 20].into();
        assert_eq!(
            setup(&mut t),
            Err(SetupError::SemaphoreTimeout { section: 0 })
        );
        assert!(t.calls.contains(&Call::UhwWrite(MTK_BT_SUBSYS_RST, 1)));
        assert!(
            !t.calls
                .iter()
                .any(|c| matches!(c, Call::Wmt(w) if w[4] == 6))
        );
    }

    #[test]
    fn missing_or_bad_firmware_and_unknown_chip() {
        assert_eq!(
            setup(&mut Fake::new(0x7961, None)),
            Err(SetupError::FirmwareMissing)
        );
        assert!(matches!(
            setup(&mut Fake::new(0x7961, Some(vec![0; 10]))),
            Err(SetupError::FirmwareInvalid(_))
        ));
        assert_eq!(
            setup(&mut Fake::new(0x7925, None)),
            Err(SetupError::Unsupported { dev_id: 0x7925 })
        );
    }

    #[test]
    fn mt7922_reset_sequence() {
        let mut t = Fake::new(0x7922, None);
        subsystem_reset(&mut t, Chip::Mt7922).unwrap();
        assert_eq!(
            t.calls[..6],
            [
                Call::UhwRead(MTK_BT_SUBSYS_RST),
                Call::UhwWrite(MTK_BT_SUBSYS_RST, 0x2020),
                Call::UhwWrite(MTK_EP_RST_OPT, 0x0001_0001),
                Call::UhwRead(MTK_BT_SUBSYS_RST),
                Call::UhwWrite(MTK_BT_SUBSYS_RST, 1),
                Call::Sleep(100),
            ]
        );
        assert_eq!(t.calls.last(), Some(&Call::RegRead(MTK_BT_READ_DEV_ID)));
    }
}
