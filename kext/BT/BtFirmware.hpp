//
//  BtFirmware.hpp
//  MtkWireless
//
//  Firmware embedded at build time (zlib-compressed, see
//  scripts/embed_firmware.py), inflated on demand with the kernel's zlib.
//

#ifndef MtkWirelessBT_BtFirmware_hpp
#define MtkWirelessBT_BtFirmware_hpp

#include <libkern/c++/OSData.h>

/* Decompressed image, or NULL if unknown or corrupt. Caller releases. */
OSData *mtkBtCopyFirmware(const char *name);

#endif /* MtkWirelessBT_BtFirmware_hpp */
