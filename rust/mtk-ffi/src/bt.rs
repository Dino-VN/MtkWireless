//! C ABI of the Bluetooth core. Header: `kext/BT/mtk_bt.h`.

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_void};
use core::fmt;

use mtk_bt::hci;
use mtk_bt::le_features::{self, LeFeatures, Reply};
use mtk_bt::setup::{self, IoError, Transport};
use mtk_kernel::klog;

/// USB access provided by the C++ loader. All callbacks return an IOReturn.
#[repr(C)]
pub struct MtkBtTransport {
    pub ctx: *mut c_void,
    pub reg_read: unsafe extern "C" fn(ctx: *mut c_void, reg: u32, value: *mut u32) -> i32,
    pub uhw_read: unsafe extern "C" fn(ctx: *mut c_void, reg: u32, value: *mut u32) -> i32,
    pub uhw_write: unsafe extern "C" fn(ctx: *mut c_void, reg: u32, value: u32) -> i32,
    pub wmt: unsafe extern "C" fn(
        ctx: *mut c_void,
        cmd: *const u8,
        cmd_len: u32,
        event: *mut u8,
        event_cap: u32,
        event_len: *mut u32,
    ) -> i32,
    pub sleep_ms: unsafe extern "C" fn(ctx: *mut c_void, ms: u32),
    /// Returns a handle for `fw_release` and the decompressed image, or NULL.
    pub fw_load: unsafe extern "C" fn(
        ctx: *mut c_void,
        name: *const c_char,
        data: *mut *const u8,
        len: *mut usize,
    ) -> *const c_void,
    pub fw_release: unsafe extern "C" fn(ctx: *mut c_void, handle: *const c_void),
}

#[repr(C)]
pub struct MtkBtSetupInfo {
    pub dev_id: u32,
    pub fw_version: u32,
    pub fw_flavor: u32,
    pub firmware: [c_char; 49],
}

struct CTransport<'a>(&'a MtkBtTransport);

struct CFirmware<'a> {
    t: &'a MtkBtTransport,
    handle: *const c_void,
    data: &'a [u8],
}

impl AsRef<[u8]> for CFirmware<'_> {
    fn as_ref(&self) -> &[u8] {
        self.data
    }
}

impl Drop for CFirmware<'_> {
    fn drop(&mut self) {
        unsafe { (self.t.fw_release)(self.t.ctx, self.handle) }
    }
}

fn status(ret: i32) -> Result<(), IoError> {
    if ret == 0 { Ok(()) } else { Err(IoError(ret)) }
}

impl<'a> Transport for CTransport<'a> {
    type Firmware = CFirmware<'a>;

    fn reg_read(&mut self, reg: u32) -> Result<u32, IoError> {
        let mut v = 0;
        status(unsafe { (self.0.reg_read)(self.0.ctx, reg, &mut v) })?;
        Ok(v)
    }

    fn uhw_read(&mut self, reg: u32) -> Result<u32, IoError> {
        let mut v = 0;
        status(unsafe { (self.0.uhw_read)(self.0.ctx, reg, &mut v) })?;
        Ok(v)
    }

    fn uhw_write(&mut self, reg: u32, value: u32) -> Result<(), IoError> {
        status(unsafe { (self.0.uhw_write)(self.0.ctx, reg, value) })
    }

    fn wmt(&mut self, command: &[u8], event: &mut [u8]) -> Result<usize, IoError> {
        let mut got = 0u32;
        status(unsafe {
            (self.0.wmt)(
                self.0.ctx,
                command.as_ptr(),
                command.len() as u32,
                event.as_mut_ptr(),
                event.len() as u32,
                &mut got,
            )
        })?;
        Ok((got as usize).min(event.len()))
    }

    fn sleep_ms(&mut self, ms: u32) {
        unsafe { (self.0.sleep_ms)(self.0.ctx, ms) }
    }

    fn load_firmware(&mut self, name: &str) -> Option<CFirmware<'a>> {
        let mut cname = [0u8; 64];
        let bytes = name.as_bytes();
        if bytes.len() >= cname.len() {
            return None;
        }
        cname[..bytes.len()].copy_from_slice(bytes);
        let (mut data, mut len) = (core::ptr::null(), 0usize);
        let handle =
            unsafe { (self.0.fw_load)(self.0.ctx, cname.as_ptr().cast(), &mut data, &mut len) };
        if handle.is_null() {
            return None;
        }
        let data = if data.is_null() {
            &[][..]
        } else {
            // SAFETY: the loader keeps the image alive until fw_release.
            unsafe { core::slice::from_raw_parts(data, len) }
        };
        Some(CFirmware {
            t: self.0,
            handle,
            data,
        })
    }

    fn log(&mut self, args: fmt::Arguments<'_>) {
        klog!("{args}");
    }
}

