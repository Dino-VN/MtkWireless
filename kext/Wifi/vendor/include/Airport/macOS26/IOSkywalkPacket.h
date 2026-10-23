// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_TAHOE_SKYWALK_PACKET_H
#define MTK_TAHOE_SKYWALK_PACKET_H
#include <IOKit/IOCommand.h>

class IOSkywalkPacketBufferPool;
class IOSkywalkPacketDescriptor;
class IOSkywalkPacketBuffer;
class IOSkywalkPacketQueue;

// Borrowed native packets only. Slot order is pinned to Tahoe's IOCommand
// superclass and IOSkywalkPacket vtable. Never allocate using this declaration.
class IOSkywalkPacket : public IOCommand {
    OSDeclareAbstractStructors(IOSkywalkPacket)
public:
    void free() override;
    virtual bool initWithPool(IOSkywalkPacketBufferPool*,IOSkywalkPacketDescriptor*,unsigned);
    virtual unsigned getPacketBuffers(IOSkywalkPacketBuffer**,unsigned) const;
    virtual unsigned getPacketBufferCount() const;
    virtual IOSkywalkPacketDescriptor *getMemoryDescriptor() const;
    virtual IOReturn setDataLength(unsigned);
    virtual unsigned getDataLength() const;
    virtual IOReturn setDataOffset(unsigned short);
    virtual unsigned short getDataOffset() const;
    virtual IOReturn setDataOffsetAndLength(unsigned short,unsigned);
    virtual IOReturn setDataOff(long long);
    virtual long long getDataOff() const;
    virtual IOReturn setDataOffAndLen(long long,unsigned long);
    virtual void *getDataVirtualAddress() const;
    virtual void *getDataIOVirtualAddress() const;
    virtual IOReturn prepareWithQueue(IOSkywalkPacketQueue*,unsigned,unsigned);
    virtual IOReturn prepare(IOSkywalkPacketQueue*,unsigned long long,unsigned);
    virtual IOReturn completeWithQueue(IOSkywalkPacketQueue*,unsigned,unsigned);
    virtual IOReturn complete(IOSkywalkPacketQueue*,unsigned);
    virtual unsigned getPacketType() const;
    virtual void setWakeFlag();
    virtual unsigned getTraceID() const;
    virtual void setTraceID(unsigned);
    virtual void traceEvent(unsigned);
    virtual void *generateTraceTag(IOSkywalkPacketQueue*) const;
    virtual IOReturn acquireWithPacketHandle(unsigned long long,unsigned);
    virtual void disposePacket();
};
#endif
