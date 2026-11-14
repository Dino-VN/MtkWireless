//! Relocatable bluetoothd RPA patches. Resolve a complete *owned file image*
//! before exec; the page-fault path only checks/applies the immutable plan.
//!
//! Image addresses are decoded from Mach-O mappings and native call/RIP
//! operands. Frame/register/object constraints remain exact signatures: a
//! different ABI is unsupported, not something to guess from a build number.
use mtk_common::{Error, Reader, Result, Writer};

use crate::page_patch::{Match, Site};

pub const MAX_IMAGE: usize = 64 * 1024 * 1024;
pub const MAX_EDITS: usize = 7;
pub const MAX_EDIT_BYTES: usize = 296;
pub const PAGE: u64 = 4096;

#[repr(C)]
pub struct Edit {
    pub file_offset: u64,
    pub len: u32,
    pub reserved: u32,
    pub original: [u8; MAX_EDIT_BYTES],
    pub replacement: [u8; MAX_EDIT_BYTES],
}

#[repr(C)]
pub struct Plan {
    pub slice_offset: u64,
    pub slice_size: u64,
    pub text_address: u64,
    pub text_file_offset: u64,
    pub text_size: u64,
    pub helper_address: u64,
    pub uuid: [u8; 16],
    /// Publish last, only after every signature, reference and edit is checked.
    pub count: u32,
    pub reserved: u32,
    pub edits: [Edit; MAX_EDITS],
}

// These offsets describe instructions *inside our own position-independent
// payload*, not locations in bluetoothd. All nine external operands are zero.
pub const HELPER: &[u8] = &[
    0x31, 0xc0, 0x89, 0x47, 0x33, 0x89, 0x47, 0x36, 0xe9, 0, 0, 0, 0, 0x48, 0x8d, 0x15, 0, 0, 0, 0,
    0x48, 0x39, 0x55, 0x08, 0x75, 0x2d, 0x80, 0x3b, 0, 0x75, 0x28, 0x48, 0x8b, 0x15, 0, 0, 0, 0,
    0x48, 0x85, 0xd2, 0x74, 0x1c, 0x83, 0xba, 0x34, 0x03, 0, 0, 0, 0x75, 0x13, 0x41, 0x8b, 0x04,
    0x24, 0x41, 0x8b, 0x4c, 0x24, 0x03, 0x89, 0x43, 0x33, 0x89, 0x4b, 0x36, 0xc6, 0x43, 0x33, 0x81,
    0x8b, 0x45, 0xb8, 0x8b, 0x4d, 0xbb, 0x41, 0x89, 0x4c, 0x24, 0x03, 0x41, 0x89, 0x04, 0x24, 0xc3,
    0x48, 0x8d, 0x0d, 0, 0, 0, 0, 0x48, 0x39, 0x4d, 0x08, 0x75, 0x5c, 0x48, 0x8b, 0x4d, 0, 0x48,
    0x8b, 0x89, 0x80, 0xfb, 0xff, 0xff, 0x48, 0x8b, 0x49, 0x08, 0x80, 0xb9, 0xd6, 0, 0, 0, 0x81,
    0x75, 0x44, 0x48, 0x8b, 0x91, 0xd0, 0, 0, 0, 0x48, 0xc7, 0x81, 0xd0, 0, 0, 0, 0, 0, 0, 0, 0x48,
    0x85, 0xc0, 0x74, 0x36, 0x48, 0x8b, 0x0d, 0, 0, 0, 0, 0x48, 0x85, 0xc9, 0x74, 0x21, 0x83, 0xb9,
    0x34, 0x03, 0, 0, 0, 0x75, 0x18, 0x48, 0x83, 0x78, 0x18, 0, 0x75, 0x08, 0x48, 0x8b, 0x48, 0x10,
    0x48, 0x89, 0x48, 0x18, 0x48, 0x0f, 0xba, 0xf2, 0x37, 0x48, 0x89, 0x50, 0x10, 0x48, 0x85, 0xc0,
    0x74, 0x04, 0x44, 0x88, 0x70, 0x61, 0xc3, 0x31, 0xd2, 0x48, 0x8b, 0x05, 0, 0, 0, 0, 0x48, 0x85,
    0xc0, 0x74, 0x0b, 0x83, 0xb8, 0x34, 0x03, 0, 0, 0, 0x75, 0x02, 0xff, 0xc2, 0xe9, 0, 0, 0, 0,
    0x48, 0x8b, 0x05, 0, 0, 0, 0, 0x48, 0x85, 0xc0, 0x74, 0x0c, 0x83, 0xb8, 0x34, 0x03, 0, 0, 0,
    0x75, 0x03, 0x31, 0xc0, 0xc3, 0xe9, 0, 0, 0, 0,
];
const _: () = assert!(HELPER.len() == 261);

const fn rel_mask<const N: usize>(fields: &[usize]) -> [u8; N] {
    let mut mask = [0xff; N];
    let mut i = 0;
    while i < fields.len() {
        let mut j = 0;
        while j < 4 {
            mask[fields[i] + j] = 0;
            j += 1;
        }
        i += 1;
    }
    mask
}

