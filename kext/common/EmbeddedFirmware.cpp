//
//  EmbeddedFirmware.cpp
//  MtkWireless
//

#include "EmbeddedFirmware.hpp"

#include <IOKit/IOLib.h>
#include <libkern/zlib.h>

struct FirmwareEntry {
    const char *name;
    const unsigned char *data;
    const unsigned char *end;
    uint32_t size; /* decompressed */
};

/* Generated: build/gen/<kext>/firmware_table.inc */
#include "firmware_table.inc"

namespace {

/* zlib allocator: the size lives in a 16-byte header so IOFree gets it back
 * and the returned pointer keeps 16-byte alignment. */
constexpr size_t kHeader = 16;

void *zAlloc(void *, uInt items, uInt size)
{
    if (size && items > (UINT32_MAX - kHeader) / size)
        return nullptr;
    size_t total = (size_t)items * size + kHeader;
    auto *p = static_cast<uint8_t *>(IOMalloc(total));
    if (!p)
        return nullptr;
    *reinterpret_cast<size_t *>(p) = total;
    return p + kHeader;
}

void zFree(void *, void *ptr)
{
    if (!ptr)
        return;
    auto *p = static_cast<uint8_t *>(ptr) - kHeader;
    IOFree(p, *reinterpret_cast<size_t *>(p));
}

OSData *inflateFirmware(const FirmwareEntry &fw)
{
    OSData *out = OSData::withCapacity(fw.size);
    if (!out)
        return nullptr;

    enum { kChunk = 4096 };
    auto *chunk = static_cast<Bytef *>(IOMalloc(kChunk));
    if (!chunk) {
        out->release();
        return nullptr;
    }
    z_stream stream {};
    stream.next_in = const_cast<Bytef *>(fw.data);
    stream.avail_in = (uInt)(fw.end - fw.data);
    stream.zalloc = zAlloc;
    stream.zfree = zFree;

    bool ok = inflateInit(&stream) == Z_OK;
    if (ok) {
        int err;
        do {
            stream.next_out = chunk;
            stream.avail_out = kChunk;
            err = inflate(&stream, Z_NO_FLUSH);
            uInt produced = kChunk - stream.avail_out;
            ok = (err == Z_OK || err == Z_STREAM_END) &&
                 out->getLength() + produced <= fw.size &&
                 (!produced || out->appendBytes(chunk, produced));
        } while (ok && err != Z_STREAM_END);
        inflateEnd(&stream);
    }
    IOFree(chunk, kChunk);
    if (!ok || out->getLength() != fw.size) {
        out->release();
        return nullptr;
    }
    return out;
}

} // namespace

OSData *mtkCopyEmbeddedFirmware(const char *name)
{
    for (const FirmwareEntry &fw : kFirmware)
        if (strcmp(fw.name, name) == 0)
            return inflateFirmware(fw);
    return nullptr;
}