/// Run the controller bring-up. Returns 0 and fills `info`, or a negative
/// error code (the reason is logged).
///
/// # Safety
/// `t` must be valid for the call; `info` may be NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_setup(t: *const MtkBtTransport, info: *mut MtkBtSetupInfo) -> i32 {
    let Some(t) = (unsafe { t.as_ref() }) else {
        return -100;
    };
    match setup::setup(&mut CTransport(t)) {
        Ok(result) => {
            if let Some(info) = unsafe { info.as_mut() } {
                info.dev_id = result.dev_id;
                info.fw_version = result.fw_version;
                info.fw_flavor = result.fw_flavor;
                for (dst, src) in info.firmware.iter_mut().zip(result.firmware.to_c_bytes()) {
                    *dst = src as c_char;
                }
            }
            0
        }
        Err(e) => {
            klog!("setup failed: {e:?}");
            e.code()
        }
    }
}

/// WMT FUNC_CTRL on/off.
///
/// # Safety
/// `t` must be valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_enable(t: *const MtkBtTransport, enable: bool) -> i32 {
    let Some(t) = (unsafe { t.as_ref() }) else {
        return -100;
    };
    match setup::enable_bluetooth(&mut CTransport(t), enable) {
        Ok(()) => 0,
        Err(e) => {
            klog!("FUNC_CTRL {enable} failed: {e:?}");
            e.code()
        }
    }
}

unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if ptr.is_null() || len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(ptr, len) }
    }
}

unsafe fn bytes_mut<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
    if ptr.is_null() || len == 0 {
        &mut []
    } else {
        unsafe { core::slice::from_raw_parts_mut(ptr, len) }
    }
}

/// Rewrites HCI_Read_Clock(…, Which_Clock = 0) in an EP0 OUT command.
///
/// # Safety
/// `cmd` must point to `len` writable bytes (or be NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_fix_read_clock(
    bm_request_type: u8,
    cmd: *mut u8,
    len: usize,
) -> bool {
    hci::is_hci_command_request(bm_request_type)
        && hci::fix_read_clock(unsafe { bytes_mut(cmd, len) })
}

#[unsafe(no_mangle)]
pub extern "C" fn mtk_bt_acl_candidate(ep_attributes: u8, ep_address: u8, length: u32) -> bool {
    hci::is_acl_out_candidate(ep_attributes, ep_address, length)
}

/// For a flushable ACL start fragment, stores the new header byte 1.
///
/// # Safety
/// `header` must point to 4 bytes, `byte1` to one writable byte.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_acl_non_flushable(
    header: *const u8,
    length: u32,
    byte1: *mut u8,
) -> bool {
    let h = unsafe { bytes(header, 4) };
    let (Ok(h), Some(out)) = (<[u8; 4]>::try_from(h), unsafe { byte1.as_mut() }) else {
        return false;
    };
    match hci::acl_non_flushable(h, length) {
        Some(b) => {
            *out = b;
            true
        }
        None => false,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn mtk_bt_le_candidate_request(
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_index: u16,
    w_length: u16,
) -> bool {
    le_features::is_candidate_request(bm_request_type, b_request, w_value, w_index, w_length)
}

/// Shared LE feature-mask state. The C++ routes serialize every call with one
/// IOLock (`mtk_bt.h`), so plain interior mutability is enough.
struct LeState(UnsafeCell<LeFeatures>);

// SAFETY: all access goes through the functions below, which the caller
// serializes.
unsafe impl Sync for LeState {}

static LE: LeState = LeState(UnsafeCell::new(LeFeatures::new()));

unsafe fn le() -> &'static mut LeFeatures {
    unsafe { &mut *LE.0.get() }
}

/// # Safety
/// Caller holds the LE lock; `cmd` points to `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_le_command(id: u64, cmd: *const u8, len: usize) -> u64 {
    unsafe { le().command(id, bytes(cmd, len)) }
}

/// # Safety
/// Caller holds the LE lock.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_le_cancel(id: u64, token: u64) {
    unsafe { le().cancel(id, token) }
}

/// # Safety
/// Caller holds the LE lock. `pipe` is only compared, never dereferenced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_le_controller_for_pipe(pipe: *const c_void) -> u64 {
    unsafe { le().controller_for_pipe(pipe as usize) }
}

/// # Safety
/// Caller holds the LE lock. `pipe` is only compared, never dereferenced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_le_bind_event_pipe(pipe: *const c_void, id: u64, mediatek: bool) {
    unsafe { le().bind_event_pipe(pipe as usize, id, mediatek) }
}

#[repr(C)]
pub struct MtkBtLeReceived {
    /// 0 none, 1 answer left unchanged, 2 rewritten.
    pub reply: u32,
    pub offset: usize,
    pub original: [u8; 6],
}

/// # Safety
/// Caller holds the LE lock; `events` points to `len` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mtk_bt_le_receive(
    id: u64,
    events: *mut u8,
    len: usize,
) -> MtkBtLeReceived {
    let r = unsafe { le().receive(id, bytes_mut(events, len)) };
    MtkBtLeReceived {
        reply: match r.reply {
            Reply::None => 0,
            Reply::Unchanged => 1,
            Reply::Rewritten => 2,
        },
        offset: r.offset,
        original: r.original,
    }
}
