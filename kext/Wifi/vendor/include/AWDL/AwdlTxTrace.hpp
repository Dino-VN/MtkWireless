// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_TX_TRACE_HPP
#define MTK_AWDL_TX_TRACE_HPP
#include <stdint.h>
#include <stddef.h>
#include <mtk_wlan.h>

namespace mtkawdl {
// MtkWireless: diagnostic ONLY. The HAL owns this on the heap and serializes
// every operation with sc_awdl_lock. Nothing here changes a descriptor, rate,
// ACK policy, native UMI result, or the lifetime of a DMA buffer.
//
// Immutable event rows deliberately have separate event/ticket ids: a late
// completion APPENDS its submitting token's metadata, never edits a wrapped
// submission row. live[] rejects an obsolete copy of a reused token's ticket.
struct HalTxTrace {
    static constexpr unsigned capacity=128, txsCapacity=64, tokenCapacity=512;
    enum : uint8_t { Empty=0, Prepared=1, Submitted=2 };
    enum TokenFlags : uint8_t {
        SequenceKnown=1, RequestedRateKnown=2, ChannelKnown=4,
        ContextKnown=8, AmbiguousToken=16, UnassignedFree=32, LengthKnown=64
    };
    enum ObserverFlags : uint8_t {
        Enabled=1, Stopping=2, Resetting=4, Detaching=8, PeerPresent=16,
        Running=32
    };
    enum Kind : uint8_t { Submit=1, SubmitFailure=2, TxFree=3, Reclaim=4, UnattributedFree=5 };
    enum RecordFlags : uint32_t {
        FreePresent=1, Attributed=2, ContextCurrent=4, LatencyKnown=8,
        CountKnown=16, SessionChanged=32, EpochChanged=64, PeerChanged=128,
        ContextInactive=256, TrackingUnavailable=512, TokenUnsafe=1024
    };
    enum Faults : uint64_t {
        CounterOverflow=1, TicketExhausted=2, EventExhausted=4, TxsExhausted=8,
        PeerExhausted=16, MembershipLost=32, SnapshotMismatch=64
    };
    // Shared-pool safety applies to EVERY occupant, even when it has no AWDL
    // ticket. These private bits do not change the published record layouts.
    enum : uint8_t { PoolInFlight=1, Unsafe=2 };

    // Lives in the actual mwx_txwi object. Zeroed on every failure/reclaim.
    // submittedAtUS is set just before the successful DMA doorbell, not at
    // allocation/mapping. Rate/sequence come from our cached, built TXWI.
    struct Token {
        uint64_t id, session, incarnation, submittedAtUS;
        uint32_t epoch, length;
        uint16_t token, wcid, sequence, requestedRate;
        uint8_t channel, pid, state, flags;
        uint8_t reserved[4];
    };
    struct Observer {
        uint64_t session, incarnation;
        uint32_t epoch;
        uint8_t flags, channel;
        uint8_t reserved[2];
    };
    struct Record {
        uint64_t id, atUS, latencyUS;
        Token submitted;
        uint32_t raw, hostError;
        uint16_t count;
        uint8_t status, kind;
        uint32_t flags;
    };
    // NO ticket or token: TXS cannot establish a submitting session or peer
    // incarnation. These fields name the OBSERVER at receipt, not the sender.
    struct TxsRecord {
        uint64_t id, atUS, observerSession, observerIncarnation;
        uint32_t raw[8];
        uint32_t observerEpoch, errors;
        uint16_t rate, wcid, sequence;
        uint8_t pid, format, observerFlags, observerChannel;
        uint8_t reserved[6];
    };
    // All fields/array elements below are u64 words, in this declared order.
    // Counters never reset on sync-off, peer replacement, or a HAL reset.
    struct Counters {
        uint64_t prepared, submitted, submitFailed, freeAcked, freeFailed;
        uint64_t freeCurrent, freeNotCurrent, freeUnattributed, reclaimUnobserved;
        uint64_t tokenTaints, metadataRejected, skipped, idleFree, outOfRangeFree;
        uint64_t transmissions, retries, countUnknown;
        uint64_t latencySamples, latencyTotalUS, latencyMaxUS, clockInvalid;
        uint64_t contextEnds, lastContextEndUS, lastContextEndSession, lastContextEndEpoch;
        uint64_t txsMalformed;
        // <1/5/10/25/50/100/500 ms, >=500 ms. Host submit-to-TX-free, NOT airtime.
        uint64_t latency[8];
    };
    struct Summary {
        uint64_t version, atUS, eventCount, ticketCount, txsCount, faults;
        uint64_t observerSession, observerEpoch, observerFlags, peerIncarnation[3];
        uint64_t outstanding, currentOutstanding, ambiguousOutstanding, prepared;
        uint64_t oldestAgeUS, oldestCurrentAgeUS, ageKnown; // bit 0 all, bit 1 current
        Counters counters;
    };
    struct Snapshot {
        Summary summary;
        Token oldest, oldestCurrent;
        Record events[capacity];
        TxsRecord txs[txsCapacity];
    };
    static_assert(sizeof(Token)==56 && offsetof(Token,token)==40, "HAL TX token v1");
    static_assert(sizeof(Record)==96 && offsetof(Record,flags)==92, "HAL TX event v1");
    static_assert(sizeof(TxsRecord)==88 && offsetof(TxsRecord,rate)==72, "HAL raw TXS v1");
    static_assert(sizeof(Counters)==34*8 && sizeof(Summary)==53*8, "HAL TX summary v1");

