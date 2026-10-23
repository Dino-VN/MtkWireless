// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_TAHOE_SKYWALK_POOL_H
#define MTK_TAHOE_SKYWALK_POOL_H
#include <libkern/c++/OSObject.h>
#include "IOSkywalkPacket.h"
class IOSkywalkMemorySegment;
class IOSkywalkMemorySegmentDescriptor;
class IOSkywalkPacketBufferDescriptor;

// Factory-owned native pools. Do not use the older generic Airport header:
// allocation/newPacket return IOReturn and the native pool is 0xc0 bytes.
class IOSkywalkPacketBufferPool : public OSObject {
    OSDeclareAbstractStructors(IOSkywalkPacketBufferPool)
public:
    struct PoolOptions {
        uint32_t packetCount, bufferCount, bufferSize, maxBuffersPerPacket;
        uint32_t memorySegmentSize, poolFlags;
        uint64_t reserved;
    };
    void free() override;
    virtual bool initWithName(const char*,void*,unsigned,const PoolOptions*);
    virtual bool initWithName(const char*,OSObject*,unsigned,const PoolOptions*);
    virtual IOReturn allocatePacket(IOSkywalkPacket**,unsigned);
    virtual IOReturn allocatePacket(unsigned,IOSkywalkPacket**,unsigned);
    virtual IOReturn allocatePackets(unsigned,unsigned*,IOSkywalkPacket**,unsigned);
    virtual void deallocatePacket(IOSkywalkPacket*);
    virtual void deallocatePackets(IOSkywalkPacket**,unsigned);
    virtual void deallocatePacketList(IOSkywalkPacket*);
    virtual void deallocatePacketChain(unsigned long long);
    virtual IOReturn allocatePacketBuffer(IOSkywalkPacketBuffer**,unsigned);
    virtual IOReturn allocatePacketBuffers(unsigned*,IOSkywalkPacketBuffer**,unsigned);
    virtual void deallocatePacketBuffer(IOSkywalkPacketBuffer*);
    virtual void deallocatePacketBuffers(IOSkywalkPacketBuffer**,unsigned);
    virtual IOReturn newPacket(IOSkywalkPacketDescriptor*,IOSkywalkPacket**);
    virtual IOReturn newPacketBuffer(IOSkywalkPacketBufferDescriptor*,IOSkywalkPacketBuffer**);
    virtual IOReturn newMemorySegment(IOSkywalkMemorySegmentDescriptor*,IOSkywalkMemorySegment**);
    static IOSkywalkPacketBufferPool *withName(const char*,OSObject*,unsigned,const PoolOptions*);
};
static_assert(sizeof(IOSkywalkPacketBufferPool::PoolOptions)==0x20,"Tahoe pool options ABI");
#endif