const LEGACY: &[u8] = &[
    0xc6, 0x45, 0xac, 0, 0xc7, 0x45, 0xaf, 0, 1, 0, 0, 0x48, 0x8d, 0x7d, 0x80, 0xe8, 0, 0, 0, 0,
    0x48, 0x8b, 0x7d, 0x90, 0xe8, 0, 0, 0, 0,
];
const LEGACY_MASK: [u8; 29] = rel_mask(&[16, 25]);
// Include the native legacy/random-report guard, not just its resolver call.
const SCAN: &[u8] = &[
    0x3c, 1, 0x75, 0x3c, 0x8a, 0x43, 2, 0x24, 0xc0, 0x3c, 0x40, 0x75, 0x33, 0x48, 0x8d, 0x55, 0xd7,
    0xc6, 2, 0, 0x31, 0xc0, 0x48, 0x8d, 0x75, 0xb8, 0x89, 0x46, 3, 0x89, 6, 0x4c, 0x89, 0xe7, 0xe8,
    0, 0, 0, 0, 0x85, 0xc0, 0x75, 0x15, 0x80, 0x7d, 0xd7, 0, 0x74, 0x0f, 0x8b, 0x45, 0xb8, 0x8b,
    0x4d, 0xbb, 0x41, 0x89, 0x4c, 0x24, 3, 0x41, 0x89, 4, 0x24,
];
const SCAN_MASK: [u8; 64] = rel_mask(&[35]);
const DECODE: &[u8] = &[
    0x49, 0x8b, 0xb6, 0x68, 0x17, 0, 0, 0x0f, 0xb6, 6, 0x48, 0x83, 0xf8, 1, 0x77, 0x48, 0x0f, 0xb6,
    0x7e, 1, 0x48, 0xc1, 0xe7, 0x28, 0x44, 0x0f, 0xb6, 0x46, 2, 0x49, 0xc1, 0xe0, 0x20, 0x44, 0x0f,
    0xb6, 0x4e, 3, 0x41, 0xc1, 0xe1, 0x18, 0x44, 0x0f, 0xb6, 0x56, 4, 0x41, 0xc1, 0xe2, 0x10, 0x44,
    0x0f, 0xb6, 0x5e, 5, 0x41, 0xc1, 0xe3, 8, 0x0f, 0xb6, 0x76, 6, 0x48, 0xc1, 0xe0, 0x30, 0x48,
    0x09, 0xf8, 0x4c, 0x09, 0xc0, 0x4c, 0x09, 0xc8, 0x4c, 0x09, 0xd0, 0x4c, 0x09, 0xd8, 0x48, 0x09,
    0xf0, 0xeb, 2, 0x31, 0xc0, 0x48, 0x8b, 0x75, 0x88, 0x48, 0x89, 0x86, 0xd0, 0, 0, 0, 8, 0xca,
    0x0f, 0x84, 0x88, 0, 0, 0, 0x48, 0x83, 0x3d, 0, 0, 0, 0, 0xff,
];
const DECODE_MASK: [u8; 117] = rel_mask(&[112]);
const DECODE_REPLACEMENT: &[u8] = &[
    0x49, 0x8b, 0xb6, 0x68, 0x17, 0, 0, 0x0f, 0xb6, 6, 0x3c, 1, 0x76, 0x1d, 0x3c, 0x81, 0x75, 0x25,
    0x41, 0x80, 0xbe, 0x56, 0x17, 0, 0, 0, 0x75, 0x1b, 0x41, 0x80, 0x3e, 0, 0x75, 0x15, 0x8a, 0x46,
    1, 0x24, 0xc0, 0x3c, 0x40, 0x75, 0x0c, 0x48, 0x8b, 6, 0x48, 0x0f, 0xc8, 0x48, 0xc1, 0xe8, 8,
    0xeb, 0x23, 0x31, 0xc0, 0xeb, 0x1f,
];
const DEVICE: &[u8] = &[
    0xe8, 0, 0, 0, 0, 0x48, 0x85, 0xc0, 0x74, 4, 0x44, 0x88, 0x70, 0x61, 0x48, 0x8d, 0x7d, 0xd0,
    0xe8, 0, 0, 0, 0, 0x48, 0x89, 0xdf, 0xff, 0x15, 0, 0, 0, 0,
];
const DEVICE_MASK: [u8; 32] = rel_mask(&[1, 19, 28]);
const CONNECT: &[u8] = &[
    0x4c, 0x89, 0xff, 0x48, 0x89, 0xde, 0xe8, 0, 0, 0, 0, 0x84, 0xc0, 0x74, 0x15, 0x4c, 0x89, 0xf7,
    0x4c, 0x89, 0xfe, 0x48, 0x89, 0xda, 0xb9, 2, 0, 0, 0, 0xe8, 0, 0, 0, 0, 0xeb, 0x25, 0x4c, 0x89,
    0xff, 0x48, 0x89, 0xde, 0x31, 0xd2, 0xe8, 0, 0, 0, 0,
];
const CONNECT_MASK: [u8; 49] = rel_mask(&[7, 30, 45]);
const UPDATE: &[u8] = &[
    0x48, 0x8b, 0x3d, 0, 0, 0, 0, 0x48, 0x8b, 0xb5, 0x78, 0xfd, 0xff, 0xff, 0xe8, 0, 0, 0, 0, 0x84,
    0xc0, 0x0f, 0x84, 0x86, 0, 0, 0, 0x48, 0x83, 0x3d, 0, 0, 0, 0, 0xff, 0x0f, 0x85, 0x42, 7, 0, 0,
    0x48, 0x8b, 0x35, 0, 0, 0, 0, 0x4c, 0x89, 0xef, 0x48, 0x8b, 0x95, 0x78, 0xfd, 0xff, 0xff, 0xb9,
    2, 0, 0, 0, 0xe8, 0, 0, 0, 0, 0x48, 0x8b, 0xb5, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x8d, 0xbd, 0x58,
    0xfd, 0xff, 0xff, 0xe8, 0, 0, 0, 0, 0x48, 0x8b, 0x45, 0xa0, 0x48, 0x8b, 0x75, 0xa8, 0x48, 0x89,
    0x85, 0x58, 0xfd, 0xff, 0xff, 0x48, 0x89, 0xb5, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x8b, 0x45, 0xb0,
    0x48, 0x89, 0x85, 0x68, 0xfd, 0xff, 0xff, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xe1, 0, 0, 0, 0x48,
    0x8d, 0x85, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x89, 0x46, 0x10, 0x48, 0x8d, 0x45, 0xa8, 0x48, 0x89,
    0x45, 0xa0, 0x0f, 0x57, 0xc0, 0x0f, 0x11, 0, 0x31, 0xf6, 0xe9, 0xcf, 0, 0, 0, 0x48, 0x83, 0x3d,
    0, 0, 0, 0, 0xff, 0x0f, 0x85, 0xc6, 6, 0, 0, 0x48, 0x8b, 0x3d, 0, 0, 0, 0, 0x48, 0x8b, 0xb5,
    0x78, 0xfd, 0xff, 0xff, 0x31, 0xd2, 0xe8, 0, 0, 0, 0,
];
const UPDATE_MASK: [u8; 196] = rel_mask(&[3, 15, 30, 44, 64, 83, 164, 178, 192]);
const PARENT: &[u8] = &[
    0x48, 0x8b, 0x85, 0x80, 0xfb, 0xff, 0xff, 0x48, 0x8b, 0x40, 8, 0x31, 0xd2, 0x83, 0xb8, 0x38, 1,
    0, 0, 0, 0x0f, 0x95, 0xc2, 0x48, 0x89, 0xdf, 0x48, 0x89, 0xb5, 0x70, 0xfb, 0xff, 0xff, 0xe8, 0,
    0, 0, 0,
];
const PARENT_MASK: [u8; 38] = rel_mask(&[34]);
const CHIP_CALL: &[u8] = &[
    0xe8, 0, 0, 0, 0, 0x83, 0xb8, 0x34, 3, 0, 0, 3, 0x77, 0x2a, 0x31, 0xff, 0xe8, 0, 0, 0, 0,
];
const CHIP_CALL_MASK: [u8; 21] = rel_mask(&[1, 17]);
const ADDRESS_RETRY: &[u8] = &[
    0x44, 0x8b, 0x04, 0x3a, 0x44, 0x33, 0x84, 0x08, 0x0e, 0x0d, 0, 0, 0x8b, 0x54, 0x3a, 3, 0x33,
    0x94, 0x08, 0x11, 0x0d, 0, 0, 0x44, 0x09, 0xc2, 0x0f, 0x84, 0x80, 0, 0, 0, 0x80, 0x3d, 0, 0, 0,
    0, 0, 0x74, 0x77, 0x48, 0x8d, 0x3c, 0x08, 0x48, 0x81, 0xc7, 0x0e, 0x0d, 0, 0, 0x48, 0x8d, 0x15,
    0x7b, 0, 0, 0, 0xe8, 0, 0, 0, 0,
];
const ADDRESS_RETRY_MASK: [u8; 64] = rel_mask(&[34, 60]);
const ADDRESS_SETTER: &[u8] = &[
    0x55, 0x48, 0x89, 0xe5, 0x40, 0x88, 0x3d, 0, 0, 0, 0, 0x5d, 0xc3,
];
const ADDRESS_SETTER_MASK: [u8; 13] = rel_mask(&[7]);
const DEVICE_ENTRY: &[u8] = &[
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x18, 0x41, 0x89, 0xd6,
    0x48, 0x89, 0xf3, 0x49, 0x89, 0xff, 0x48, 0x89, 0xf7, 0xff, 0x15, 0, 0, 0, 0, 0x0f, 0x57, 0xc0,
    0x48, 0x8d, 0x7d, 0xd0, 0x0f, 0x29, 7, 0x49, 0x8d, 0x77, 0x60, 0xe8, 0, 0, 0, 0, 0x4c, 0x89,
    0xff, 0x48, 0x89, 0xde,
];
const DEVICE_ENTRY_MASK: [u8; 56] = rel_mask(&[27, 46]);
const CHIP_GETTER: &[u8] = &[
    0x48, 0x83, 0x3d, 0, 0, 0, 0, 0xff, 0x75, 8, 0x48, 0x8b, 5, 0, 0, 0, 0, 0xc3,
];
const CHIP_GETTER_MASK: [u8; 18] = rel_mask(&[3, 13]);
const ADDRESS_ENTRY: &[u8] = &[
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x30, 0x41,
    0x89, 0xd6, 0x48, 0x89, 0xf3, 0x49, 0x89, 0xff,
];
const ADDRESS_SELECTION: &[u8] = &[
    0x45, 0x84, 0xf6, 0x75, 9, 0x4d, 0x8b, 0x77, 0x18, 0x4d, 0x85, 0xf6, 0x75, 0x2f, 0x4d, 0x8b,
    0x77, 0x10, 0xeb, 0x29,
];
const PAIRED_ENTRY: &[u8] = &[
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10, 0x48, 0x89, 0xf3, 0x49, 0x89,
    0xfe,
];

