// SPDX-License-Identifier: GPL-2.0-or-later
// Wire protocol and scheduling primitives. No IOKit dependencies: the same
// implementation is exercised by the userspace sanitizer tests.
#ifndef MTK_AWDL_PROTOCOL_HPP
#define MTK_AWDL_PROTOCOL_HPP
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mtkawdl {
static const uint8_t bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
static const uint8_t snap[8] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0};
static const unsigned maxPeers = 16, slots = 16;
static const uint64_t peerLifetimeUs = 3000000;
static const uint32_t awUs = 16 * 1024;
// Largest supported EAW is 64*16TU (1048.576ms). Leave room for ROC
// request lead/guard while bounding continuous RF ownership below MT76's 5s.
static const uint32_t maxRocMs = 1100;
// mt7921_set_roc waits HZ (one second). A delayed firmware response must
// retain its request identity; this timeout does not grant any RF time.
static const uint32_t rocResponseTimeoutUs = 1000000;
static const uint32_t rocTokenQuarantineUs = rocResponseTimeoutUs + maxRocMs*1000;
inline uint16_t le16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t le32(const uint8_t *p) { return le16(p) | uint32_t(le16(p+2)) << 16; }
inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
inline void put16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
inline void put32(uint8_t *p, uint32_t v) { put16(p,v); put16(p+2,v>>16); }
inline void putBE32(uint8_t *p,uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
inline bool unicast(const uint8_t *p) {
    static const uint8_t zero[6] = {};
    return !(p[0]&1) && memcmp(p,zero,6);
}
inline bool zeroPadding(const uint8_t *p,size_t n) {
    if(n>=3) return false;
    for(size_t i=0;i<n;++i) if(p[i]) return false;
    return true;
}
struct Sequence {
    uint8_t channels[slots];
    uint8_t count, step;
};
// Power-of-two EAW sizes divide the 16-bit AW counter without a partial
// slot at rollover. Zero (unavailable) and other presence modes are not yet
// implemented by the scheduler and must not be advertised as supported.
inline bool validPresence(unsigned presence) {
    return presence && presence <= 64 && !(presence & (presence-1));
}
inline bool validSequence(const Sequence &s) {
    if (!s.count || s.count > slots || !validPresence(unsigned(s.step)+1)) return false;
    for (unsigned i=0; i<s.count; ++i) if (s.channels[i]>165) return false;
    return true;
}
inline bool parseSequence(const uint8_t *p, size_t n, Sequence &out) {
    if (n < 6 || p[0] >= slots || p[2] != 0 || !validPresence(unsigned(p[3])+1) ||
        le16(p+4)!=0xffff) return false;
    unsigned count = unsigned(p[0])+1;
    unsigned width = p[1]==0 ? 1 : (p[1]==1 || p[1]==3 ? 2 : 0);
    if (!width || count*width > n-6) return false;
    size_t used=6+count*width;
    // V9's captured iPhone PSF has two pad bytes in the embedded legacy
    // schedule and three in the standalone opclass schedule. Bound padding
    // separately from TLV framing; never accept nonzero extension bytes.
    if (n-used>3) return false;
    for(size_t i=used;i<n;++i) if(p[i]) return false;
    Sequence s = {}; s.count=count; s.step=p[3];
    for (unsigned i=0; i<count; ++i) {
        s.channels[i] = p[6+i*width+(p[1]==1 ? 1 : 0)];
        // 0 means an unavailable slot. A regulatory check is done by HAL.
        if (s.channels[i] > 165) return false;
        if (p[1]==3 && s.channels[i]) {
            uint8_t opclass=p[7+i*width];
            // A 6 GHz channel number must not be mistaken for a 2/5 GHz
            // channel. This transport currently supports only those bands.
            if (s.channels[i]<=14 ? (opclass<81 || opclass>84) :
                                   (opclass<115 || opclass>130)) return false;
        }
    }
    out=s; return true;
}
struct Advertisement {
    Sequence sequence;
    uint8_t master[6], sync[6];
    uint16_t period, counter, down;
    uint8_t presence;
    uint32_t metric, distance, masterCounter, selfCounter;
};
// Validate framing for native peer discovery without imposing the host
// scheduler's narrower election/sequence policy. The native peer manager owns
// semantic validation, including MIFs without a synchronization TLV.
inline bool validActionEnvelope(const uint8_t *f,size_t n) {
    static const uint8_t action[6]={127,0,0x17,0xf2,8,0x10};
    if(!f || n<40 || n>2048 || f[0]!=0xd0 || (f[1]&0xc7) ||
       (f[22]&15) || !unicast(f+10) || memcmp(f+16,bssid,6) ||
       memcmp(f+24,action,6) || (f[30]!=0 && f[30]!=3)) return false;
    for(size_t at=40;at<n;) {
        if(n-at<3) return zeroPadding(f+at,n-at);
        size_t len=le16(f+at+1); at+=3;
        if(len>n-at) return false;
        at+=len;
    }
    return n>40;
}
// Input is a complete 802.11 action frame without radiotap/FCS. Parse into a
// temporary value so a truncated last TLV cannot partially update live state.
inline bool parseAction(const uint8_t *f, size_t n, Advertisement &out) {
    static const uint8_t action[6]={127,0,0x17,0xf2,8,0x10};
    if (n<40 || f[0]!=0xd0 || (f[1]&0xc7) || (f[22]&15) || !unicast(f+10) ||
        memcmp(f+16,bssid,6) || memcmp(f+24,action,6) ||
        (f[30]!=0 && f[30]!=3)) return false;
    Advertisement a={}; bool sync=false, election=false, separateSequence=false;
    Sequence extra={}; uint8_t electionMaster[6]={};
    uint8_t embeddedEncoding=0, separateEncoding=0;
    for (size_t at=40; at<n;) {
        if (n-at<3) { if(zeroPadding(f+at,n-at)) break; return false; }
        unsigned type=f[at]; size_t len=le16(f+at+1); at+=3;
        if (len>n-at) return false;
        const uint8_t *p=f+at;
        if (type==4) {
            if (sync || len<39) return false;
            a.period=le16(p+5); a.down=le16(p+1); a.presence=p[27];
            a.counter=le16(p+29); memcpy(a.master,p+21,6);
            if (a.period!=16 || !validPresence(a.presence) || !a.down ||
                a.down>unsigned(a.presence)*a.period || !unicast(a.master) ||
                !parseSequence(p+33,len-33,a.sequence) || a.sequence.step+1!=a.presence) return false;
            // The down-counter and AW counter must describe the same EAW.
            if (a.counter%a.presence != (unsigned(a.presence)*a.period-a.down)/a.period) return false;
            embeddedEncoding=p[34]; sync=true;
        } else if (type==24) {
            if (election || len!=40 || !unicast(p) || !unicast(p+6)) return false;
            memcpy(a.sync,p+6,6); a.metric=le32(p+20); a.distance=le32(p+16);
            memcpy(electionMaster,p,6); a.masterCounter=le32(p+12); a.selfCounter=le32(p+36);
            if (a.distance>10) return false;
            election=true;
        } else if (type==18) {
            if (separateSequence || !parseSequence(p,len,extra)) return false;
            separateEncoding=p[1]; separateSequence=true;
        }
        at+=len;
    }
    if (!sync) return false;
    // Sync parameters describe the transmitting peer's clock. Election v2
    // names the elected root, which can differ during a tree transition.
    // iOS 26 advertises this while following us. Keep its usable channel
    // schedule and use the election TLV for root/loop decisions, as OWL does.
    if (election) memcpy(a.master,electionMaster,6);
    if (separateSequence) {
        if(extra.count!=a.sequence.count || extra.step!=a.sequence.step) return false;
        // The captured PSF advertises legacy channel 151 in sync params but
        // channel 149/opclass 128 in TLV 18. OWL consumes TLV 18 as the peer
        // schedule. Do not invent a conversion of the legacy flags: prefer
        // the separately validated explicit opclass schedule in this case.
        if(embeddedEncoding!=1 || separateEncoding!=3) {
            if(memcmp(extra.channels,a.sequence.channels,extra.count)) return false;
        }
        a.sequence=extra;
    }
    if (!election) memcpy(a.sync,f+10,6);
    out=a; return true;
}
struct Peer {
    uint8_t mac[6]; Advertisement advertisement;
    uint64_t seen, epoch;
    uint16_t baseCounter, lastData;
    uint64_t replay;
    bool used, hasData;
    uint8_t channel(uint64_t now) const {
        const Sequence &s=advertisement.sequence;
        if (!used || now<epoch || now<seen || now-seen>peerLifetimeUs || !validSequence(s)) return 0;
        uint16_t count=baseCounter+uint16_t((now-epoch)/awUs);
        return s.channels[(count/(unsigned(s.step)+1))%s.count];
    }
};
// Native cache/traffic registration is authoritative for data membership;
// synchronization advertisements have a separate, short-lived radio schedule.
// Keeping replay state here prevents schedule expiry from reopening old data.
struct DataPeers {
    struct Entry {
        Peer replayState;
        uint64_t seen;
        bool cached, traffic;
    } entries[maxPeers]={};
    void reset() { memset(entries,0,sizeof(entries)); }
    bool live(const Entry &e,uint64_t now) const {
        return e.replayState.used && (e.cached || e.traffic ||
            (now>=e.seen && now-e.seen<=peerLifetimeUs));
    }
    int find(const uint8_t *mac,uint64_t now) const {
        for(unsigned i=0;i<maxPeers;++i)
            if(live(entries[i],now) && !memcmp(entries[i].replayState.mac,mac,6)) return i;
        return -1;
    }
    int observe(const uint8_t *mac,uint64_t now) {
        if(!unicast(mac)) return -1;
        int i=find(mac,now);
        if(i<0) for(unsigned j=0;j<maxPeers;++j) if(!live(entries[j],now)) {
            i=j; memset(&entries[i],0,sizeof(entries[i]));
            memcpy(entries[i].replayState.mac,mac,6); entries[i].replayState.used=true; break;
        }
        if(i>=0) entries[i].seen=now;
        return i;
    }
    bool canObserve(const uint8_t *mac,uint64_t now) const {
        if(!unicast(mac)) return false;
        if(find(mac,now)>=0) return true;
        for(unsigned i=0;i<maxPeers;++i) if(!live(entries[i],now)) return true;
        return false;
    }
    bool setCached(const uint8_t *mac,bool value,uint64_t now) {
        int i=value ? observe(mac,now) : find(mac,now);
        if(i<0) return !value;
        entries[i].cached=value;
        // Native cache removal revokes authorization and replay even if the
        // last advertisement is recent. A traffic registration may still own it.
        if(!value && !entries[i].traffic) memset(&entries[i],0,sizeof(entries[i]));
        return true;
    }
    bool setTraffic(const uint8_t *mac,bool value,uint64_t now) {
        int i=value ? observe(mac,now) : find(mac,now);
        if(i<0) return !value;
        entries[i].traffic=value;
        if(!value && !entries[i].cached) memset(&entries[i],0,sizeof(entries[i]));
        return true;
    }
};
struct State {
    uint8_t mac[6], master[6], sync[6], forcedSync[6];
    Sequence sequence, ownSequence; Peer peers[maxPeers];
    uint64_t epoch; uint16_t baseCounter, dataCounter, frameCounter, psfPeriod;
    uint32_t metric, masterMetric, distance, masterCounter, selfCounter; bool synchronized;
    uint64_t electionEpoch;
    uint8_t presence, ownPresence, syncRole, minExtension[4];
    bool nativeElectionManaged, nativeParentSelected;
    uint8_t nativeParent[6];
    uint64_t clockSampleAt, clockCorrections, clockRejected;
    // Software equivalent of a firmware AW clock: peer observations remain
    // fresh, but phase discipline waits until the promised radio AW ends.
    // Both event production and synchronization TLVs use epoch/baseCounter.
    uint64_t clockHoldUntil, clockDeferredAt, clockDeferred;
    void holdClock(uint64_t until) { clockHoldUntil=until; }
    // The selected clock survives opmode changes and temporary peer expiry.
    // Effective radio controls are a transaction applied between AW events.
    bool nativeClockSelected, nativeTimelineStarted, controlsHeld, controlsDirty;
    bool nativeClockHoldover, nativeClockValidated, nativeClockAcquisitionPending;
    bool clockSampleRejected;
    uint64_t nativeHoldoverTransitions, nativeClockAcquisitions;
    uint8_t clockParent[6], clockRoot[6];
    uint8_t acquisitionParent[6], acquisitionRoot[6];
    bool acquisitionRootKnown;
    uint64_t controlsDeferred;
    struct Controls {
        Sequence sequence;
        uint8_t presence, role, forced[6], parent[6];
        bool parentSelected;
    } pendingControls;
    void holdControls() {
        if(controlsHeld) return;
        pendingControls.sequence=ownSequence; pendingControls.presence=ownPresence;
        pendingControls.role=syncRole; memcpy(pendingControls.forced,forcedSync,6);
        memcpy(pendingControls.parent,nativeParent,6);
        pendingControls.parentSelected=nativeParentSelected;
        controlsHeld=true; controlsDirty=false;
    }
    void discardControls() { controlsHeld=controlsDirty=false; }
    uint8_t requestedRole() const { return controlsHeld ? pendingControls.role : syncRole; }
    bool requestedParentIs(const uint8_t *address) const {
        return controlsHeld ? pendingControls.parentSelected &&
            !memcmp(pendingControls.parent,address,6) : nativeParentSelected &&
            !memcmp(nativeParent,address,6);
    }
    void selectNativeParent(const uint8_t *address,const uint8_t *root=nullptr,
                            bool acquire=false) {
        nativeElectionManaged=true;
        // Only an explicit native selection authorizes acquiring another AW
        // namespace. Ordinary RX from an already selected peer remains a
        // bounded drift observation, even if it carries a discontinuous clock.
        bool keepRoot=!root && nativeClockAcquisitionPending &&
            !memcmp(acquisitionParent,address,6);
        nativeClockAcquisitionPending=acquire || !nativeClockSelected || memcmp(clockParent,address,6) ||
            (root && memcmp(clockRoot,root,6));
        if(keepRoot) nativeClockAcquisitionPending=true;
        memcpy(acquisitionParent,address,6);
        if(!keepRoot) acquisitionRootKnown=root!=nullptr;
        if(root) memcpy(acquisitionRoot,root,6);
        if(controlsHeld) {
            if(!pendingControls.parentSelected || memcmp(pendingControls.parent,address,6)) {
                memcpy(pendingControls.parent,address,6); pendingControls.parentSelected=true;
                controlsDirty=true; ++controlsDeferred;
            }
        } else {
            if(memcmp(nativeParent,address,6)) nativeClockHoldover=false;
            memcpy(nativeParent,address,6); nativeParentSelected=true;
        }
    }
    void clearNativeParent() {
        nativeClockAcquisitionPending=false; acquisitionRootKnown=false;
        if(controlsHeld) {
            if(pendingControls.parentSelected) {
                pendingControls.parentSelected=false; controlsDirty=true; ++controlsDeferred;
            }
            return;
        }
        nativeParentSelected=nativeClockSelected=nativeClockValidated=false;
        nativeClockHoldover=clockSampleRejected=synchronized=false;
        memset(nativeParent,0,6); memset(clockParent,0,6); memset(clockRoot,0,6);
        memcpy(master,mac,6); memcpy(sync,mac,6);
        sequence=ownSequence; presence=ownPresence;
        masterMetric=metric; masterCounter=selfCounter; distance=0;
        // Keep the physical AW counter/phase through local-master promotion.
    }
    bool releaseControls(uint64_t now) {
        if(!controlsHeld) return false;
        bool changed=controlsDirty; controlsHeld=controlsDirty=false;
        if(changed) {
            setSyncRole(pendingControls.role,pendingControls.forced);
            setSequence(pendingControls.sequence,now);
            memcpy(nativeParent,pendingControls.parent,6);
            nativeParentSelected=pendingControls.parentSelected;
            if(nativeElectionManaged && !nativeParentSelected) clearNativeParent();
        }
        elect(now); return changed;
    }
    void init(const uint8_t *address, uint64_t now) {
        memset(this,0,sizeof(*this)); memcpy(mac,address,6);
        memcpy(master,address,6); memcpy(sync,address,6);
        epoch=electionEpoch=now; metric=masterMetric=1; presence=ownPresence=4; syncRole=1; psfPeriod=110;
        sequence.count=slots; sequence.step=3;
        for (unsigned i=0;i<slots;++i) sequence.channels[i]=6;
        ownSequence=sequence;
        for (unsigned i=0;i<4;++i) minExtension[i]=3;
    }
    uint16_t counter(uint64_t now) const {
        return baseCounter+uint16_t((now>=epoch ? now-epoch : 0)/awUs);
    }
    uint32_t untilNext(uint64_t now) const {
        if (!validPresence(presence)) return 0;
        uint32_t period=unsigned(presence)*awUs;
        uint64_t elapsed=now>=epoch ? now-epoch : 0;
        return period-uint32_t((uint64_t(baseCounter%presence)*awUs+elapsed%period)%period);
    }
    uint8_t channel(uint64_t now) const {
        if (!radioScheduleReady()) return 0;
        return validSequence(sequence) ? sequence.channels[(counter(now)/(unsigned(sequence.step)+1))%sequence.count] : 0;
    }
    bool radioScheduleReady() const {
        return syncRole!=2 || synchronized || (nativeClockHoldover && nativeElectionManaged &&
            nativeClockSelected && nativeClockValidated && nativeParentSelected &&
            !memcmp(clockParent,nativeParent,6));
    }
    bool setSyncRole(unsigned role,const uint8_t *address) {
        if (role<1 || role>3 || !address || !unicast(address) ||
            (role==2 ? !memcmp(address,mac,6) :
             role==3 ? memcmp(address,mac,6) : (!nativeElectionManaged && memcmp(address,mac,6)))) return false;
        if(controlsHeld) {
            if(pendingControls.role!=role || memcmp(pendingControls.forced,address,6)) {
                pendingControls.role=role; memcpy(pendingControls.forced,address,6);
                controlsDirty=true; ++controlsDeferred;
            }
            return true;
        }
        if (syncRole==role && !memcmp(forcedSync,address,6)) return true;
        holdClock(0);
        syncRole=role; memcpy(forcedSync,address,6);
        synchronized=false; nativeClockHoldover=false; sequence=ownSequence; presence=ownPresence;
        memcpy(master,mac,6); memcpy(sync,mac,6); masterMetric=metric;
        masterCounter=selfCounter; distance=0; return true;
    }
    bool setPresence(uint8_t value,uint64_t now) {
        if (!validPresence(value)) return false;
        if(controlsHeld) {
            if(pendingControls.presence!=value) {
                pendingControls.presence=value; pendingControls.sequence.step=value-1;
                controlsDirty=true; ++controlsDeferred;
            }
            return true;
        }
        ownPresence=value; ownSequence.step=value-1;
        if (!synchronized) {
            // Keep the current AW counter and sub-AW phase through a change
            // of EAW duration. untilNext() includes the base-counter phase.
            uint64_t elapsed=now>=epoch ? now-epoch : 0;
            baseCounter=counter(now); epoch=now-elapsed%awUs;
            presence=value; sequence.step=value-1;
        }
        return true;
    }
    bool setSequence(const Sequence &value,uint64_t now) {
        if(!validSequence(value) || !validPresence(unsigned(value.step)+1)) return false;
        if(controlsHeld) {
            if(memcmp(&pendingControls.sequence,&value,sizeof(value))) {
                pendingControls.sequence=value; pendingControls.presence=value.step+1;
                controlsDirty=true; ++controlsDeferred;
            }
        } else {
            ownSequence=value; setPresence(value.step+1,now);
            sequence=value; presence=value.step+1;
        }
        return true;
    }
    void setSocialChannel(unsigned index,uint8_t channel,uint64_t now) {
        Sequence desired=controlsHeld ? pendingControls.sequence : ownSequence;
        if(index>=desired.count) return;
        desired.channels[index]=channel; setSequence(desired,now);
    }
    int find(const uint8_t *address) const {
        for(unsigned i=0;i<maxPeers;++i) if(peers[i].used && !memcmp(peers[i].mac,address,6)) return i;
        return -1;
    }
    void elect(uint64_t now) {
        // Peer RX may refresh observations, but cannot alter an open AW's
        // role, parent, channel or clock. Commit the latest controls at end.
        if(controlsHeld) return;
        // Only a top master advances its own election counter. Followers
        // advertise the counter learned from their selected master.
        if(now<electionEpoch || synchronized || nativeClockHoldover) electionEpoch=now;
        else {
            uint64_t intervals=(now-electionEpoch)/3140000;
            selfCounter+=uint32_t(intervals); electionEpoch+=intervals*3140000;
        }
        // A native interface receives its election result through advertisers.
        // Do not adopt an independently ranked peer before that transaction.
        if(nativeElectionManaged && !nativeParentSelected) return;
        int best=-1;
        for(unsigned i=0;i<maxPeers;++i) {
            Peer &p=peers[i];
            if(p.used && (now<p.seen || now-p.seen>peerLifetimeUs)) p.used=false;
            if(!p.used || !memcmp(p.advertisement.master,mac,6)) continue;
            const Advertisement &a=p.advertisement;
            if(a.distance>=10 || !memcmp(a.sync,mac,6)) continue;
            if(syncRole==3) continue;
            if(nativeElectionManaged) {
                if(!memcmp(p.mac,nativeParent,6)) best=i;
                continue;
            }
            if(syncRole==2) { if(!memcmp(p.mac,forcedSync,6)) best=i; continue; }
            // omdrop-owl's iOS 26 captures show metric precedes counter.
            int32_t rank=int32_t(a.masterCounter-selfCounter);
            if(a.metric<metric || (a.metric==metric && (rank<0 ||
                (!rank && (a.distance || memcmp(p.mac,mac,6)<=0))))) continue;
            if(best<0) { best=i; continue; }
            const Advertisement &b=peers[best].advertisement;
            rank=int32_t(a.masterCounter-b.masterCounter);
            if(a.metric>b.metric || (a.metric==b.metric && (rank>0 || (!rank &&
                (a.distance<b.distance || (a.distance==b.distance && memcmp(p.mac,peers[best].mac,6)>0)))))) best=i;
        }
        if(best<0) {
            // Native selected this firmware-clock equivalent and still owns
            // the configured sequence. A short RX cache expiry must not stop
            // all radio reservations or silently promote us to another tree.
            // Holdover retains the last clock; it does not claim peer RF sync.
            if(nativeElectionManaged && nativeClockSelected && nativeClockValidated &&
               nativeParentSelected && syncRole!=3 &&
               !memcmp(clockParent,nativeParent,6)) {
                if(!nativeClockHoldover) ++nativeHoldoverTransitions;
                nativeClockHoldover=true; synchronized=false;
                sequence=ownSequence; presence=ownPresence;
                memcpy(master,clockRoot,6); memcpy(sync,clockParent,6);
                return;
            }
            nativeClockHoldover=false;
            // Preserve the current AW clock when an elected master expires,
            // but restore our own advertised channel sequence.
            if(synchronized) { sequence=ownSequence; presence=ownPresence; holdClock(0); }
            synchronized=false; masterMetric=metric; masterCounter=selfCounter; distance=0;
            memcpy(master,mac,6); memcpy(sync,mac,6); return;
        }
        const Peer &p=peers[best];
        bool sameClock=nativeElectionManaged ? nativeClockSelected &&
            !memcmp(clockParent,p.mac,6) && !memcmp(clockRoot,p.advertisement.master,6) :
            synchronized && !memcmp(sync,p.mac,6) && !memcmp(master,p.advertisement.master,6);
        // A native selection authorizes this parent/root only. RX arriving
        // while the old AW closes cannot substitute an unrelated clock tree.
        if(nativeElectionManaged && nativeClockAcquisitionPending &&
           (memcmp(acquisitionParent,p.mac,6) ||
            (acquisitionRootKnown && memcmp(acquisitionRoot,p.advertisement.master,6)))) return;
        bool adopt=!nativeElectionManaged || nativeClockAcquisitionPending ||
            (!nativeClockSelected && !nativeTimelineStarted);
        if(nativeElectionManaged && !sameClock && !adopt) {
            // This RX belongs to another clock tree; native has not selected
            // it. Do not overwrite the committed identity before rejecting it,
            // or a subsequent explicit selection looks like a duplicate and
            // leaves validation permanently false.
            if(p.seen>clockSampleAt) { clockSampleAt=p.seen; ++clockRejected; }
            clockSampleRejected=true; synchronized=false;
            if(!nativeClockHoldover) ++nativeHoldoverTransitions;
            nativeClockHoldover=true;
            return;
        }
        if(adopt && nativeElectionManaged && now<clockHoldUntil) {
            if(p.seen>clockDeferredAt) { clockDeferredAt=p.seen; ++clockDeferred; }
            return;
        }
        // A timer rechecking a rejected observation is not a new aligned RX.
        // Retain the acquired clock as holdover until a fresh accepted sample.
        nativeClockHoldover=clockSampleRejected; synchronized=!clockSampleRejected;
        memcpy(master,p.advertisement.master,6);
        memcpy(sync,p.mac,6);
        // Native programs our radio sequence independently of its selected
        // synchronization parent. A peer supplies the clock, not an implicit
        // replacement for the sequence accepted by the native setter.
        sequence=nativeElectionManaged ? ownSequence : p.advertisement.sequence;
        presence=nativeElectionManaged ? ownPresence : p.advertisement.presence;
        masterMetric=p.advertisement.metric; masterCounter=p.advertisement.masterCounter;
        distance=p.advertisement.distance+1;
        if(nativeElectionManaged && !sameClock) {
            nativeClockSelected=true; nativeClockValidated=false; memcpy(clockParent,p.mac,6);
            memcpy(clockRoot,p.advertisement.master,6);
        }
        if(adopt) {
            holdClock(0);
            // Native programChipWithNewMaster supplies a real selected-peer
            // counter/down/timestamp. Acquire that shared event/wire clock at
            // a safe edge, including when local warmup AWs already ran.
            epoch=p.epoch; baseCounter=p.baseCounter; clockSampleAt=p.seen;
            if(nativeElectionManaged) {
                nativeClockValidated=true; nativeClockAcquisitionPending=false;
                clockSampleRejected=false; nativeClockHoldover=false; synchronized=true;
                ++nativeClockAcquisitions;
            }
        } else if(!sameClock || p.seen>clockSampleAt) {
            uint64_t local=now>=epoch ? now-epoch : 0;
            uint64_t remote=now>=p.epoch ? now-p.epoch : 0;
            int32_t delta=int16_t(uint16_t(p.baseCounter+remote/awUs)-counter(now))*int32_t(awUs)+
                int32_t(remote%awUs)-int32_t(local%awUs);
            // Native permits 5 TU between consecutive matching starts. Slew
            // at most half a TU per fresh PSF; a discontinuous peer clock
            // needs a new selection instead of silently skipping real AWs.
            if(delta>5*1024 || delta<-5*1024) {
                clockSampleAt=p.seen; ++clockRejected; clockSampleRejected=true;
                synchronized=false;
                if(!nativeClockHoldover) ++nativeHoldoverTransitions;
                nativeClockHoldover=true; return;
            }
            if(nativeElectionManaged) {
                nativeClockValidated=true; clockSampleRejected=false;
                nativeClockHoldover=false; synchronized=true;
            }
            if(now<clockHoldUntil) {
                if(p.seen>clockDeferredAt) { clockDeferredAt=p.seen; ++clockDeferred; }
                // Do not consume the sample. At the safe edge elect() uses
                // the newest peer observation, never a sum of old slews.
                return;
            }
            clockSampleAt=p.seen;
            int32_t correction=delta>512 ? 512 : delta<-512 ? -512 : delta;
            if(correction) {
                uint16_t base=counter(now);
                int32_t phase=int32_t(local%awUs)+correction;
                if(phase<0) { --base; phase+=awUs; }
                else if(phase>=int32_t(awUs)) { ++base; phase-=awUs; }
                if(now>=uint32_t(phase)) {
                    epoch=now-uint32_t(phase); baseCounter=base; ++clockCorrections;
                }
            }
        }
    }
    int receiveAction(const uint8_t *f,size_t n,uint64_t now) {
        Advertisement a;
        if(!parseAction(f,n,a) || !memcmp(f+10,mac,6)) return -1;
        uint64_t elapsed=uint64_t(unsigned(a.presence)*a.period-a.down)*1024;
        if(now<elapsed) return -1;
        int idx=find(f+10);
        if(idx<0) for(unsigned i=0;i<maxPeers;++i)
            if(!peers[i].used || now<peers[i].seen || now-peers[i].seen>peerLifetimeUs) { idx=i; break; }
        if(idx<0) return -1;
        Peer &p=peers[idx];
        if(!p.used || now<p.seen || now-p.seen>peerLifetimeUs || memcmp(p.mac,f+10,6)) memset(&p,0,sizeof(p));
        memcpy(p.mac,f+10,6); p.used=true; p.seen=now; p.advertisement=a;
        p.epoch=now-elapsed; p.baseCounter=a.counter-a.counter%a.presence;
        elect(now); return idx;
    }
    bool acceptData(Peer &p,uint16_t seq) {
        if(!p.hasData) { p.hasData=true; p.lastData=seq; p.replay=1; return true; }
        int16_t delta=int16_t(seq-p.lastData);
        if(delta>0) { p.replay=delta>=64 ? 1 : (p.replay<<delta)|1; p.lastData=seq; return true; }
        unsigned back=uint16_t(p.lastData-seq);
        if(back>=64 || (p.replay & (uint64_t(1)<<back))) return false;
        p.replay|=uint64_t(1)<<back; return true;
    }
};
inline void writeSequence(uint8_t *p,const Sequence &s) {
    p[0]=s.count-1; p[1]=p[2]=0; p[3]=s.step; put16(p+4,0xffff);
    memcpy(p+6,s.channels,s.count);
}
inline void writeSync(State &s,uint8_t *p,uint64_t now) {
    p[0]=p[3]=s.channel(now); put16(p+1,(s.untilNext(now)+1023)/1024);
    put16(p+5,16); put16(p+11,16); put16(p+13,16);
    uint32_t remaining=s.untilNext(now), duration=unsigned(s.presence)*awUs;
    uint32_t elapsed=duration-remaining;
    put16(p+15,elapsed<awUs ? (awUs-elapsed)/1024 : 0);
    // No advertised extension may cross the next scheduled channel change.
    for(unsigned i=0;i<4;++i) p[17+i]=s.minExtension[i]<s.presence ? s.minExtension[i] : s.presence-1;
    memcpy(p+21,s.master,6); p[27]=s.presence;
    put16(p+29,s.counter(now)); put16(p+31,s.counter(now));
    writeSequence(p+33,s.sequence);
}
inline void writeElection(State &s,uint8_t *p) {
    memcpy(p,s.master,6); memcpy(p+6,s.sync,6); put32(p+12,s.masterCounter);
    put32(p+16,s.distance); put32(p+20,s.masterMetric); put32(p+24,s.metric);
    put32(p+36,s.selfCounter);
}
inline size_t buildAction(State &s,uint8_t *f,size_t capacity,uint64_t now) {
    const size_t syncLength=39+s.sequence.count+2;
    const size_t size=40+3+syncLength+3+40+3+15+3+2+3+9;
    if(capacity<size || !validSequence(s.sequence) || !validPresence(s.presence) ||
        s.sequence.step+1!=s.presence || !s.radioScheduleReady()) return 0;
    memset(f,0,size); f[0]=0xd0; memset(f+4,0xff,6);
    memcpy(f+10,s.mac,6); memcpy(f+16,bssid,6); put16(f+22,s.frameCounter++<<4);
    f[24]=127; f[26]=0x17; f[27]=0xf2; f[28]=8; f[29]=0x10; f[30]=3;
    put32(f+32,uint32_t(now)); put32(f+36,uint32_t(now));
    size_t at=40; f[at]=4; put16(f+at+1,syncLength); uint8_t *p=f+at+3;
    writeSync(s,p,now); put16(p+7,s.psfPeriod); put16(p+9,0x1800);
    at+=3+syncLength; f[at]=24; put16(f+at+1,40); p=f+at+3;
    writeElection(s,p); at+=43;
    f[at]=12; put16(f+at+1,15); p=f+at+3; put16(p,0x0304);
    p[2]='X'; p[3]='0'; put16(p+5,7); memcpy(p+7,s.mac,6); at+=18;
    f[at]=21; put16(f+at+1,2); f[at+3]=0x30; f[at+4]=1; at+=5;
    f[at]=6; put16(f+at+1,9); return size;
}
// Tahoe's native SYNC_FRAME_TEMPLATE contains supplemental TLVs, not the
// vendor action header or synchronization TLV. Preserve native service,
// election and data-path advertisements alongside our scheduler's sync TLV.
inline bool validTemplateTLVs(const uint8_t *p,size_t n) {
    if(!p || !n || n>2048) return false;
    bool election=false;
    for(size_t at=0;at<n;) {
        if(n-at<3) return false;
        unsigned type=p[at]; size_t length=le16(p+at+1); at+=3;
        if(length>n-at) return false;
        if(type==4 || type==18) return false; // scheduler owns timing/sequence
        if(type==24) { if(election || length!=40) return false; election=true; }
        if(type==5 && length!=21) return false;
        if(type==20 && length%6) return false;
        at+=length;
    }
    return true;
}
inline size_t buildActionWithTemplate(State &s,uint8_t *f,size_t capacity,
                                     uint64_t now,const uint8_t *p,size_t n) {
    if(!validTemplateTLVs(p,n)) return 0;
    // Validate capacity before buildAction advances counters or writes output.
    const size_t prefix=43+39+s.sequence.count+2;
    if(capacity<prefix || n>capacity-prefix) return 0;
    uint8_t base[256];
    size_t baseLength=buildAction(s,base,sizeof(base),now);
    if(!baseLength || prefix>baseLength) return 0;
    memcpy(f,base,prefix);
    // The host owns RF timing/election. A native supplement may name an old
    // master; keep its non-election fields but align the election with sync.
    size_t dst=prefix;
    for(size_t at=0;at<n;) {
        unsigned type=p[at]; size_t len=le16(p+at+1);
        if(type!=20) { // stale native sync-tree path cannot describe our clock
            memcpy(f+dst,p+at,3+len);
            if(type==24) writeElection(s,f+dst+3);
            if(type==5) {
                uint8_t *v=f+dst+3; v[3]=uint8_t(s.distance);
                memcpy(v+5,s.master,6); put32(v+11,s.masterMetric); put32(v+15,s.metric);
            }
            dst+=3+len;
        }
        at+=3+len;
    }
    return dst;
}
// 802.11 Block Ack action frames (category 3) addressed to this AWDL MAC.
// Peers request RX aggregation with ADDBA; this transport has no firmware
// BA session or reorder buffer, so it declines and the peer sends ordinary
// acknowledged MPDUs.
enum : uint8_t { blockAckCategory=3, addbaRequest=0, addbaResponse=1, delba=2 };
enum : uint16_t { statusRequestDeclined=37 };
inline bool blockAckAction(const uint8_t *f,size_t n,const uint8_t *mac) {
    return f && n>=26 && f[0]==0xd0 && !(f[1]&0xc7) && !memcmp(f+4,mac,6) &&
        unicast(f+10) && memcmp(f+10,mac,6) && !memcmp(f+16,bssid,6) &&
        f[24]==blockAckCategory;
}
// ADDBA Request body: token, BA parameter set, timeout, starting sequence.
// The response echoes token/parameters/timeout with the declined status.
inline size_t buildAddbaDecline(State &s,const uint8_t *request,size_t n,uint8_t *f,size_t capacity) {
    const size_t size=24+9;
    if(capacity<size || !blockAckAction(request,n,s.mac) || request[25]!=addbaRequest || n<24+9) return 0;
    memset(f,0,size); f[0]=0xd0;
    memcpy(f+4,request+10,6); memcpy(f+10,s.mac,6); memcpy(f+16,bssid,6);
    put16(f+22,s.frameCounter++<<4);
    f[24]=blockAckCategory; f[25]=addbaResponse; f[26]=request[26];
    put16(f+27,statusRequestDeclined);
    memcpy(f+29,request+27,2); memcpy(f+31,request+29,2);
    return size;
}
// Refresh a native action-frame template without guessing legacy flag or
// operating-class values. Channel sequences are normalized to supported
// simple encoding, and the remaining TLVs are shifted down in the buffer.
// Validation is a separate pass: rejection leaves the template and State
// untouched. A template with insufficient room for our sequence is rejected.
inline size_t refreshAction(State &s,uint8_t *f,size_t n,uint64_t now) {
    static const uint8_t action[6]={127,0,0x17,0xf2,8,0x10};
    if(n<40 || f[0]!=0xd0 || (f[1]&0xc7) || memcmp(f+24,action,6) ||
        (f[30]!=0 && f[30]!=3) || !validSequence(s.sequence) ||
        s.sequence.step+1!=s.presence || !s.radioScheduleReady()) return 0;
    bool hasSync=false,hasElection=false,hasSequence=false,hasLegacyElection=false;
    for(size_t at=40;at<n;) {
        if(n-at<3) return 0;
        unsigned type=f[at]; size_t len=le16(f+at+1); at+=3;
        if(len>n-at) return 0;
        Sequence old={};
        if(type==4) {
            if(hasSync || len<39 || !parseSequence(f+at+33,len-33,old) ||
                len<39+s.sequence.count) return 0;
            hasSync=true;
        } else if(type==18) {
            if(hasSequence || !parseSequence(f+at,len,old) || len<6+s.sequence.count) return 0;
            hasSequence=true;
        } else if(type==24) {
            if(hasElection || len!=40) return 0;
            hasElection=true;
        } else if(type==5) {
            if(hasLegacyElection || len!=21) return 0;
            hasLegacyElection=true;
        } else if(type==20 && len%6) return 0;
        at+=len;
    }
    if(!hasSync) return 0;
    size_t dst=40;
    for(size_t src=40;src<n;) {
        unsigned type=f[src]; size_t old=le16(f+src+1),len=old;
        if(type==4) len=39+s.sequence.count;
        if(type==18) len=6+s.sequence.count;
        // The template's election tree may belong to a previous master. The
        // optional tree cannot be rebuilt without the full advertised path.
        if(type==20) { src+=3+old; continue; }
        memmove(f+dst,f+src,3+len); put16(f+dst+1,len);
        uint8_t *p=f+dst+3;
        if(type==4) writeSync(s,p,now);
        else if(type==18) writeSequence(p,s.sequence);
        else if(type==24) writeElection(s,p);
        else if(type==5) {
            p[3]=uint8_t(s.distance); memcpy(p+5,s.master,6);
            put32(p+11,s.masterMetric); put32(p+15,s.metric);
        }
        src+=3+old; dst+=3+len;
    }
    memcpy(f+10,s.mac,6); memcpy(f+16,bssid,6);
    put16(f+22,s.frameCounter++<<4);
    put32(f+32,uint32_t(now)); put32(f+36,uint32_t(now));
    return dst;
}
inline bool validIPv6(const uint8_t *p,size_t n) {
    if(n<40 || (p[0]>>4)!=6) return false;
    size_t payload=unsigned(p[4])*256+p[5];
    // Ethernet MTU already bounds this path; IPv6 jumbograms are unsupported.
    return n==40+payload;
}
inline size_t encodeData(uint8_t *out,size_t capacity,const uint8_t *eth,size_t n,uint16_t seq) {
    if(n<54 || n>1514 || eth[12]!=0x86 || eth[13]!=0xdd || !unicast(eth+6) ||
        !validIPv6(eth+14,n-14) || capacity<n+26) return 0;
    memset(out,0,40); out[0]=8;
    memcpy(out+4,eth,6); memcpy(out+10,eth+6,6); memcpy(out+16,bssid,6);
    put16(out+22,seq<<4); memcpy(out+24,snap,8); put16(out+32,0x0403);
    put16(out+34,seq); memcpy(out+38,eth+12,n-12); return n+26;
}
inline size_t dataHeaderLength(const uint8_t *f,size_t n) {
    if(!f || n<40 || (f[0]!=8 && f[0]!=0x88) || (f[1]&0xc7) || (f[22]&15) ||
       !unicast(f+10) || memcmp(f+16,bssid,6)) return 0;
    size_t hdr=f[0]==0x88 ? 26 : 24;
    // Fragment reassembly and HT-control/mesh headers remain unsupported.
    if(hdr==26 && (f[25]&1)) return 0;
    return hdr;
}
// MT7921 hardware deaggregation adds two bytes after the 802.11 header.
// Called only with RXD4 payload-format != 0; the caller pulls two bytes after
// relocating the header, as in mt76's raw (non-translated) RX path.
inline bool relocateDeaggregatedHeader(uint8_t *f,size_t n) {
    size_t hdr=dataHeaderLength(f,n);
    if(hdr!=26 || !(f[24]&0x80) || n<hdr+2+14+56) return false;
    memmove(f+2,f,hdr); return true;
}
struct DataView {
    const uint8_t *destination, *source, *payload;
    size_t length; uint16_t sequence;
};
inline size_t dataView(const uint8_t *f,size_t n,size_t at,DataView &out) {
    size_t hdr=dataHeaderLength(f,n);
    if(!hdr || at<hdr || at>=n) return 0;
    const bool aggregate=hdr==26 && (f[24]&0x80);
    const uint8_t *dst=f+4,*src=f+10,*p=f+at;
    size_t len=n-at,used=len;
    if(aggregate) {
        if(len<14) return 0;
        dst=p; src=p+6; size_t body=unsigned(p[12])*256+p[13];
        if(body>len-14 || memcmp(src,f+10,6)) return 0;
        used=14+body; p+=14; len=body;
        if(used<n-at) {
            size_t pad=(4-used%4)%4;
            // Padding occurs only between complete subframes.
            if(n-at-used<pad+14+56) return 0;
            used+=pad;
        }
    } else if(at!=hdr) return 0;
    if(len<56 || len>1516 || memcmp(p,snap,8) || le16(p+8)!=0x0403 ||
       le16(p+12)!=0 || p[14]!=0x86 || p[15]!=0xdd || !validIPv6(p+16,len-16)) return 0;
    out={dst,src,p,len,le16(p+10)}; return used;
}
inline bool validDataFrame(const uint8_t *f,size_t n) {
    size_t at=dataHeaderLength(f,n);
    if(!at || n>11454) return false;
    while(at<n) {
        DataView view={}; size_t used=dataView(f,n,at,view);
        if(!used) return false;
        at+=used;
    }
    return true;
}
inline size_t decodeDataView(uint8_t *out,size_t capacity,const DataView &view) {
    if(!out || !view.payload || !view.source || !view.destination ||
       view.length<56 || view.length>1516) return 0;
    size_t len=view.length-2;
    if(capacity<len) return 0;
    memcpy(out,view.destination,6); memcpy(out+6,view.source,6);
    memcpy(out+12,view.payload+14,view.length-14); return len;
}
// Single-MSDU API retained for callers expecting exactly one Ethernet frame.
inline size_t decodeData(uint8_t *out,size_t capacity,const uint8_t *f,size_t n,uint16_t &seq) {
    size_t hdr=dataHeaderLength(f,n);
    if(!hdr || (hdr==26 && (f[24]&0x80))) return 0;
    DataView view={};
    if(!dataView(f,n,hdr,view)) return 0;
    size_t len=decodeDataView(out,capacity,view);
    if(len) seq=view.sequence;
    return len;
}
// Reserved software PIDs distinguish AWDL frame classes in MT7921 TXS.
// WCIDs 15..18 belong exclusively to AWDL. A DMA release is not a TX result.
constexpr uint8_t txPidUnicast=125, txPidMulticast=126, txPidAction=127;
struct TxStatusCounters {
    uint64_t dataAcked=0, dataFailed=0, multicastCompleted=0, multicastFailed=0;
    uint64_t actionCompleted=0, actionFailed=0;
    uint32_t lastStatus=0;
    uint16_t lastSequence=0, lastWcid=0;
    uint8_t lastPid=0;
    bool observe(const uint8_t *p,size_t n) {
        if(!p || n<32) return false;
        uint32_t status=le32(p), wcid=(le32(p+8)>>16)&0x3ff;
        if(wcid<15 || wcid>18) return false;
        uint8_t pid=le32(p+12)>>24;
        // MT76 accepts only MPDU formats 0/1 here, not PPDU summaries.
        if(((status>>23)&3)>1 || pid<txPidUnicast || pid>txPidAction) return true;
        bool failed=status&0x00070000; // queue, RTS, or ACK timeout
        lastStatus=status; lastSequence=le32(p+4)>>20; lastWcid=wcid; lastPid=pid;
        if(pid==txPidUnicast) { if(failed) ++dataFailed; else ++dataAcked; }
        else if(pid==txPidMulticast) { if(failed) ++multicastFailed; else ++multicastCompleted; }
        else { if(failed) ++actionFailed; else ++actionCompleted; }
        return true;
    }
};
// A matching, unexpired firmware grant is mandatory before any transmission.
enum class RocResult : uint8_t {
    Accepted, Malformed, NoGrant, WrongBss, FirmwareStatus, WrongType,
    Disabled, NotPending, Token, Channel, Late, Duration, Count
};
struct Reservation {
    uint64_t deadline, grantObservedAt, requestedAt; uint8_t token, channel; bool pending, granted;
    uint32_t responseDelayUS;
    void request(uint8_t t,uint8_t c,uint64_t now) {
        token=t; channel=c; pending=true; granted=false; grantObservedAt=0;
        requestedAt=now; deadline=now+rocResponseTimeoutUs;
        responseDelayUS=0;
    }
    RocResult accept(uint8_t t,uint8_t c,uint32_t durationMs,uint64_t now) {
        if(!pending) return RocResult::NotPending;
        if(t!=token) return RocResult::Token;
        if(c!=channel) return RocResult::Channel;
        if(now<requestedAt || now>=deadline) return RocResult::Late;
        if(!durationMs || durationMs>maxRocMs) return RocResult::Duration;
        responseDelayUS=static_cast<uint32_t>(now-requestedAt);
        pending=false; granted=true; grantObservedAt=now; deadline=now+uint64_t(durationMs)*1000;
        return RocResult::Accepted;
    }
    bool grant(uint8_t t,uint8_t c,uint32_t durationMs,uint64_t now) {
        return accept(t,c,durationMs,now)==RocResult::Accepted;
    }
    bool canTransmit(uint8_t c,uint64_t now) const { return granted && channel==c && now<deadline && deadline-now>2000; }
    void cancel() { pending=granted=false; deadline=grantObservedAt=requestedAt=0; responseDelayUS=0; }
};
// Unified ROC event: four-byte header followed by bounded TLVs. MT7921's
// grant tag is zero and its complete structure is 20 bytes (mt7921.h).
struct RocGrant {
    uint8_t bss, token, status, channel, requestType;
    uint32_t durationMs;
};
inline RocResult decodeRocGrant(const uint8_t *p,size_t n,RocGrant &out) {
    out={};
    if(!p || n<4) return RocResult::Malformed;
    bool found=false;
    for(size_t at=4;at<n;) {
        if(n-at<4) return RocResult::Malformed;
        uint16_t tag=le16(p+at), len=le16(p+at+2);
        if(len<4 || len>n-at) return RocResult::Malformed;
        if(tag==0) {
            if(found || len!=20) return RocResult::Malformed;
            found=true;
            out={p[at+4],p[at+5],p[at+6],p[at+7],p[at+13],le32(p+at+16)};
        }
        at+=len;
    }
    return found ? RocResult::Accepted : RocResult::NoGrant;
}
inline RocResult receiveRocGrant(Reservation &reservation,const uint8_t *p,size_t n,
                                uint8_t bss,bool enabled,uint64_t now,RocGrant &out) {
    RocResult result=decodeRocGrant(p,n,out);
    if(result!=RocResult::Accepted) return result;
    if(out.bss!=bss) return RocResult::WrongBss;
    if(out.status) return RocResult::FirmwareStatus;
    if(out.requestType!=1) return RocResult::WrongType;
    if(!enabled) return RocResult::Disabled;
    return reservation.accept(out.token,out.channel,out.durationMs,now);
}
}
#endif
