//! Offline resolver inspection. Reads bytes; never runs or modifies bluetoothd.
use mtk_bt::rpa::{self, Plan};

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn main() {
    let path = std::env::args_os()
        .nth(1)
        .expect("usage: rpa-plan /path/to/bluetoothd");
    let image = std::fs::read(path).expect("read binary");
    // All fields are integers/byte arrays, so an all-zero allocation is valid.
    // Avoid constructing the large plan on the kernel-sized stack in examples.
    let raw = unsafe { std::alloc::alloc_zeroed(std::alloc::Layout::new::<Plan>()) as *mut Plan };
    if raw.is_null() {
        std::alloc::handle_alloc_error(std::alloc::Layout::new::<Plan>());
    }
    let mut plan = unsafe { Box::from_raw(raw) };
    if let Err(error) = rpa::resolve(&image, &mut plan) {
        eprintln!("RPA plan refused: {error:?}");
        std::process::exit(1);
    }
    // Exercise the actual complete-page applier on an owned copy, then again
    // for idempotence. The supplied file is left untouched.
    let mut patched = image.clone();
    for edit in &plan.edits {
        let page = (edit.file_offset & !4095) as usize;
        let len = patched.len();
        if page + 4096 > len {
            panic!("destination page outside file");
        }
        rpa::apply_page(&plan, page as u64, &mut patched[page..page + 4096])
            .expect("page precheck/apply");
    }
    let once = patched.clone();
    for edit in &plan.edits {
        let page = (edit.file_offset & !4095) as usize;
        rpa::apply_page(&plan, page as u64, &mut patched[page..page + 4096]).expect("idempotence");
    }
    assert_eq!(once, patched);
    println!(
        "{{\"uuid\":\"{}\",\"slice_offset\":{},\"slice_size\":{},\"text_address\":{},\"helper_address\":{},\"edits\":[",
        hex(&plan.uuid),
        plan.slice_offset,
        plan.slice_size,
        plan.text_address,
        plan.helper_address
    );
    for (i, edit) in plan.edits.iter().enumerate() {
        println!(
            "{}{{\"file_offset\":{},\"len\":{},\"original\":\"{}\",\"replacement\":\"{}\"}}",
            if i == 0 { "" } else { "," },
            edit.file_offset,
            edit.len,
            hex(&edit.original[..edit.len as usize]),
            hex(&edit.replacement[..edit.len as usize])
        );
    }
    println!("]}}");
}