fn bytes(data: &[u8], at: usize, len: usize) -> Result<&[u8]> {
    let mut r = Reader::new(data);
    r.seek(at)?;
    r.take(len)
}
fn u32le(data: &[u8], at: usize) -> Result<u32> {
    let mut r = Reader::new(data);
    r.seek(at)?;
    r.le32()
}
fn u64le(data: &[u8], at: usize) -> Result<u64> {
    let mut r = Reader::new(data);
    r.seek(at)?;
    Ok(u64::from_le_bytes(r.array()?))
}
fn usize64(value: u64) -> Result<usize> {
    usize::try_from(value).map_err(|_| Error::Invalid("RPA file offset"))
}
fn add(a: u64, b: u64) -> Result<u64> {
    a.checked_add(b)
        .ok_or(Error::Invalid("RPA address overflow"))
}
fn rel_target(data: &[u8], field: usize, field_address: u64) -> Result<u64> {
    let displacement = i32::from_le_bytes(
        bytes(data, field, 4)?
            .try_into()
            .map_err(|_| Error::Invalid("rel32"))?,
    );
    let end = add(field_address, 4)?;
    if displacement < 0 {
        end.checked_sub(-(displacement as i64) as u64)
            .ok_or(Error::Invalid("RPA rel32 underflow"))
    } else {
        add(end, displacement as u64)
    }
}
fn put_rel(data: &mut [u8], field: usize, field_address: u64, target: u64) -> Result<()> {
    let end = add(field_address, 4)?;
    let displacement = i32::try_from(target as i128 - end as i128)
        .map_err(|_| Error::Invalid("RPA rel32 range"))?;
    let tail = data
        .get_mut(field..)
        .ok_or(Error::Invalid("RPA relocation field"))?;
    Writer::new(tail).le32(displacement as u32)
}
fn signature(window: &[u8], find: &[u8], mask: &[u8]) -> bool {
    window.len() == find.len()
        && window
            .iter()
            .zip(find)
            .zip(mask)
            .all(|((&a, &b), &m)| (a ^ b) & m == 0)
}

