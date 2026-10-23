// `cfg(kext)`: building for targets/x86_64-apple-kext.json (kernel runtime,
// no_std) rather than the host (unit tests with std).
fn main() {
    println!("cargo::rustc-check-cfg=cfg(kext)");
    if std::env::var("TARGET").as_deref() == Ok("x86_64-apple-kext") {
        println!("cargo::rustc-cfg=kext");
    }
}