    Record events[capacity];
    TxsRecord txs[txsCapacity];
    Counters counters;
    uint64_t eventCount, ticketCount, txsCount, faults, peerIncarnation[3];
    uint64_t live[tokenCapacity];
    // Unsafe survives clearing the object's submission metadata. Reclaim
    // without TX-free is NOT a proven device-status fence. A later free of a
    // reused unsafe token is raw evidence only, never an ACK for its new owner.
    uint8_t tokenFlags[tokenCapacity];
    Snapshot published; // watchdog copy; never a kernel-stack-sized temporary

    void add(uint64_t &n,uint64_t amount=1) {
        if(UINT64_MAX-n<amount) { n=UINT64_MAX; faults|=CounterOverflow; }
        else n+=amount;
    }
    uint64_t serial(uint64_t &n,uint64_t failure) {
        if(n==UINT64_MAX) { faults|=failure; return 0; }
        return ++n;
    }
    void taint(unsigned token) {
        if(token>=tokenCapacity || (tokenFlags[token]&Unsafe)) return;
        tokenFlags[token]|=Unsafe; add(counters.tokenTaints);
    }
    // Called only from the actual HAL token's lifecycle, never from a saved
    // diagnostic ticket. No AWDL identity is required for these witnesses.
    void poolSubmitted(unsigned token) {
        if(token>=tokenCapacity) { faults|=MembershipLost; return; }
        if(tokenFlags[token]&PoolInFlight) taint(token);
        tokenFlags[token]|=PoolInFlight;
    }
    void poolCompleted(unsigned token) {
        if(token>=tokenCapacity) { faults|=MembershipLost; return; }
        if(!(tokenFlags[token]&PoolInFlight)) taint(token);
        tokenFlags[token]&=uint8_t(~PoolInFlight);
    }
    void poolReclaimed(unsigned token) {
        if(token>=tokenCapacity) { faults|=MembershipLost; return; }
        if(tokenFlags[token]&PoolInFlight) taint(token);
        tokenFlags[token]&=uint8_t(~PoolInFlight);
    }
    void peerChanged(unsigned peer) {
        if(peer>=3) return;
        if(peerIncarnation[peer]==UINT64_MAX) faults|=PeerExhausted;
        else ++peerIncarnation[peer];
    }
    void contextEnded(uint64_t session,uint32_t epoch,uint64_t at) {
        add(counters.contextEnds); counters.lastContextEndUS=at;
        counters.lastContextEndSession=session; counters.lastContextEndEpoch=epoch;
        // Keep tickets, guard ids, taints, rows and counters. DMA still owns
        // outstanding objects until its existing reclaim path says otherwise.
    }
    bool matches(const Token &t) const {
        return t.id && t.token<tokenCapacity && live[t.token]==t.id;
    }
    uint32_t contextFlags(const Token &t,const Observer &o) const {
        uint32_t flags=0;
        if(!t.session || t.session!=o.session) flags|=SessionChanged;
        if(t.epoch!=o.epoch) flags|=EpochChanged;
        if(!t.incarnation || t.incarnation!=o.incarnation) flags|=PeerChanged;
        const uint8_t required=Enabled|PeerPresent|Running;
        if((o.flags&required)!=required || (o.flags&(Stopping|Resetting|Detaching))) flags|=ContextInactive;
        if(faults || !(t.flags&ContextKnown)) flags|=TrackingUnavailable;
        if((t.flags&AmbiguousToken) || (t.token<tokenCapacity && (tokenFlags[t.token]&Unsafe))) flags|=TokenUnsafe;
        // A known submitting context can still be current while token-status
        // attribution is unsafe. Keep those two observations independent.
        if(!(flags&(SessionChanged|EpochChanged|PeerChanged|ContextInactive|TrackingUnavailable))) flags|=ContextCurrent;
        return flags;
    }
    Record *record(Kind kind,const Token &t,uint64_t at,uint32_t flags) {
        uint64_t id=serial(eventCount,EventExhausted);
        if(!id) return nullptr;
        auto &r=events[(id-1)%capacity]; r={};
        r.id=id; r.atUS=at; r.submitted=t; r.kind=kind; r.flags=flags;
        r.status=0xff; // zero is a REAL firmware success, never the default
        return &r;
    }
    void prepare(Token &t,const Token &candidate) {
        if(candidate.token>=tokenCapacity || t.id || live[candidate.token]) {
            add(counters.metadataRejected); add(counters.skipped); faults|=MembershipLost;
            return; // never clobber a newer token's guard
        }
        if(faults) { add(counters.skipped); return; }
        uint64_t id=serial(ticketCount,TicketExhausted);
        if(!id) { add(counters.skipped); return; }
        t=candidate; t.id=id; t.state=Prepared; t.submittedAtUS=0;
        if(tokenFlags[t.token]&Unsafe) t.flags|=AmbiguousToken;
        live[t.token]=id;
        add(counters.prepared);
    }
    void submit(Token &t,const Observer &o,uint64_t at) {
        if(!t.id) return;
        if(!matches(t) || t.state!=Prepared) { add(counters.metadataRejected); return; }
        t.state=Submitted; t.submittedAtUS=at;
        add(counters.submitted);
        record(Submit,t,at,contextFlags(t,o));
    }
    void retire(Token &t) {
        if(matches(t)) live[t.token]=0;
        t={};
    }
    void failed(Token &t,const Observer &o,uint64_t at,uint32_t error) {
        if(!t.id) return;
        if(!matches(t) || t.state!=Prepared) { add(counters.metadataRejected); return; }
        add(counters.submitFailed);
        if(auto *r=record(SubmitFailure,t,at,contextFlags(t,o))) r->hostError=error;
        retire(t);
    }
    void reclaim(Token &t,const Observer &o,uint64_t at) {
        if(!t.id) return;
        if(!matches(t)) { add(counters.metadataRejected); t={}; return; }
        if(t.state==Submitted) {
            add(counters.reclaimUnobserved); taint(t.token);
            record(Reclaim,t,at,contextFlags(t,o));
        } else {
            add(counters.submitFailed);
            // No caller-supplied errno: an abandoned, never-submitted ticket.
            if(auto *r=record(SubmitFailure,t,at,contextFlags(t,o))) r->hostError=UINT32_MAX;
        }
        retire(t);
    }
    void unassignedFree(const mtk_wlan_tx_freed &f,uint64_t at,uint32_t flags) {
        add(counters.freeUnattributed);
        Token unknown={}; unknown.token=f.msdu;
        if(auto *r=record(UnattributedFree,unknown,at,FreePresent|flags)) {
            r->raw=f.raw; r->count=f.count; r->status=f.status;
            if(f.count) r->flags|=CountKnown;
        }
    }
    void complete(Token &t,const mtk_wlan_tx_freed &f,const Observer &o,uint64_t at) {
        if(!t.id) return; // infrastructure, multicast, action and native UMI
        if(!matches(t) || t.token!=f.msdu || t.state!=Submitted) {
            add(counters.metadataRejected);
            unassignedFree(f,at,TrackingUnavailable);
            return;
        }
        if(faults || (tokenFlags[t.token]&Unsafe) || !(t.flags&ContextKnown)) {
            t.flags|=UnassignedFree;
            unassignedFree(f,at,TrackingUnavailable|((tokenFlags[t.token]&Unsafe) ? TokenUnsafe : 0));
            // The ordinary HAL put will record an UNOBSERVED reclaim of this
            // ticket. This raw token value cannot complete its new owner.
            return;
        }
        uint32_t flags=contextFlags(t,o)|FreePresent|Attributed;
        add(f.status ? counters.freeFailed : counters.freeAcked);
        add((flags&ContextCurrent) ? counters.freeCurrent : counters.freeNotCurrent);
        if(f.count) {
            flags|=CountKnown; add(counters.transmissions,f.count); add(counters.retries,f.count-1);
        } else add(counters.countUnknown);
        uint64_t latency=0;
        if(t.submittedAtUS && at>=t.submittedAtUS) {
            latency=at-t.submittedAtUS; flags|=LatencyKnown;
            add(counters.latencySamples); add(counters.latencyTotalUS,latency);
            if(latency>counters.latencyMaxUS) counters.latencyMaxUS=latency;
            const uint64_t limits[7]={1000,5000,10000,25000,50000,100000,500000};
            unsigned bin=0; while(bin<7 && latency>=limits[bin]) ++bin;
            add(counters.latency[bin]);
        } else add(counters.clockInvalid);
        if(auto *r=record(TxFree,t,at,flags)) {
            r->raw=f.raw; r->count=f.count; r->status=f.status; r->latencyUS=latency;
        }
        retire(t); // exactly once, even when the original submit row was evicted
    }
    void orphanFree(const mtk_wlan_tx_freed &f,uint64_t at) {
        if(f.msdu>=tokenCapacity) { add(counters.outOfRangeFree); return; }
        // An idle report is known unsafe-reuse evidence, even if the prior
        // occupant was not AWDL. This raw row deliberately has no ticket or
        // submitting context: pool-safety observations are not AWDL outcomes.
        add(counters.idleFree); taint(f.msdu);
        unassignedFree(f,at,TokenUnsafe|TrackingUnavailable);
    }
    void observeTxs(const mtk_wlan_tx_status &s,const Observer &o,uint64_t at) {
        if(s.wcid<15 || s.wcid>17 || s.pid!=125) return;
        uint64_t id=serial(txsCount,TxsExhausted); if(!id) return;
        auto &r=txs[(id-1)%txsCapacity]; r={};
        r.id=id; r.atUS=at; r.observerSession=o.session; r.observerIncarnation=o.incarnation;
        r.observerEpoch=o.epoch; r.observerFlags=o.flags; r.observerChannel=o.channel;
        for(unsigned i=0;i<8;++i) r.raw[i]=s.raw[i];
        r.errors=s.errors; r.rate=s.rate; r.wcid=s.wcid; r.sequence=s.sequence;
        r.pid=s.pid; r.format=s.format;
    }
    void snapshotBegin(const Observer &o,uint64_t at) {
        // published is heap-owned. Never value-initialize a whole Snapshot
        // temporary: it is larger than the kernel stack.
        auto &s=published.summary; s={}; published.oldest={}; published.oldestCurrent={};
        s.version=1; s.atUS=at; s.observerSession=o.session; s.observerEpoch=o.epoch; s.observerFlags=o.flags;
        for(unsigned i=0;i<3;++i) s.peerIncarnation[i]=peerIncarnation[i];
    }
    void snapshotToken(const Token &t,const Observer &o) {
        if(!t.id) return;
        if(!matches(t) || (t.state!=Submitted && t.state!=Prepared)) { faults|=SnapshotMismatch; return; }
        auto &s=published.summary;
        if(t.state==Prepared) { ++s.prepared; return; }
        ++s.outstanding;
        uint32_t flags=contextFlags(t,o);
        if(flags&(TrackingUnavailable|TokenUnsafe)) ++s.ambiguousOutstanding;
        if(!published.oldest.id || t.id<published.oldest.id) published.oldest=t;
        if(flags&ContextCurrent) {
            ++s.currentOutstanding;
            if(!published.oldestCurrent.id || t.id<published.oldestCurrent.id) published.oldestCurrent=t;
        }
    }
    void snapshotEnd() {
        auto &s=published.summary;
        // Derive gauges from live submitting objects, never by subtracting a
        // completion from a reset/overwritten ring entry. Max 512 objects.
        if(published.oldest.id && published.oldest.submittedAtUS && s.atUS>=published.oldest.submittedAtUS) {
            s.oldestAgeUS=s.atUS-published.oldest.submittedAtUS; s.ageKnown|=1;
        }
        if(published.oldestCurrent.id && published.oldestCurrent.submittedAtUS && s.atUS>=published.oldestCurrent.submittedAtUS) {
            s.oldestCurrentAgeUS=s.atUS-published.oldestCurrent.submittedAtUS; s.ageKnown|=2;
        }
        s.eventCount=eventCount; s.ticketCount=ticketCount; s.txsCount=txsCount; s.faults=faults; s.counters=counters;
        for(unsigned i=0;i<capacity;++i) published.events[i]=events[i];
        for(unsigned i=0;i<txsCapacity;++i) published.txs[i]=txs[i];
    }
};
static_assert(sizeof(HalTxTrace)<48*1024, "bounded heap HAL TX diagnostics");
}
#endif
