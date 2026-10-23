//! Global allocator and panic handler for the kext target.

use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_void};
use core::fmt::Write;

use crate::log::LineBuf;

unsafe extern "C" {
    fn IOMalloc(size: usize) -> *mut c_void;
    fn IOFree(ptr: *mut c_void, size: usize);
    fn IOMallocAligned(size: usize, alignment: usize) -> *mut c_void;
    fn IOFreeAligned(ptr: *mut c_void, size: usize);
    fn panic(format: *const c_char, ...) -> !;
}

/// IOMalloc guarantees pointer alignment; larger alignments go through
/// IOMallocAligned. Free must use the same size and the same family.
const IOMALLOC_ALIGN: usize = 8;

struct KernelAlloc;

unsafe impl GlobalAlloc for KernelAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let ptr = if layout.align() <= IOMALLOC_ALIGN {
            unsafe { IOMalloc(layout.size()) }
        } else {
            unsafe { IOMallocAligned(layout.size(), layout.align()) }
        };
        ptr.cast()
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= IOMALLOC_ALIGN {
            unsafe { IOFree(ptr.cast(), layout.size()) }
        } else {
            unsafe { IOFreeAligned(ptr.cast(), layout.size()) }
        }
    }
}

#[global_allocator]
static ALLOCATOR: KernelAlloc = KernelAlloc;

/// A Rust panic is a bug in the kext; unwinding is not available, so stop the
/// machine with the location and message in the panic log.
#[panic_handler]
fn on_panic(info: &core::panic::PanicInfo<'_>) -> ! {
    let mut line = LineBuf::new();
    let _ = write!(line, "MtkWireless Rust core: {info}");
    unsafe { panic(c"%s".as_ptr(), line.as_c_ptr()) }
}
