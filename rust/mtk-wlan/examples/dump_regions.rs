//! Prints the download plan of firmware images (for comparing with the C HAL).
use mtk_wlan::connac2::fw::{PatchImage, RamImage};

fn main() {
    for path in std::env::args().skip(1) {
        let data = std::fs::read(&path).expect("read");
        let name = path.rsplit('/').next().unwrap();
        if name.contains("patch") {
            let img = PatchImage::parse(&data).expect("patch");
            for i in 0..img.regions {
                let r = img.region(i).expect("region");
                println!("{name} P {i} {:#x} {} {:#x} {} 1", r.addr, r.data.len(), r.mode, r.offset);
            }
        } else {
            let img = RamImage::parse(&data).expect("ram");
            for i in 0..img.regions {
                let r = img.region(i).expect("region");
                println!("{name} R {i} {:#x} {} {:#x} {} {}", r.addr, r.data.len(), r.mode, r.offset, r.download as u8);
            }
        }
    }
}