#[derive(Clone, Copy)]
struct Segment {
    address: u64,
    file: u64,
    size: u64,
    protection: u32,
}
struct Image<'a> {
    data: &'a [u8],
    base: u64,
    commands: &'a [u8],
    count: u32,
    text: Segment,
    code_file: usize,
    code_size: usize,
    section_end: u64,
    commands_end: u64,
    uuid: [u8; 16],
}
impl<'a> Image<'a> {
    fn parse(full: &'a [u8]) -> Result<Self> {
        if full.len() > MAX_IMAGE {
            return Err(Error::Invalid("RPA image too large"));
        }
        let magic = bytes(full, 0, 4)?;
        let mut base = 0;
        let mut data = full;
        if magic == [0xca, 0xfe, 0xba, 0xbe] || magic == [0xca, 0xfe, 0xba, 0xbf] {
            let wide = magic[3] == 0xbf;
            let mut r = Reader::new(full);
            r.skip(4)?;
            let n = r.be32()?;
            if n == 0 || n > 32 {
                return Err(Error::Invalid("RPA fat architecture count"));
            }
            let mut selected = false;
            for _ in 0..n {
                let cpu = r.be32()?;
                r.skip(4)?;
                let (off, size) = if wide {
                    let o = u64::from_be_bytes(r.array()?);
                    let s = u64::from_be_bytes(r.array()?);
                    r.skip(8)?;
                    (o, s)
                } else {
                    let o = r.be32()?;
                    let s = r.be32()?;
                    r.skip(4)?;
                    (o as u64, s as u64)
                };
                let candidate = bytes(full, usize64(off)?, usize64(size)?)?;
                if cpu == 0x0100_0007 {
                    if selected || off % PAGE != 0 {
                        return Err(Error::Invalid("RPA ambiguous fat slice"));
                    }
                    data = candidate;
                    base = off;
                    selected = true;
                }
            }
            if !selected {
                return Err(Error::Invalid("RPA no x86_64 slice"));
            }
            if base < r.position() as u64 {
                return Err(Error::Invalid("RPA fat header overlap"));
            }
        }
        if bytes(data, 0, 4)? != [0xcf, 0xfa, 0xed, 0xfe]
            || u32le(data, 4)? != 0x0100_0007
            || u32le(data, 12)? != 2
        {
            return Err(Error::Invalid("RPA needs x86_64 Mach-O executable"));
        }
        let count = u32le(data, 16)?;
        let size = u32le(data, 20)? as usize;
        if count == 0 || count > 256 || size > 65536 {
            return Err(Error::Invalid("RPA load commands"));
        }
        let commands = bytes(data, 32, size)?;
        let mut at = 0;
        let mut text = None;
        let mut code = None;
        let mut section_end = 0;
        let mut uuid = None;
        for _ in 0..count {
            let cmd = u32le(commands, at)?;
            let n = u32le(commands, at + 4)? as usize;
            if n < 8 || n % 8 != 0 {
                return Err(Error::Invalid("RPA command length"));
            }
            let c = bytes(commands, at, n)?;
            if cmd == 0x19 {
                if n < 72 {
                    return Err(Error::Invalid("RPA segment command"));
                }
                let name = bytes(c, 8, 16)?;
                let seg = Segment {
                    address: u64le(c, 24)?,
                    file: u64le(c, 40)?,
                    size: u64le(c, 48)?,
                    protection: u32le(c, 60)?,
                };
                if seg.size > 0 {
                    bytes(data, usize64(seg.file)?, usize64(seg.size)?)?;
                }
                if seg.size > u64le(c, 32)? {
                    return Err(Error::Invalid("RPA segment extent"));
                }
                add(seg.address, seg.size)?;
                let sections = u32le(c, 64)? as usize;
                if sections > 128 || 72 + sections * 80 != n {
                    return Err(Error::Invalid("RPA sections"));
                }
                if name == b"__TEXT\0\0\0\0\0\0\0\0\0\0" {
                    if text.is_some() || u32le(c, 56)? != 5 || seg.protection != 5 || seg.file != 0
                    {
                        return Err(Error::Invalid("RPA executable __TEXT mapping"));
                    }
                    for i in 0..sections {
                        let s = bytes(c, 72 + i * 80, 80)?;
                        let address = u64le(s, 32)?;
                        let len = u64le(s, 40)?;
                        let file = u32le(s, 48)? as u64;
                        let end = add(address, len)?;
                        if address < seg.address
                            || end > add(seg.address, seg.size)?
                            || file != address - seg.address
                            || u32le(s, 64)? & 0xff == 1
                        {
                            return Err(Error::Invalid("RPA __TEXT section mapping"));
                        }
                        bytes(data, usize64(file)?, usize64(len)?)?;
                        section_end = section_end.max(end);
                        if bytes(s, 0, 16)? == b"__text\0\0\0\0\0\0\0\0\0\0" {
                            if code.is_some() {
                                return Err(Error::Invalid("RPA duplicate __text"));
                            }
                            code = Some((usize64(file)?, usize64(len)?));
                        }
                    }
                    text = Some(seg);
                }
            } else if cmd == 0x1b {
                if n != 24 || uuid.is_some() {
                    return Err(Error::Invalid("RPA UUID command"));
                }
                uuid = Some(
                    bytes(c, 8, 16)?
                        .try_into()
                        .map_err(|_| Error::Invalid("RPA UUID"))?,
                );
            }
            at = at
                .checked_add(n)
                .ok_or(Error::Invalid("RPA commands overflow"))?;
        }
        if at != commands.len() {
            return Err(Error::Invalid("RPA load command extent"));
        }
        let (code_file, code_size) = code.ok_or(Error::Invalid("RPA no code section"))?;
        Ok(Self {
            data,
            base,
            commands,
            count,
            text: text.ok_or(Error::Invalid("RPA no __TEXT"))?,
            code_file,
            code_size,
            section_end,
            commands_end: 32 + size as u64,
            uuid: uuid.ok_or(Error::Invalid("RPA missing UUID"))?,
        })
    }
    fn address(&self, file: usize) -> Result<u64> {
        add(self.text.address, file as u64)
    }
    fn mapping(&self, address: u64, len: usize, file_backed: bool) -> Result<(usize, u32)> {
        let mut at = 0;
        let mut found = None;
        for _ in 0..self.count {
            let n = u32le(self.commands, at + 4)? as usize;
            let c = bytes(self.commands, at, n)?;
            if u32le(c, 0)? == 0x19 {
                let va = u64le(c, 24)?;
                let size = u64le(c, if file_backed { 48 } else { 32 })?;
                if address >= va && add(address, len as u64)? <= add(va, size)? && size > 0 {
                    if found.is_some() {
                        return Err(Error::Invalid("RPA overlapping VM mappings"));
                    }
                    found = Some((usize64(add(u64le(c, 40)?, address - va)?)?, u32le(c, 60)?));
                }
            }
            at += n;
        }
        found.ok_or(Error::Invalid("RPA unmapped target"))
    }
    fn code_at(&self, address: u64, len: usize) -> Result<&[u8]> {
        let (file, protection) = self.mapping(address, len, true)?;
        if protection != 5
            || file < self.code_file
            || file
                .checked_add(len)
                .is_none_or(|e| e > self.code_file + self.code_size)
        {
            return Err(Error::Invalid("RPA target outside native code"));
        }
        bytes(self.data, file, len)
    }
    fn find(&self, find: &[u8], mask: &[u8]) -> Result<usize> {
        let s = Site {
            find,
            find_mask: mask,
            replace: find,
            replace_mask: &[],
        };
        match s.find_in(bytes(self.data, self.code_file, self.code_size)?)? {
            Match::One(at) => Ok(self.code_file + at),
            Match::None => Err(Error::Invalid("RPA missing signature")),
            Match::Many => Err(Error::Invalid("RPA ambiguous signature")),
        }
    }
    fn call(&self, site: usize, instruction: usize) -> Result<u64> {
        if bytes(self.data, site + instruction, 1)? != [0xe8] {
            return Err(Error::Invalid("RPA native call"));
        }
        let target = rel_target(
            self.data,
            site + instruction + 1,
            self.address(site + instruction + 1)?,
        )?;
        self.code_at(target, 1)?;
        Ok(target)
    }
    fn edit(&self, out: &mut Edit, file: usize, len: usize) -> Result<()> {
        if len == 0 || len > MAX_EDIT_BYTES {
            return Err(Error::Invalid("RPA edit length"));
        }
        let whole = add(self.base, file as u64)?;
        let end = add(whole, len as u64 - 1)?;
        if whole / PAGE != end / PAGE {
            return Err(Error::Invalid("RPA edit crosses validation page"));
        }
        out.file_offset = whole;
        out.len = len as u32;
        out.reserved = 0;
        out.original.fill(0);
        out.replacement.fill(0);
        out.original[..len].copy_from_slice(bytes(self.data, file, len)?);
        out.replacement[..len].copy_from_slice(&out.original[..len]);
        Ok(())
    }
}

