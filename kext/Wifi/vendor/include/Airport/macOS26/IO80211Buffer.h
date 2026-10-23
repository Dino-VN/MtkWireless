// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_TAHOE_BUFFER_H
#define MTK_TAHOE_BUFFER_H
#include <libkern/c++/OSObject.h>
// Borrowed native objects only: no allocation or private data layout here.
// Virtual slots verified against macOS 26.7.1 IO80211Family.
class IO80211Buffer : public OSObject {
    OSDeclareAbstractStructors(IO80211Buffer)
public:
    void free() override;
    virtual void returnBuffer();                 // 0x118
    virtual void zero();
    virtual bool isZero() const;
    virtual unsigned long getLength() const;
    virtual void setLength(unsigned long);
    virtual unsigned long getCapacity() const;
    virtual void *getCursor();
    virtual const void *getCursor() const;
    virtual void *getBytesNoCopy();               // 0x158
};
class IO80211BufferPool : public OSObject {
    OSDeclareAbstractStructors(IO80211BufferPool)
public:
    void free() override;
    virtual IO80211Buffer *getBuffer();           // 0x118
    virtual void returnBuffer(IO80211Buffer*);
    virtual void flushPool();
    virtual void resetPool();
};
#endif
