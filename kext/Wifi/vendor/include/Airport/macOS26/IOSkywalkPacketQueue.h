// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_TAHOE_SKYWALK_QUEUE_H
#define MTK_TAHOE_SKYWALK_QUEUE_H
#include <IOKit/IOEventSource.h>
#include "IOSkywalkPacketBufferPool.h"
class IOSkywalkPacketPoller;

// Native factory objects only; inherited IOEventSource slots and appended
// slots checked against the installed IOSkywalkFamily. No private field access.
class IOSkywalkPacketQueue : public IOEventSource {
    OSDeclareAbstractStructors(IOSkywalkPacketQueue)
public:
    void free() override;
    void disable() override;
    virtual IOReturn initialize(void*);
    virtual IOReturn finalize();
    virtual IOReturn synchronize(unsigned);
    virtual IOReturn packetSubmission(unsigned long long*,unsigned*,unsigned*);
    virtual IOReturn packetSubmission(bool);
    virtual IOReturn sendNotification(unsigned);
    virtual IOSkywalkPacketBufferPool *getPacketBufferPool() const;
    virtual void setPacketBufferPool(IOSkywalkPacketBufferPool*);
    virtual unsigned getPacketCount() const=0;
    virtual unsigned getPacketBufferCount() const;
    virtual unsigned getFreeSpace() const;
    virtual void setPacketPoller(IOSkywalkPacketPoller*);
    virtual IOReturn performCommand(unsigned,void*,unsigned long);
    virtual void packetCompletion(IOSkywalkPacket*,IOSkywalkPacketQueue*,unsigned);
    virtual void bufferCompletion(IOSkywalkPacketBuffer*,IOSkywalkPacketQueue*,unsigned);
    virtual bool initWithPool(IOSkywalkPacketBufferPool*,unsigned,unsigned,unsigned,unsigned,OSObject*,Action,void*);
    virtual void packetCompletion(unsigned,unsigned,bool);
    virtual void _RESERVEDIOSkywalkPacketQueue0();
    virtual void _RESERVEDIOSkywalkPacketQueue1();
    virtual void _RESERVEDIOSkywalkPacketQueue2();
    virtual void _RESERVEDIOSkywalkPacketQueue3();
    virtual void _RESERVEDIOSkywalkPacketQueue4();
    virtual void _RESERVEDIOSkywalkPacketQueue5();
    virtual void _RESERVEDIOSkywalkPacketQueue6();
    virtual void _RESERVEDIOSkywalkPacketQueue7();
    virtual void _RESERVEDIOSkywalkPacketQueue8();
    virtual void _RESERVEDIOSkywalkPacketQueue9();
    virtual void _RESERVEDIOSkywalkPacketQueue10();
};
class IOSkywalkTxSubmissionQueue : public IOSkywalkPacketQueue {
    OSDeclareAbstractStructors(IOSkywalkTxSubmissionQueue)
public:
    using Submit=unsigned (*)(OSObject*,IOSkywalkTxSubmissionQueue*,IOSkywalkPacket *const*,unsigned,void*);
    using Capacity=unsigned (*)(OSObject*,IOSkywalkTxSubmissionQueue*,unsigned*);
    bool checkForWork() override;
    void enable() override;
    void disable() override;
    IOReturn initialize(void*) override;
    IOReturn finalize() override;
    IOReturn synchronize(unsigned) override;
    IOReturn packetSubmission(unsigned long long*,unsigned*,unsigned*) override;
    IOReturn packetSubmission(bool) override;
    IOReturn sendNotification(unsigned) override;
    unsigned getPacketCount() const override;
    IOReturn performCommand(unsigned,void*,unsigned long) override;
    void packetCompletion(IOSkywalkPacket*,IOSkywalkPacketQueue*,unsigned) override;
    void packetCompletion(unsigned,unsigned,bool) override;
    virtual IOReturn requestDequeue(void*,unsigned);
    virtual void purgePackets();
    virtual bool initWithPool(IOSkywalkPacketBufferPool*,unsigned,unsigned,unsigned,OSObject*,Capacity,Submit,void*,unsigned);
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool*,unsigned,unsigned,OSObject*,Submit,void*,unsigned);
};
class IOSkywalkRxCompletionQueue : public IOSkywalkPacketQueue {
    OSDeclareAbstractStructors(IOSkywalkRxCompletionQueue)
public:
    using Enqueue=unsigned (*)(OSObject*,IOSkywalkRxCompletionQueue*,IOSkywalkPacket**,unsigned,void*);
    bool checkForWork() override;
    void enable() override;
    void disable() override;
    IOReturn initialize(void*) override;
    IOReturn finalize() override;
    IOReturn synchronize(unsigned) override;
    IOReturn sendNotification(unsigned) override;
    unsigned getPacketCount() const override;
    void setPacketPoller(IOSkywalkPacketPoller*) override;
    IOReturn performCommand(unsigned,void*,unsigned long) override;
    virtual IOReturn requestEnqueue(void*,unsigned);
    virtual IOReturn enqueuePackets(IOSkywalkPacket *const*,unsigned,unsigned);
    virtual IOReturn enqueuePackets(IOSkywalkPacket*,unsigned,unsigned);
    virtual bool initWithPool(IOSkywalkPacketBufferPool*,unsigned,unsigned,OSObject*,Enqueue,void*,unsigned);
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool*,unsigned,unsigned,OSObject*,Enqueue,void*,unsigned);
};
class IOSkywalkTxCompletionQueue : public IOSkywalkPacketQueue {
    OSDeclareAbstractStructors(IOSkywalkTxCompletionQueue)
public:
    using Enqueue=unsigned (*)(OSObject*,IOSkywalkTxCompletionQueue*,IOSkywalkPacket**,unsigned,void*);
    bool checkForWork() override;
    void enable() override;
    void disable() override;
    IOReturn initialize(void*) override;
    IOReturn finalize() override;
    unsigned getPacketCount() const override;
    void setPacketPoller(IOSkywalkPacketPoller*) override;
    IOReturn performCommand(unsigned,void*,unsigned long) override;
    virtual IOReturn requestEnqueue(void*,unsigned);
    virtual IOReturn enqueuePackets(IOSkywalkPacket *const*,unsigned,unsigned);
    virtual IOReturn enqueuePackets(IOSkywalkPacket*,unsigned,unsigned);
    virtual bool initWithPool(IOSkywalkPacketBufferPool*,unsigned,unsigned,OSObject*,Enqueue,void*,unsigned);
    static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool*,unsigned,unsigned,OSObject*,Enqueue,void*,unsigned);
};
#endif