pub const COMMON_ADVERTISING: u32 = 1;
pub const COMMON_MSBC: u32 = 2;

fn controller_global(image: &Image<'_>, getter: u64) -> Result<u64> {
    let code = image.code_at(getter, CHIP_GETTER.len())?;
    if !signature(code, CHIP_GETTER, &CHIP_GETTER_MASK) {
        return Err(Error::Invalid("controller getter ABI"));
    }
    let global = rel_target(code, 13, add(getter, 13)?)?;
    if image.mapping(global, 8, false)?.1 & 2 == 0 {
        return Err(Error::Invalid("controller global mapping"));
    }
    Ok(global)
}

fn address_gate(image: &Image<'_>, address: usize, retry: usize) -> Result<()> {
    let setter = image.call(address, 16)?;
    let code = image.code_at(setter, ADDRESS_SETTER.len())?;
    if !signature(code, ADDRESS_SETTER, &ADDRESS_SETTER_MASK) {
        return Err(Error::Invalid("advertising address setter ABI"));
    }
    let gate = rel_target(code, 7, add(setter, 7)?)?;
    let file = image.mapping(gate, 1, true)?.0;
    // cmpb disp32(%rip), imm8 ends one byte beyond its rel32 field.
    let retry_gate = add(
        rel_target(image.data, retry + 34, image.address(retry + 34)?)?,
        1,
    )?;
    if bytes(image.data, file, 1)? != [1] || retry_gate != gate {
        return Err(Error::Invalid("advertising address gates disagree"));
    }
    Ok(())
}

