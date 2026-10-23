//
//  mtk_rust.h
//  MtkWireless
//
//  C ABI of the Rust core (rust/mtk-ffi). Keep in sync with mtk-ffi/src/lib.rs.
//

#ifndef mtk_rust_h
#define mtk_rust_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receives one NUL-terminated line without a trailing newline. */
typedef void (*mtk_rust_log_fn)(const char *msg);

/* Route Rust logging to `sink`; NULL restores the IOLog default. */
void mtk_rust_set_log(mtk_rust_log_fn sink);

/* Static "<version> <component>" string, e.g. "0.1.0 wifi". */
const char *mtk_rust_version(void);

/* Runtime and table checks; returns a bitmask of failures, 0 = all passed. */
uint32_t mtk_rust_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* mtk_rust_h */
