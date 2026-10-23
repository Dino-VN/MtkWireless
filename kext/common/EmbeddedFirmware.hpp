//
//  EmbeddedFirmware.hpp
//  MtkWireless
//
//  Firmware embedded at build time (zlib-compressed, see
//  scripts/embed_firmware.py), inflated on demand with the kernel's zlib.
//  Each kext compiles this with its own generated firmware_table.inc.
//

#ifndef MtkWireless_EmbeddedFirmware_hpp
#define MtkWireless_EmbeddedFirmware_hpp

#include <libkern/c++/OSData.h>

/* Decompressed image, or NULL if unknown or corrupt. Caller releases. */
OSData *mtkCopyEmbeddedFirmware(const char *name);

#endif /* MtkWireless_EmbeddedFirmware_hpp */