fn checked_site(image: &Image<'_>, site: &Site<'_>) -> Result<usize> {
    site.check()?;
    let file = image.find(site.find, site.find_mask)?;
    let whole = add(image.base, file as u64)?;
    if site.find.len() > PAGE as usize
        || whole / PAGE != add(whole, site.find.len() as u64 - 1)? / PAGE
    {
        return Err(Error::Invalid("common site crosses validation page"));
    }
    Ok(file)
}

/// Independent common-feature validation, not RPA activation. `sites` is the
/// existing byte-descriptor table: extAdv, address, retry, WBS, HFP fill.
/// A missing RPA helper/scan signature or disabled advertiser cannot veto HFP.
pub fn validate_common(full: &[u8], sites: &[Site<'_>]) -> u32 {
    if sites.len() != 5 {
        return 0;
    }
    let Ok(image) = Image::parse(full) else {
        return 0;
    };
    let advertising = (|| -> Result<()> {
        checked_site(&image, &sites[0])?;
        let address = checked_site(&image, &sites[1])?;
        let retry = checked_site(&image, &sites[2])?;
        controller_global(&image, image.call(address, 0)?)?;
        address_gate(&image, address, retry)
    })()
    .is_ok();
    let msbc = (|| -> Result<()> {
        let wbs = checked_site(&image, &sites[3])?;
        let fill = checked_site(&image, &sites[4])?;
        controller_global(&image, image.call(wbs, 4)?)?;
        image.call(fill, 7)?;
        Ok(())
    })()
    .is_ok();
    (if advertising { COMMON_ADVERTISING } else { 0 }) | (if msbc { COMMON_MSBC } else { 0 })
}

/// Caller preallocates `out` off the kernel stack. Errors always leave count=0.
pub fn resolve(full: &[u8], out: &mut Plan) -> Result<()> {
    out.count = 0;
    let image = Image::parse(full)?;
    let legacy = image.find(LEGACY, &LEGACY_MASK)?;
    let scan = image.find(SCAN, &SCAN_MASK)?;
    let decode = image.find(DECODE, &DECODE_MASK)?;
    let device = image.find(DEVICE, &DEVICE_MASK)?;
    let connect = image.find(CONNECT, &CONNECT_MASK)?;
    let update = image.find(UPDATE, &UPDATE_MASK)?;
    let parent = image.find(PARENT, &PARENT_MASK)?;
    let chip_call = image.find(CHIP_CALL, &CHIP_CALL_MASK)?;
    let address_retry = image.find(ADDRESS_RETRY, &ADDRESS_RETRY_MASK)?;
    address_gate(&image, chip_call, address_retry)?;
    let chip_getter = image.call(chip_call, 0)?;
    let global = controller_global(&image, chip_getter)?;
    let sink = image.call(legacy, 15)?;
    if image.address(scan)? < sink
        || image.address(scan)? - sink > 512
        || image.code_at(sink, 16)?
            != [
                0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                0x83, 0xec,
            ]
    {
        return Err(Error::Invalid("RPA scan frame ABI"));
    }
    let getter = image.call(connect, 44)?;
    let paired = image.call(connect, 6)?;
    if image.call(update, 191)? != getter || image.call(update, 14)? != paired {
        return Err(Error::Invalid("RPA initiating paths disagree"));
    }
    if image.code_at(getter, ADDRESS_ENTRY.len())? != ADDRESS_ENTRY
        || image.code_at(add(getter, 149)?, ADDRESS_SELECTION.len())? != ADDRESS_SELECTION
        || image.code_at(paired, PAIRED_ENTRY.len())? != PAIRED_ENTRY
    {
        return Err(Error::Invalid("RPA native address/paired ABI"));
    }
    let device_entry = image.call(parent, 33)?;
    if add(device_entry, DEVICE_ENTRY.len() as u64)? != image.address(device)?
        || !signature(
            image.code_at(device_entry, DEVICE_ENTRY.len())?,
            DEVICE_ENTRY,
            &DEVICE_ENTRY_MASK,
        )
    {
        return Err(Error::Invalid("RPA locked device frame ABI"));
    }
    // The caller and the native setter's lock/unlock calls stay unmodified.
    image.call(device, 0)?;
    image.call(device, 18)?;
    image.code_at(image.call(scan, 34)?, 1)?;

    // Padding follows *every* file-backed __TEXT section. Round to a 256-byte
    // boundary to preserve the historical layout when possible; never scan
    // arbitrary zero runs, reclaim a section, or touch load-command pages.
    let helper = add(image.section_end, 255)? & !255;
    let helper_file = usize64(
        helper
            .checked_sub(image.text.address)
            .ok_or(Error::Invalid("RPA padding address"))?,
    )?;
    let helper_end = add(helper, HELPER.len() as u64)?;
    if helper_end > add(image.text.address, image.text.size)?
        || helper / PAGE != (helper_end - 1) / PAGE
        || helper_file as u64 / PAGE <= (image.commands_end - 1) / PAGE
        || bytes(image.data, helper_file, HELPER.len())?
            .iter()
            .any(|&b| b != 0)
    {
        return Err(Error::Invalid("RPA no safe executable padding"));
    }
    image.edit(&mut out.edits[0], helper_file, HELPER.len())?;
    out.edits[0].replacement[..HELPER.len()].copy_from_slice(HELPER);
    for (field, target) in [
        (9, sink),
        (16, image.address(legacy + 20)?),
        (34, global),
        (90, image.address(parent + 38)?),
        (150, global),
        (207, global),
        (228, getter),
        (235, global),
        (257, paired),
    ] {
        put_rel(
            &mut out.edits[0].replacement,
            field,
            add(helper, field as u64)?,
            target,
        )?;
    }
    for (index, file, len) in [
        (1, legacy, LEGACY.len()),
        (2, scan, SCAN.len()),
        (3, decode, DECODE.len()),
        (4, device, DEVICE.len()),
        (5, connect, CONNECT.len()),
        (6, update, UPDATE.len()),
    ] {
        image.edit(&mut out.edits[index], file, len)?;
    }
    put_rel(
        &mut out.edits[1].replacement,
        16,
        image.address(legacy + 16)?,
        helper,
    )?;
    out.edits[2].replacement[49] = 0xe8;
    put_rel(
        &mut out.edits[2].replacement,
        50,
        image.address(scan + 50)?,
        add(helper, 13)?,
    )?;
    out.edits[2].replacement[54..64].fill(0x90);
    out.edits[3].replacement[..90].fill(0x90);
    out.edits[3].replacement[..DECODE_REPLACEMENT.len()].copy_from_slice(DECODE_REPLACEMENT);
    out.edits[4].replacement[5] = 0xe8;
    put_rel(
        &mut out.edits[4].replacement,
        6,
        image.address(device + 6)?,
        add(helper, 87)?,
    )?;
    out.edits[4].replacement[10..14].fill(0x90);
    for (index, file, fields) in [
        (5, connect, [(7, 232), (45, 202)]),
        (6, update, [(15, 232), (192, 202)]),
    ] {
        for (field, entry) in fields {
            put_rel(
                &mut out.edits[index].replacement,
                field,
                image.address(file + field)?,
                add(helper, entry)?,
            )?;
        }
    }
    // No overlaps and no relocations into headers or non-executable storage.
    for i in 0..MAX_EDITS {
        let a = &out.edits[i];
        let end = add(a.file_offset, a.len as u64)?;
        for b in &out.edits[..i] {
            if a.file_offset < add(b.file_offset, b.len as u64)? && b.file_offset < end {
                return Err(Error::Invalid("RPA overlapping edits"));
            }
        }
    }
    out.slice_offset = image.base;
    out.slice_size = image.data.len() as u64;
    out.text_address = image.text.address;
    out.text_file_offset = image.base;
    out.text_size = image.text.size;
    out.helper_address = helper;
    out.uuid = image.uuid;
    out.reserved = 0;
    out.count = MAX_EDITS as u32;
    Ok(())
}

/// Validate the *whole destination page* before its first write. Returns a bit
/// per edit on this page. Never silently leave one member of an active group
/// native when its expected bytes have changed.
pub fn apply_page(plan: &Plan, offset: u64, page: &mut [u8]) -> Result<u32> {
    if plan.count != MAX_EDITS as u32 || page.len() != PAGE as usize || offset % PAGE != 0 {
        return Err(Error::Invalid("RPA page plan"));
    }
    let end = add(offset, PAGE)?;
    let mut mask = 0;
    for (i, e) in plan.edits.iter().enumerate() {
        if e.file_offset < offset || e.file_offset >= end {
            continue;
        }
        let n = e.len as usize;
        if n == 0 || n > MAX_EDIT_BYTES {
            return Err(Error::Invalid("RPA page edit length"));
        }
        let at = usize64(e.file_offset - offset)?;
        let window = bytes(page, at, n)?;
        if window != &e.original[..n] && window != &e.replacement[..n] {
            return Err(Error::Invalid("RPA destination changed"));
        }
        mask |= 1 << i;
    }
    for (i, e) in plan.edits.iter().enumerate() {
        if mask & (1 << i) == 0 {
            continue;
        }
        let at = usize64(e.file_offset - offset)?;
        let n = e.len as usize;
        page.get_mut(at..at + n)
            .ok_or(Error::Invalid("RPA page write"))?
            .copy_from_slice(&e.replacement[..n]);
    }
    Ok(mask)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plan() -> Box<Plan> {
        let layout = std::alloc::Layout::new::<Plan>();
        // Byte arrays and integers accept an all-zero representation.
        let pointer = unsafe { std::alloc::alloc_zeroed(layout) as *mut Plan };
        if pointer.is_null() {
            std::alloc::handle_alloc_error(layout);
        }
        unsafe { Box::from_raw(pointer) }
    }

    #[test]
    fn signed_relocations_are_checked() {
        let mut code = [0; 4];
        put_rel(&mut code, 0, 0x2000, 0x1000).unwrap();
        assert_eq!(rel_target(&code, 0, 0x2000).unwrap(), 0x1000);
        put_rel(&mut code, 0, 0x1000, 0x2000).unwrap();
        assert_eq!(rel_target(&code, 0, 0x1000).unwrap(), 0x2000);
        assert!(put_rel(&mut code, 0, 0x1000, 0x1_0000_0000).is_err());
        assert!(put_rel(&mut code, 2, 0x1000, 0x2000).is_err());
        assert!(rel_target(&[0xff; 4], 0, u64::MAX).is_err());
    }

    #[test]
    fn malformed_images_leave_the_plan_unpublished() {
        let mut out = plan();
        for length in 0..64 {
            out.count = 7;
            assert!(resolve(&[0; 64][..length], &mut out).is_err());
            assert_eq!(out.count, 0);
        }
    }

    #[test]
    fn page_precheck_is_atomic_and_idempotent() {
        let mut out = plan();
        out.count = 7;
        for (index, at, original, replacement) in [
            (0, 100, [0x11, 0x22], [0xaa, 0xbb]),
            (1, 110, [0x33, 0x44], [0xcc, 0xdd]),
        ] {
            let e = &mut out.edits[index];
            e.file_offset = PAGE + at;
            e.len = 2;
            e.original[..2].copy_from_slice(&original);
            e.replacement[..2].copy_from_slice(&replacement);
        }
        let mut page = [0; 4096];
        page[100..102].copy_from_slice(&[0x11, 0x22]);
        let original = page;
        assert!(apply_page(&out, PAGE, &mut page).is_err());
        assert_eq!(page, original); // The first edit did not leak through.
        page[110..112].copy_from_slice(&[0x33, 0x44]);
        assert_eq!(apply_page(&out, PAGE, &mut page).unwrap(), 3);
        assert_eq!(&page[100..102], &[0xaa, 0xbb]);
        let once = page;
        assert_eq!(apply_page(&out, PAGE, &mut page).unwrap(), 3);
        assert_eq!(page, once);
        assert!(apply_page(&out, PAGE + 1, &mut page).is_err());
        assert!(apply_page(&out, PAGE, &mut page[..4095]).is_err());
    }
}
