// SPDX-License-Identifier: GPL-2.0-or-later
// Template for scripts/test-awdl-tx-trace.py. @...@ sections are verbatim
// production excerpts, not test reimplementations. OS/transport stubs below
// deliberately make no claim about actual firmware, radio or kernel timing.
#include <AWDL/AwdlProtocol.hpp>
#include <AWDL/AwdlTxTrace.hpp>
#include <mtk_wlan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/types.h>
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define GENMASK(h,l) ((~uint32_t(0) << (l)) & (~uint32_t(0) >> (31-(h))))
#define KASSERT(x) assert(x)
#define _KASSERT(x) assert(x)
#define htole16(x) uint16_t(x)
#define htole32(x) uint32_t(x)
#define le16toh(x) uint16_t(x)
#define le32toh(x) uint32_t(x)
#define letoh16(x) uint16_t(x)
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define NBPFILTER 0
#define IEEE80211_STA_ONLY 1
#define IEEE80211_CATEG_BA 3
#define IEEE80211_M_STA 1
#define IEEE80211_M_MONITOR 6
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->ic_flags & 0x80)
#define MBUF_DONTWAIT 0
#define M_DONTWAIT 0
#define IFF_RUNNING 1
#define MWX_DEV_LOG(...) ((void)0)
#define MTK_JOIN_TX_NONE 0
#define BUS_DMA_WRITE 1
#define BUS_DMA_NOWAIT 2
#define BUS_DMASYNC_PREWRITE 1
#define BUS_DMASYNC_POSTWRITE 2
#define MSEC_TO_NSEC(x) (uint64_t(x)*1000000)
using IOReturn=unsigned;
constexpr unsigned kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBadArgument=2,
    kIOReturnNoResources=3,kIOReturnNotFound=4,kIOReturnNoMemory=5,
    kIOReturnIOError=6,kIOReturnAborted=7;
unsigned heldLocks=0;
struct IOLock {std::mutex mutex;};
IOLock *IOLockAlloc(){return new IOLock;}
void IOLockFree(IOLock *p){delete p;}
void IOLockLock(IOLock *p){assert(p);p->mutex.lock();++heldLocks;}
void IOLockUnlock(IOLock *p){assert(p&&heldLocks);--heldLocks;p->mutex.unlock();}
unsigned liveMbufs=0,duplicateFailure=0,mapFailure=0,mappingSegments=1,peerAllocationFailure=0;
int mcuSendError=0,mcuWaitError=0;uint32_t mcuStatus=0;
uint64_t now=1000000;
struct mbuf {
    std::vector<uint8_t> bytes;size_t offset=64,packetLength=0;
    mbuf(){++liveMbufs;bytes.reserve(16384);bytes.resize(offset);}
    ~mbuf(){assert(liveMbufs);--liveMbufs;}
};
using mbuf_t=mbuf *;
void *mbuf_data(mbuf *m){return m->bytes.data()+m->offset;}
size_t mbuf_len(const mbuf *m){return m->bytes.size()-m->offset;}
size_t mbuf_pkthdr_len(const mbuf *m){return m->packetLength;}
void mbuf_setlen(mbuf *m,size_t n){assert(n+m->offset<=m->bytes.capacity());m->bytes.resize(m->offset+n);}
void mbuf_pkthdr_setlen(mbuf *m,size_t n){m->packetLength=n;}
mbuf *mbuf_next(mbuf *){return nullptr;}
#define mtod(m,t) reinterpret_cast<t>(mbuf_data(m))
void m_freem(mbuf *m){delete m;}
void mbuf_freem(mbuf *m){delete m;}
void m_copydata(mbuf *m,size_t at,size_t n,caddr_t out){assert(at+n<=mbuf_len(m));memcpy(out,mtod(m,const uint8_t *)+at,n);}
mbuf *m_pullup(mbuf *m,size_t n){assert(n<=mbuf_len(m));return m;}
int mbuf_dup(mbuf *m,int,mbuf **out){
    if(duplicateFailure)return ENOBUFS;
    auto copy=new mbuf;copy->bytes=m->bytes;copy->offset=m->offset;copy->packetLength=m->packetLength;*out=copy;return 0;
}
mbuf *mwx_mbuf_linearize(mbuf *){assert(false);return nullptr;}
int m_trailingspace(const mbuf *m){return int(m->bytes.capacity()-m->bytes.size());}
size_t m_leadingspace(const mbuf *m){return m->offset;}
mbuf *m_prepend(mbuf *m,size_t n,int){assert(n<=m->offset);m->offset-=n;m->packetLength+=n;return m;}
mbuf *mwx_alloc_sta_req_tlv(int n){
    if(peerAllocationFailure)return nullptr;
    auto m=new mbuf;m->offset+=n;m->bytes.resize(m->offset);return m;
}
[[noreturn]] void panic(const char *){assert(false);__builtin_trap();}
struct DmaSegment {uint64_t ds_addr=0;size_t ds_len=0;};
using bus_dma_segment_t=DmaSegment;
struct DmaMap {unsigned dm_nsegs=0;size_t dm_mapsize=0;DmaSegment dm_segs[8];bool loaded=false;};
using bus_dmamap_t=DmaMap *;
int bus_dmamap_load_mbuf(void *,DmaMap *map,mbuf *m,unsigned){
    assert(!map->loaded);if(mapFailure)return EFBIG;
    assert(mappingSegments && mappingSegments<=8);
    map->dm_nsegs=mappingSegments;map->dm_mapsize=mbuf_len(m);map->loaded=true;
    size_t remain=map->dm_mapsize;
    for(unsigned i=0;i<map->dm_nsegs;++i){size_t len=remain/(map->dm_nsegs-i);map->dm_segs[i]={0x10000000+uint64_t(i)*4096,len};remain-=len;}
    return 0;
}
void bus_dmamap_sync(void *,DmaMap *,size_t,size_t,unsigned){}
void bus_dmamap_unload(void *,DmaMap *map){map->loaded=false;map->dm_mapsize=0;}
uint64_t mwx_awdl_now(){return now;}
uint64_t mtk_join_now(){return now;}
bool PE_parse_boot_argn(const char *,void *,int){return false;}
int ratecheck(timeval *,const timeval *){return 1;}
int tsleep_nsec(void *,int,const char *,uint64_t){assert(false);return 0;}
@MACROS@
@WIRE@
@FRAMES@
@QUEUE_ENUM@
@PHY_ENUM@
@FRAME_HELPERS@
struct ieee80211_channel {unsigned ic_flags=0,index=0;};
struct ieee80211_node {uint8_t ni_macaddr[6]{};ieee80211_channel *ni_chan=nullptr;unsigned ni_flags=0,ni_rsnakms=0;uint16_t ni_associd=0;};
@NODES@
struct mwx_softc;
class MtkMwx {public:
@RESULT_TYPE@
    mwx_softc *com;
    IOReturn awdlTryTransmit(mbuf_t *,uint8_t);
    IOReturn awdlPeer(const uint8_t *,bool);
    bool awdlCanTransmit(uint8_t);
    void mwxWakeup(void *){}
};
MtkMwx *gMwxHal=nullptr;
void wakeup(void *){}
@NATIVE@
@TX_STRUCTS@
using Trace=mtkawdl::HalTxTrace;
struct JoinDiagnostic {bool Active=false;uint64_t Attempt=0;};
struct ieee80211com {
    struct {unsigned if_flags=IFF_RUNNING;} ic_if;ieee80211_channel ic_channels[256];
    ieee80211_node *ic_bss=nullptr;unsigned ic_opmode=IEEE80211_M_STA;uint8_t ic_myaddr[6]{};
    JoinDiagnostic ic_join_diag;uint64_t ic_awdl_data_at=0;
};
unsigned ieee80211_chan2ieee(ieee80211com *,ieee80211_channel *c){return c->index;}
struct mwx_softc {
    IOLock *sc_awdl_lock=IOLockAlloc();
    bool sc_awdl_enabled=true,sc_awdl_stopping=false,sc_resetting=false,sc_detaching=false,sc_awdl_busy=false;
    unsigned sc_awdl_calls=0;uint32_t sc_awdl_epoch=1;
    uint64_t sc_awdl_native_session=1;
    uint8_t sc_awdl_address[6]{2,1,1,1,1,1};
    mwx_vif sc_vif{},sc_awdl_vif{};mwx_node sc_awdl_peers[3]{};
    bool sc_awdl_peer_used[3]{},sc_awdl_station_pending[4]{};
    mtkawdl::TxStatusCounters sc_awdl_tx_status{};
    mtkawdl::Reservation sc_awdl_roc{};
    Trace *sc_awdl_tx_trace=nullptr;
    bool sc_awdl_roc_abort_pending=false;uint8_t sc_awdl_roc_abort_token=0,sc_awdl_channel=0;
    uint64_t sc_awdl_token_used[256]{},sc_awdl_roc_abort_retry_at=0;
    mwx_awdl_native_tx_table sc_awdl_native_tx{};
    mbuf *sc_awdl_native_tx_frame=nullptr;uint8_t sc_awdl_native_tx_pid=0;bool sc_awdl_native_tx_override=false;
    ieee80211com sc_ic;
    void *sc_dmat=nullptr;mwx_txwi_desc sc_txwi{};mwx_queue sc_txq{};unsigned dma_index=0;
    uint32_t sc_knobs=MWX_KNOB_DATA_RA;timeval sc_txs_last{};
    unsigned sc_tx_nobufs=0,sc_tx_ring_full=0;
    uint64_t sc_awdl_tx_bad_frame=0,sc_awdl_tx_busy=0,sc_awdl_tx_no_grant=0,
        sc_awdl_tx_no_peer=0,sc_awdl_tx_backpressure=0,sc_awdl_tx_copy_failed=0,
        sc_awdl_tx_dma_failed=0,sc_awdl_tx_submitted=0;int sc_awdl_tx_errno=0;
};
struct IOService {
    std::map<std::string,uint64_t> properties;
    std::map<std::string,std::vector<uint8_t>> bytes;
    void setProperty(const char *key,uint64_t value,unsigned){assert(!heldLocks);properties[key]=value;}
    void setProperty(const char *key,bool value){assert(!heldLocks);properties[key]=value;}
    // Match IOKit's non-const byte-copy parameter, so const publisher inputs fail.
    void setProperty(const char *key,void *value,unsigned length){
        assert(!heldLocks);auto p=static_cast<const uint8_t *>(value);bytes[key]={p,p+length};
    }
};
IOLock *mwx_txq_lock=IOLockAlloc();
void mwx_write(mwx_softc *,uint32_t reg,uint32_t){assert(reg==MT_DMA_CPU_IDX);}
uint32_t mwx_read(mwx_softc *sc,uint32_t reg){assert(reg==MT_DMA_DMA_IDX);return sc->dma_index;}
void mwx_tx_resume(mwx_softc *){}
void mtk_join_tx_complete(JoinDiagnostic *,uint64_t,unsigned,uint8_t,uint16_t,uint32_t){}
static bool mwx_awdl_current(mwx_softc *,uint32_t);
@TRACE_HELPERS@
@OPERATIONS@
int mwx_tx(mwx_softc *,mbuf *,ieee80211_node *,unsigned=0);
@RATES@
@FUNCTIONS@
// Production AWDL branch, including its existing (unchanged) rate adaptation.
// Infrastructure rate reporting is outside this diagnostic test's scope.
void mt7921_mac_add_txs(mwx_softc *sc,const uint32_t *txs_data){
@TXS_ARM@
}
std::vector<std::vector<uint8_t>> commands;
int mwx_mcu_send_mbuf(mwx_softc *,uint32_t cmd,mbuf *m,int *seq){
    assert(cmd==MCU_UNI_CMD_STA_REC_UPDATE);*seq=1;
    commands.emplace_back(mtod(m,const uint8_t *),mtod(m,const uint8_t *)+mbuf_len(m));m_freem(m);return mcuSendError;
}
int mwx_mcu_wait_resp_int(mwx_softc *,uint32_t cmd,int seq,uint32_t *status){
    assert(cmd==MCU_UNI_CMD_STA_REC_UPDATE && seq==1);*status=mcuStatus;return mcuWaitError;
}
int mwx_awdl_abort(mwx_softc *sc){sc->sc_awdl_roc.cancel();sc->sc_awdl_roc_abort_pending=false;return 0;}
@TAIL_FUNCTIONS@

static const Trace::Record &last(const Trace &t){assert(t.eventCount);return t.events[(t.eventCount-1)%Trace::capacity];}
static const Trace::TxsRecord &lastTxs(const Trace &t){assert(t.txsCount);return t.txs[(t.txsCount-1)%Trace::txsCapacity];}
static std::vector<uint8_t> bytes(const void *p,size_t n){auto b=static_cast<const uint8_t *>(p);return {b,b+n};}
static bool empty(const Trace::Token &t){Trace::Token zero{};return !memcmp(&t,&zero,sizeof(t));}
struct Fixture {
    std::unique_ptr<mwx_softc> state{new mwx_softc};mwx_softc &sc=*state;MtkMwx hal{&sc};mwx_node infra{};
    std::unique_ptr<Trace> trace{new Trace{}};Trace &tr=*trace;IOService registry;
    std::vector<mt76_txwi> descriptors{MWX_TXWI_MAX};std::vector<mwx_txwi> tokens{MWX_TXWI_MAX};
    std::vector<DmaMap> tokenMaps{MWX_TXWI_MAX};DmaMap descriptorMap{},queueMap{};
    std::vector<mwx_desc> ring{256};std::vector<mwx_queue_data> ringData{256};std::vector<DmaMap> ringMaps{256};
    uint8_t peer[6]{2,2,2,2,2,2};
    Fixture(){
        now=1000000;assert(!liveMbufs&&!heldLocks);commands.clear();mappingSegments=1;
        mapFailure=duplicateFailure=peerAllocationFailure=0;mcuSendError=mcuWaitError=mcuStatus=0;
        sc.sc_awdl_tx_trace=trace.get();
        sc.sc_awdl_vif.idx=1;sc.sc_awdl_vif.omac_idx=1;sc.sc_awdl_vif.wmm_idx=1;sc.sc_awdl_vif.vif_mn.wcid=18;
        for(unsigned i=0;i<256;++i)sc.sc_ic.ic_channels[i].index=i;
        infra.wcid=1;infra.ni.ni_chan=&sc.sc_ic.ic_channels[149];sc.sc_ic.ic_bss=&infra.ni;
        sc.sc_txwi.mt_desc=descriptors.data();sc.sc_txwi.mt_data=tokens.data();sc.sc_txwi.mt_count=tokens.size();sc.sc_txwi.mt_map=&descriptorMap;
        LIST_INIT(&sc.sc_txwi.mt_freelist);
        for(unsigned i=0;i<tokens.size();++i){auto &t=tokens[i];t.mt_desc=&descriptors[i];t.mt_idx=i;t.mt_map=&tokenMaps[i];t.mt_addr=0x20000000+i*64;
            if(i>=MT_PACKET_ID_FIRST)LIST_INSERT_HEAD(&sc.sc_txwi.mt_freelist,&t,mt_entry);}
        sc.sc_txq.mq_desc=ring.data();sc.sc_txq.mq_data=ringData.data();sc.sc_txq.mq_count=ring.size();sc.sc_txq.mq_map=&queueMap;
        for(unsigned i=0;i<ringData.size();++i)ringData[i].md_map=&ringMaps[i];
        assert(hal.awdlPeer(peer,true)==kIOReturnSuccess);grant();
    }
    ~Fixture(){for(auto &t:tokens)mwx_txwi_put(&sc,&t);IOLockFree(sc.sc_awdl_lock);assert(!liveMbufs&&!heldLocks);}
    void grant(uint8_t channel=149){sc.sc_awdl_roc.request(1,channel,now);assert(sc.sc_awdl_roc.grant(1,channel,300,now));}
    void reenable(){sc.sc_awdl_stopping=sc.sc_resetting=sc.sc_detaching=false;sc.sc_awdl_enabled=true;
        sc.sc_awdl_native_session+=101;assert(hal.awdlPeer(peer,true)==0);grant();}
    mbuf *frame(size_t length=1524,uint8_t tid=0,uint16_t sequence=17,bool multicast=false){
        assert(length>=40);auto m=new mbuf;mbuf_setlen(m,length);mbuf_pkthdr_setlen(m,length);auto f=mtod(m,uint8_t *);f[0]=0x88;
        memcpy(f+4,peer,6);if(multicast)f[4]|=1;memcpy(f+10,sc.sc_awdl_address,6);memcpy(f+16,mtkawdl::bssid,6);
        mtkawdl::put16(f+22,sequence<<4);mtkawdl::put16(f+24,tid);return m;
    }
    unsigned submit(uint16_t seq=17){auto m=frame(1524,0,seq);auto index=sc.sc_txq.mq_prod;
        assert(hal.awdlTryTransmit(&m,149)==0 && !m);assert(ringData[index].md_txwi);return ringData[index].md_txwi->mt_idx;}
    std::array<uint32_t,8> status(unsigned token,bool failed=false){
        auto &d=descriptors[token];std::array<uint32_t,8> s{};
        s[0]=0x80008000|((d.txwi[6]&MT_TXD6_TX_RATE_MASK)>>16)|(failed?0x10000:0);
        s[1]=((d.txwi[3]&MT_TXD3_SEQ)>>16)<<20;s[2]=(d.txwi[1]&MT_TXD1_WLAN_IDX_MASK)<<16;s[3]=(d.txwi[5]&255)<<24;return s;
    }
    void dmaConsumed(){sc.dma_index=sc.sc_txq.mq_prod;mwx_dma_tx_cleanup(&sc,&sc.sc_txq);}
    void freeTokens(const std::vector<unsigned> &ids,unsigned status=0,unsigned count=1,bool pairs=false){
        assert(ids.size()<1024 && status<4 && count<8192);
        auto m=new mbuf;size_t n=8+4*(ids.size()+(pairs?2:0));mbuf_setlen(m,n);mbuf_pkthdr_setlen(m,n);auto p=mtod(m,uint8_t *);
        mtkawdl::put32(p,uint32_t(ids.size())<<16);size_t at=8;
        if(pairs){mtkawdl::put32(p+at,0x80000000|(15<<14));at+=4;}
        for(auto id:ids){mtkawdl::put32(p+at,(id<<16)|(status<<13)|count);at+=4;}
        if(pairs)mtkawdl::put32(p+at,0x80000000|(16<<14));
        sc.dma_index=sc.sc_txq.mq_prod;mwx_mac_tx_free(&sc,m);
    }
    const Trace::Summary &publish(){mwx_awdl_tx_trace_publish(&sc,&registry);return tr.published.summary;}
    Trace::Observer observer(unsigned wcid=15){IOLockLock(sc.sc_awdl_lock);auto o=mwx_awdl_tx_trace_observer(&sc,wcid);IOLockUnlock(sc.sc_awdl_lock);return o;}
};

static void parserAndLayouts(){
    static_assert(sizeof(mtk_wlan_tx_status)==44 && offsetof(mtk_wlan_tx_status,errors)==32 && offsetof(mtk_wlan_tx_status,pid)==42,"manually synced TXS ABI");
    static_assert(sizeof(Trace::Token)==56 && sizeof(Trace::Record)==96 && sizeof(Trace::TxsRecord)==88,"trace wire layouts");
    static_assert(sizeof(Trace::Summary)==424 && offsetof(Trace::Summary,counters)==152,"summary u64 layout");
    std::array<uint32_t,8> raw{0xd987abcd,0xfff01234,0x03ff0000,0xfd010203,0x12345678,0xabcdef01,0x11223344,0x55667788};
    std::vector<uint8_t> misaligned(40);memcpy(misaligned.data()+1,raw.data(),32);
    mtk_wlan_tx_status out{},zero{};
    for(unsigned n=0;n<32;++n){memset(&out,0xa5,sizeof(out));assert(!mtk_wlan_txs_parse(misaligned.data()+1,n,&out));assert(!memcmp(&out,&zero,sizeof(out)));}
    assert(mtk_wlan_txs_parse(misaligned.data()+1,39,&out));assert(!memcmp(out.raw,raw.data(),32));
    assert(out.wcid==1023 && out.sequence==4095 && out.pid==253 && out.format==3 && out.rate==0x2bcd && out.errors==0x70000);
    assert(!mtk_wlan_txs_parse(raw.empty()?nullptr:reinterpret_cast<uint8_t *>(raw.data()),32,nullptr));
    puts("PASS bounded TXS Reader/FFI: every truncation, unaligned/trailing bytes, raw words, masks and exact C layouts");
}

static void submittingAndCompleting(){
    Fixture f;auto id=f.submit(4095);auto own=f.tokens[id].mt_awdl_tx;auto txs=f.status(id);
    assert(own.id==1 && own.session==1 && own.epoch==1 && own.incarnation==1 && own.token==id);
    assert(own.wcid==15 && own.sequence==4095 && own.requestedRate==0x4b && own.length==1524 && own.channel==149 && own.pid==125);
    assert(own.state==Trace::Submitted && own.submittedAtUS==now && own.flags==(Trace::SequenceKnown|Trace::RequestedRateKnown|Trace::ChannelKnown|Trace::ContextKnown|Trace::LengthKnown));
    assert(last(f.tr).kind==Trace::Submit && last(f.tr).status==0xff && !last(f.tr).count && last(f.tr).flags==Trace::ContextCurrent);
    assert(!f.tr.counters.freeAcked&&!f.tr.txsCount&&f.sc.sc_txwi.mt_inuse==1);
    auto &s=f.publish();assert(s.outstanding==1 && s.currentOutstanding==1 && !s.ambiguousOutstanding && s.ageKnown==3 && !s.oldestAgeUS);
    f.dmaConsumed();assert(f.sc.sc_txwi.mt_inuse==1 && f.publish().outstanding==1); // DMA consumed is not TX-free
    now+=12345;f.freeTokens({id},0,4,true);
    const auto &r=last(f.tr);
    assert(r.kind==Trace::TxFree && r.submitted.id==own.id && r.submitted.requestedRate==0x4b && r.raw==((id<<16)|4));
    assert(r.status==0 && r.count==4 && r.atUS==now && r.latencyUS==12345 && r.flags==(Trace::ContextCurrent|Trace::FreePresent|Trace::Attributed|Trace::LatencyKnown|Trace::CountKnown));
    assert(f.tr.counters.freeAcked==1 && f.tr.counters.transmissions==4 && f.tr.counters.retries==3 && f.tr.counters.latency[3]==1);
    assert(empty(f.tokens[id].mt_awdl_tx)&&!f.tr.live[id]&&!f.sc.sc_txwi.mt_inuse&&!f.publish().outstanding&&!liveMbufs);
    txs[0]=(txs[0]&~0x3ffu)|0x4a;mt7921_mac_add_txs(&f.sc,txs.data());
    assert(lastTxs(f.tr).rate==0x4a && last(f.tr).submitted.requestedRate==0x4b && f.tr.counters.freeAcked==1 && f.tr.txsCount==1);
    for(unsigned status=0;status<4;++status){id=f.submit(status);now+=10;f.freeTokens({id},status,status?8191:0);}
    assert(f.tr.counters.freeAcked==2 && f.tr.counters.freeFailed==3 && f.tr.counters.countUnknown==1);
    assert(f.tr.counters.transmissions==4+3*8191 && f.tr.counters.retries==3+3*8190);
    id=f.submit(23);--now;f.freeTokens({id});assert(!(last(f.tr).flags&Trace::LatencyKnown)&&!last(f.tr).latencyUS&&f.tr.counters.clockInvalid==1);
    puts("PASS owned submission/TX-free: actual token metadata, DMA-vs-firmware ownership, statuses 0..3, count 0/8191, retries/latency and requested-vs-reported rates");
}

static void descriptorControls(){
    Fixture f;static constexpr uint8_t ac[8]={1,0,0,1,2,2,3,3};
    for(unsigned rate=0;rate<7;++rate)for(uint8_t tid=0;tid<8;++tid)for(size_t size:{size_t(64),size_t(1524),size_t(3839)}){
        f.sc.sc_awdl_tx_status.rates[0].index=rate;
        auto m=f.frame(size,tid,4095);mt76_txwi d{};mt7921_mac_write_txwi(&f.sc,m,&f.sc.sc_awdl_peers[0].ni,&d);
        assert((d.txwi[0]&MT_TXD0_TX_BYTES_MASK)==size+MT_TXD_SIZE);
        assert(((d.txwi[0]&MT_TXD0_Q_IDX_MASK)>>25)==4+ac[tid]);
        assert((d.txwi[1]&MT_TXD1_WLAN_IDX_MASK)==15 && ((d.txwi[1]&MT_TXD1_OWN_MAC_MASK)>>24)==1);
        assert(((d.txwi[1]&MT_TXD1_HDR_INFO_MASK)>>11)==13 && ((d.txwi[1]&MT_TXD1_TID_MASK)>>20)==tid);
        assert((d.txwi[2]&MT_TXD2_FIX_RATE) && !(d.txwi[2]&MT_TXD2_MAX_TX_TIME_MASK));
        assert((d.txwi[3]&(MT_TXD3_BA_DISABLE|MT_TXD3_SN_VALID))==(MT_TXD3_BA_DISABLE|MT_TXD3_SN_VALID));
        assert(((d.txwi[3]&MT_TXD3_SEQ)>>16)==4095 && ((d.txwi[3]&MT_TXD3_REM_TX_COUNT_MASK)>>11)==15);
        assert(!(d.txwi[3]&(MT_TXD3_NO_ACK|MT_TXD3_PROTECT_FRAME)));
        assert((d.txwi[5]&MT_TXD5_PID)==125 && (d.txwi[5]&MT_TXD5_TX_STATUS_HOST));
        assert(((d.txwi[6]&MT_TXD6_TX_RATE_MASK)>>16)==f.sc.sc_awdl_tx_status.rate(15));
        assert((d.txwi[6]&MT_TXD6_FIXED_BW) && !(d.txwi[6]&MT_TXD6_BW));
        assert(!(d.txwi[7]&(MT_TXD7_HW_AMSDU|MT_TXD7_IP_SUM|MT_TXD7_UDP_TCP_SUM)));m_freem(m);
    }
    auto m=f.frame();mt76_txwi d{};mt7921_mac_write_txwi(&f.sc,m,&f.infra.ni,&d);
    assert(!(d.txwi[2]&MT_TXD2_FIX_RATE) && !(d.txwi[3]&MT_TXD3_BA_DISABLE));
    auto p=mtod(m,uint8_t *);p[0]=0x48;p[1]=0x11;mt7921_mac_write_txwi(&f.sc,m,&f.infra.ni,&d);assert(d.txwi[3]&MT_TXD3_SW_POWER_MGMT);m_freem(m);
    for(unsigned kind=0;kind<3;++kind){
        m=f.frame(64,0,23,kind==0);auto ring=f.sc.sc_txq.mq_prod;
        if(kind==1){auto raw=mtod(m,uint8_t *);raw[0]=0xd0;raw[24]=127;}
        if(kind==2)assert(mwx_tx(&f.sc,m,&f.infra.ni)==0);
        else assert(f.hal.awdlTryTransmit(&m,149)==0&&!m);
        auto id=f.ringData[ring].md_txwi->mt_idx;assert(empty(f.tokens[id].mt_awdl_tx));
        if(kind==0)assert(f.descriptors[id].txwi[3]&MT_TXD3_NO_ACK);
        if(kind==1)assert((f.descriptors[id].txwi[5]&255)>=80 && (f.descriptors[id].txwi[5]&255)<112);
        f.freeTokens({id});
    }
    assert(!f.tr.ticketCount&&!f.tr.eventCount&&!f.tr.counters.freeAcked);
    puts("PASS unchanged descriptor controls: 168 length/TID/rate cases, ACK/retry/QoS/SN bits, infra RA/PM-null, multicast and native UMI excluded");
}

static void failurePaths(){
    Fixture f;auto m=f.frame();auto original=m;duplicateFailure=1;
    assert(f.hal.awdlTryTransmit(&m,149)==kIOReturnNoMemory&&m==original&&!f.tr.ticketCount);duplicateFailure=0;
    for(unsigned mode=0;mode<2;++mode){
        if(mode==0)mapFailure=1;else mappingSegments=5;
        assert(f.hal.awdlTryTransmit(&m,149)==kIOReturnNoResources&&m==original&&!f.sc.sc_txwi.mt_inuse);
        assert(last(f.tr).kind==Trace::SubmitFailure&&last(f.tr).status==0xff&&last(f.tr).hostError==EFBIG&&!last(f.tr).submitted.submittedAtUS);
        assert(f.publish().outstanding==0&&f.publish().prepared==0&&!f.tr.counters.reclaimUnobserved);
        for(const auto &t:f.tokens)assert(empty(t.mt_awdl_tx));
        mapFailure=0;mappingSegments=1;
    }
    mbuf_freem(m);std::vector<unsigned> ids;
    for(unsigned i=0;i<255;++i)ids.push_back(f.submit(i));
    m=f.frame();assert(mwx_tx(&f.sc,m,&f.sc.sc_awdl_peers[0].ni)==EBUSY);
    assert(last(f.tr).kind==Trace::SubmitFailure&&last(f.tr).hostError==EBUSY&&f.sc.sc_txwi.mt_inuse==255);
    assert(f.publish().outstanding==255&&f.tr.counters.submitFailed==3);
    f.freeTokens(ids);assert(!f.publish().outstanding&&f.tr.counters.freeAcked==255);
    auto id=f.submit(500);f.freeTokens({id});assert(f.tr.counters.freeAcked==256); // failed tickets did not poison normal reuse
    auto trace=f.sc.sc_awdl_tx_trace;f.sc.sc_awdl_tx_trace=nullptr;
    id=f.submit(501);f.freeTokens({id});mwx_awdl_tx_trace_publish(&f.sc,&f.registry);
    assert(!f.registry.properties["MTKAWDLTXTraceAvailable"]&&!f.sc.sc_txwi.mt_inuse);
    f.sc.sc_awdl_tx_trace=trace;assert(f.tr.counters.freeAcked==256);
    puts("PASS failures: duplicate/map/segment/ring admission, cleared sidecars and guards, original ownership, allocation-unavailable diagnostics do not block TX");
}

static void wrapAndOutstanding(){
    Fixture f;std::vector<unsigned> ids;
    for(unsigned i=0;i<255;++i)ids.push_back(f.submit(i));
    f.dmaConsumed();for(unsigned i=0;i<254;++i)ids.push_back(f.submit(255+i));
    assert(f.sc.sc_txwi.mt_inuse==509&&f.tr.eventCount==509);
    auto m=f.frame();assert(f.hal.awdlTryTransmit(&m,149)==kIOReturnNoResources&&m);m_freem(m);
    now+=2000000;const auto &s=f.publish();assert(s.outstanding==509&&s.currentOutstanding==509&&s.oldestAgeUS==2000000);
    assert(f.tr.published.oldest.id==1&&f.tr.published.oldest.sequence==0);
    auto old=f.tokens[ids[0]].mt_awdl_tx;
    assert(f.tr.events[0].submitted.id!=old.id); // submitting row has wrapped several times
    f.freeTokens({ids[0]},2,7);assert(last(f.tr).submitted.id==old.id&&last(f.tr).submitted.sequence==0&&last(f.tr).latencyUS==2000000);
    assert(f.publish().outstanding==508&&f.tr.counters.freeFailed==1);
    f.freeTokens(std::vector<unsigned>(ids.begin()+1,ids.end()),0,1,true);
    assert(!f.publish().outstanding&&!f.sc.sc_txwi.mt_inuse&&f.tr.counters.freeAcked==508&&f.tr.counters.freeFailed==1);
    for(unsigned i=0;i<Trace::capacity;++i){const auto &r=f.tr.events[i];assert(r.id&&(r.id-1)%Trace::capacity==i&&r.id<=f.tr.eventCount);}
    assert(!f.tr.txsCount&&!f.tr.faults&&!f.sc.sc_awdl_tx_status.dataAcked);
    puts("PASS pressure/wrap: all 509 tokens, real 255-slot DMA ring wrap, PAIR words, evicted submitting rows, oldest live identity/age and zero-underflow gauges");
}

static void staleTicketAndNormalReuse(){
    Fixture f;auto id=f.submit(8);auto old=f.tokens[id].mt_awdl_tx;f.freeTokens({id});
    auto reused=f.submit(9);assert(id==reused&&f.tokens[id].mt_awdl_tx.id!=old.id);
    auto live=f.tokens[id].mt_awdl_tx;auto poolFlags=f.tr.tokenFlags[id];
    assert(poolFlags==Trace::PoolInFlight);
    mtk_wlan_tx_freed free{};free.msdu=id;free.count=1;free.raw=id<<16|1;
    // This represents an obsolete saved metadata reference, not a hardware
    // generation that TX-free magically supplies (it does not supply one).
    f.tr.complete(old,free,f.observer(),now);
    assert(!memcmp(&live,&f.tokens[id].mt_awdl_tx,sizeof(live))&&f.tr.live[id]==live.id);
    assert(f.publish().outstanding==1&&f.tr.counters.freeAcked==1&&last(f.tr).kind==Trace::UnattributedFree&&!last(f.tr).submitted.id);
    assert(f.tr.tokenFlags[id]==poolFlags);
    f.tr.reclaim(old,f.observer(),now);assert(empty(old)&&f.tr.live[id]==live.id&&f.publish().outstanding==1);
    assert(f.tr.tokenFlags[id]==poolFlags); // saved tickets cannot clear the newer HAL pool witness
    f.freeTokens({id});assert(f.tr.counters.freeAcked==2&&f.tr.counters.freeCurrent==2&&!f.publish().outstanding);
    // A wire duplicate while idle is observable. Poison diagnostic attribution
    // only; a later reused token is still immediately available to ordinary TX.
    f.freeTokens({id});assert(f.tr.counters.idleFree==1&&f.tr.counters.tokenTaints==1&&!f.publish().outstanding);
    assert(f.submit(10)==id);assert(f.publish().ambiguousOutstanding==1&&f.publish().currentOutstanding==1);
    f.freeTokens({id});assert(f.tr.counters.freeAcked==2&&f.tr.counters.reclaimUnobserved==1&&!f.publish().outstanding);
    puts("PASS reused token guards: obsolete ticket copies and idle wire duplicates cannot complete newer metadata or decrement its outstanding gauge; clean reuse still succeeds");
}

static void sharedPoolSafety(){
    enum {CleanFree,UnobservedReset,IdleDuplicate,MappingFailure};
    unsigned cases=0;
    for(unsigned scope=0;scope<4;++scope)for(bool previouslyTraced:{false,true})
    for(unsigned end=CleanFree;end<=MappingFailure;++end){
        Fixture f;const uint16_t sequence=uint16_t(1000+cases*3);++cases;
        if(previouslyTraced){auto id=f.submit(sequence);f.freeTokens({id});}
        const auto before=f.tr.counters;const auto events=f.tr.eventCount;
        auto available=LIST_FIRST(&f.sc.sc_txwi.mt_freelist);assert(available);
        const unsigned id=available->mt_idx,index=f.sc.sc_txq.mq_prod;
        auto m=f.frame(1524,0,sequence+1,scope==1);
        if(scope>=2){auto p=mtod(m,uint8_t *);p[0]=0xd0;p[24]=scope==3?127:4;}
        mapFailure=end==MappingFailure;
        if(scope==0){
            assert(mwx_tx(&f.sc,m,&f.infra.ni)==(mapFailure?EFBIG:0));
        }else{
            auto original=m;auto error=f.hal.awdlTryTransmit(&m,149);
            if(mapFailure){assert(error==kIOReturnNoResources&&m==original);m_freem(m);}
            else assert(error==kIOReturnSuccess&&!m);
        }
        mapFailure=0;
        assert(empty(f.tokens[id].mt_awdl_tx)&&f.tr.eventCount==events);
        assert(f.tr.ticketCount==unsigned(previouslyTraced));
        if(end==MappingFailure){
            assert(!f.tokens[id].mt_busy&&!f.sc.sc_txwi.mt_inuse);
            assert(!(f.tr.tokenFlags[id]&Trace::PoolInFlight));
        }else{
            assert(f.ringData[index].md_txwi==&f.tokens[id]&&f.tokens[id].mt_busy);
            assert(f.tr.tokenFlags[id]&Trace::PoolInFlight);
            assert(!f.publish().outstanding); // not an ordinary-unicast ticket
            if(scope==3){auto pid=f.descriptors[id].txwi[5]&255;assert(pid>=80&&pid<112);}
            if(end==UnobservedReset){
                mwx_awdl_quiesce(&f.sc,true);f.dmaConsumed();
                // Production reset's token put loop, without an observed TX-free.
                IOLockLock(mwx_txq_lock);
                for(auto &token:f.tokens)mwx_txwi_put(&f.sc,&token);
                IOLockUnlock(mwx_txq_lock);
                f.reenable();
            }else{
                f.freeTokens({id});
                assert(!f.tokens[id].mt_busy&&!(f.tr.tokenFlags[id]&Trace::PoolInFlight));
                assert(!(f.tr.tokenFlags[id]&Trace::Unsafe));
                if(end==IdleDuplicate)f.freeTokens({id}); // warning BEFORE AWDL reuse
            }
        }
        const bool unsafe=end==UnobservedReset||end==IdleDuplicate;
        assert(bool(f.tr.tokenFlags[id]&Trace::Unsafe)==unsafe);
        assert(!(f.tr.tokenFlags[id]&Trace::PoolInFlight));
        assert(f.tr.counters.tokenTaints==unsigned(unsafe));
        assert(f.tr.counters.idleFree==unsigned(end==IdleDuplicate));
        assert(f.tr.counters.reclaimUnobserved==0&&f.tr.counters.freeAcked==before.freeAcked);
        assert(f.submit(sequence+2)==id);auto current=f.tokens[id].mt_awdl_tx;
        auto &summary=f.publish();
        assert(summary.outstanding==1&&summary.currentOutstanding==1&&summary.ambiguousOutstanding==unsigned(unsafe));
        now+=100;f.freeTokens({id},0,7); // delayed prior report when unsafe; normal control otherwise
        assert(!f.publish().outstanding&&!f.sc.sc_txwi.mt_inuse&&!f.tr.faults);
        assert(!(f.tr.tokenFlags[id]&Trace::PoolInFlight));
        assert(bool(f.tr.tokenFlags[id]&Trace::Unsafe)==unsafe);
        if(unsafe){
            assert(f.tr.counters.freeAcked==before.freeAcked&&f.tr.counters.freeFailed==before.freeFailed);
            assert(f.tr.counters.transmissions==before.transmissions&&f.tr.counters.retries==before.retries);
            assert(f.tr.counters.latencySamples==before.latencySamples&&f.tr.counters.reclaimUnobserved==1);
            assert(last(f.tr).kind==Trace::Reclaim&&last(f.tr).submitted.id==current.id&&last(f.tr).status==0xff);
            const auto &raw=f.tr.events[(f.tr.eventCount-2)%Trace::capacity];
            assert(raw.kind==Trace::UnattributedFree&&raw.status==0&&raw.count==7&&raw.submitted.token==id);
            assert(!raw.submitted.id&&!raw.submitted.session&&!raw.submitted.incarnation);
            assert((raw.flags&Trace::TokenUnsafe)&&!(raw.flags&(Trace::Attributed|Trace::ContextCurrent|Trace::LatencyKnown)));
        }else{
            assert(last(f.tr).kind==Trace::TxFree&&last(f.tr).submitted.id==current.id&&last(f.tr).latencyUS==100);
            assert((last(f.tr).flags&(Trace::Attributed|Trace::ContextCurrent))==(Trace::Attributed|Trace::ContextCurrent));
            assert(f.tr.counters.freeAcked==before.freeAcked+1&&f.tr.counters.transmissions==before.transmissions+7);
            assert(f.tr.counters.retries==before.retries+6&&f.tr.counters.latencySamples==before.latencySamples+1);
        }
    }
    assert(cases==32);
    puts("PASS shared pool: 32 infra/multicast/action/native-UMI cases, with/without prior AWDL; reset and observed idle duplicates taint later reuse, clean frees and pre-doorbell failures stay attributable");
}

static void peerAndSessionLifetimes(){
    for(unsigned mode=0;mode<5;++mode){
        Fixture f;auto id=f.submit(123);auto old=f.tokens[id].mt_awdl_tx;
        if(mode<2){assert(f.hal.awdlPeer(f.peer,false)==0);if(mode)++f.peer[5];assert(f.hal.awdlPeer(f.peer,true)==0);}
        if(mode==2)++f.sc.sc_awdl_native_session;
        if(mode==3)++f.sc.sc_awdl_epoch;
        if(mode==4)assert(f.hal.awdlPeer(f.peer,false)==0);
        assert(!f.publish().currentOutstanding&&f.publish().outstanding==1);
        now+=1000;f.freeTokens({id},mode==1?2:0,3);auto r=last(f.tr);
        assert(r.kind==Trace::TxFree&&(r.flags&Trace::Attributed)&&!(r.flags&Trace::ContextCurrent));
        assert(r.submitted.session==old.session&&r.submitted.epoch==old.epoch&&r.submitted.incarnation==old.incarnation);
        assert(f.tr.counters.freeNotCurrent==1);
        if(mode==2)assert(r.flags&Trace::SessionChanged);else if(mode==3)assert(r.flags&Trace::EpochChanged);else assert(r.flags&Trace::PeerChanged);
        if(mode==4)assert(f.hal.awdlPeer(f.peer,true)==0);
        id=f.submit(124);f.freeTokens({id});assert(last(f.tr).flags&Trace::ContextCurrent);assert(f.tr.counters.freeCurrent==1);
    }
    for(unsigned fail=0;fail<4;++fail){
        Fixture f;auto id=f.submit();auto incarnation=f.tr.peerIncarnation[0];
        if(fail==0)peerAllocationFailure=1;if(fail==1)mcuSendError=EIO;if(fail==2)mcuWaitError=ETIMEDOUT;if(fail==3)mcuStatus=7;
        assert(f.hal.awdlPeer(f.peer,false)!=0&&f.sc.sc_awdl_peer_used[0]&&f.tr.peerIncarnation[0]==incarnation);
        peerAllocationFailure=0;mcuSendError=mcuWaitError=mcuStatus=0;
        f.freeTokens({id});assert(last(f.tr).flags&Trace::ContextCurrent);
    }
    puts("PASS per-peer/session/HAL lifetimes: same/different-address reincarnation, removal, independent session/epoch change, failed peer commands, and current success controls");
}

static void quiesceAndUnobservedReclaim(){
    Fixture f;auto id=f.submit(31);auto old=f.tokens[id].mt_awdl_tx;auto events=bytes(f.tr.events,sizeof(f.tr.events));
    mwx_awdl_quiesce(&f.sc,true);
    assert(!f.sc.sc_awdl_enabled&&!f.sc.sc_awdl_native_session&&f.sc.sc_awdl_epoch==old.epoch+1);
    assert(f.tokens[id].mt_awdl_tx.id==old.id&&f.sc.sc_txwi.mt_inuse==1&&bytes(f.tr.events,sizeof(f.tr.events))==events);
    assert(f.tr.counters.contextEnds==1&&f.tr.counters.lastContextEndSession==old.session);
    f.registry.properties.clear();f.registry.bytes.clear();now+=1000000;
    auto &s=f.publish();assert(s.outstanding==1&&!s.currentOutstanding&&s.oldestAgeUS==1000000);
    assert(f.registry.bytes["MTKAWDLTXTrace"]==events&&f.registry.properties["MTKAWDLTXTraceAtUS"]==now);
    assert(f.registry.bytes["MTKAWDLTXTraceSummary"]==bytes(&s,sizeof(s)));
    // Execute the exact reclaim primitive used by reset; register/firmware reset
    // itself and enable's MCU commands are explicit boundaries of this fixture.
    f.dmaConsumed();mwx_txwi_put(&f.sc,&f.tokens[id]);
    assert(last(f.tr).kind==Trace::Reclaim&&last(f.tr).submitted.id==old.id&&last(f.tr).status==0xff&&!(last(f.tr).flags&Trace::FreePresent));
    assert(empty(f.tokens[id].mt_awdl_tx)&&f.tr.counters.reclaimUnobserved==1&&!f.tr.counters.freeAcked&&!f.publish().outstanding);
    f.reenable();auto newId=f.submit(32);assert(newId==id);auto newer=f.tokens[id].mt_awdl_tx;
    assert(newer.id!=old.id&&newer.session!=old.session&&newer.incarnation!=old.incarnation&&(newer.flags&Trace::AmbiguousToken));
    assert(f.publish().outstanding==1&&f.publish().ambiguousOutstanding==1);
    // Exactly the delayed old wire token after reuse: no generation is present.
    f.freeTokens({id});assert(!f.tr.counters.freeAcked&&f.tr.counters.freeUnattributed==1&&f.tr.counters.reclaimUnobserved==2);
    assert(last(f.tr).kind==Trace::Reclaim&&last(f.tr).submitted.id==newer.id);
    const auto &raw=f.tr.events[(f.tr.eventCount-2)%Trace::capacity];
    assert(raw.kind==Trace::UnattributedFree&&raw.status==0&&raw.submitted.token==id&&!raw.submitted.id&&!raw.submitted.session);
    // Untainted current tokens still produce real status while the canceled
    // token remains usable (not quarantined) by the actual DMA allocator.
    auto tainted=f.submit(33);auto safe=f.submit(34);assert(tainted==id&&safe!=id);
    f.freeTokens({safe});assert(f.tr.counters.freeAcked==1&&(last(f.tr).flags&Trace::ContextCurrent));f.freeTokens({tainted});
    assert(!f.publish().outstanding&&f.tr.counters.freeAcked==1);
    mwx_awdl_quiesce(&f.sc,true);f.registry.bytes.clear();f.publish();assert(!f.registry.bytes["MTKAWDLTXTrace"].empty());
    puts("PASS real quiesce/put: history and watchdog publication survive sync off, outstanding vs reclaim remain distinct, reset-token reuse fails closed only for diagnostics");
}

static void rawTxsObserverOnly(){
    Fixture f;auto id=f.submit(4095);auto raw=f.status(id);auto original=f.tokens[id].mt_awdl_tx;
    assert(f.hal.awdlPeer(f.peer,false)==0);++f.peer[5];assert(f.hal.awdlPeer(f.peer,true)==0);++f.sc.sc_awdl_native_session;
    for(unsigned i=0;i<Trace::txsCapacity*3+7;++i){
        raw[0]=0x80008000|((i%4)<<23)|0x487|((i%8)<<16);
        raw[4]=0x11223344+i;raw[7]=0x55667788-i;now+=7;mt7921_mac_add_txs(&f.sc,raw.data());
        const auto &r=lastTxs(f.tr);assert(r.rate==0x487&&r.errors==((i%8)<<16)&&r.format==i%4&&r.wcid==15&&r.sequence==4095&&r.pid==125);
        assert(r.atUS==now&&r.observerSession==f.sc.sc_awdl_native_session&&r.observerSession!=original.session&&r.observerIncarnation!=original.incarnation);
        assert(!memcmp(r.raw,raw.data(),32));
    }
    assert(f.tr.eventCount==1&&!f.tr.counters.freeAcked&&f.publish().outstanding==1);
    for(unsigned i=0;i<Trace::txsCapacity;++i)assert((f.tr.txs[i].id-1)%Trace::txsCapacity==i);
    auto count=f.tr.txsCount;
    for(unsigned pid:{80,111,126,127}){auto other=raw;other[3]=pid<<24;mt7921_mac_add_txs(&f.sc,other.data());}
    raw[2]=14<<16;mt7921_mac_add_txs(&f.sc,raw.data());assert(f.tr.txsCount==count);
    mwx_awdl_tx_trace_txs(&f.sc,reinterpret_cast<uint8_t *>(raw.data()),31);assert(f.tr.txsCount==count&&f.tr.counters.txsMalformed==1);
    mwx_awdl_quiesce(&f.sc,true);raw[2]=15<<16;mt7921_mac_add_txs(&f.sc,raw.data());
    assert(!lastTxs(f.tr).observerSession&&(lastTxs(f.tr).observerFlags&Trace::Stopping));
    f.publish();assert(f.registry.bytes["MTKAWDLTXTraceTXS"]==bytes(f.tr.txs,sizeof(f.tr.txs)));
    f.freeTokens({id});assert(f.tr.counters.freeAcked==1&&f.tr.counters.freeNotCurrent==1);
    puts("PASS raw TXS ring: all raw words, PPDU formats, errors/rate, wrap/filtering, delayed old reports labeled current OBSERVER only, no token/session inference");
}

static void rateAdaptationUnchanged(){
    Fixture f;
    for(unsigned i=0;i<96;++i){auto id=f.submit(i);auto s=f.status(id);mt7921_mac_add_txs(&f.sc,s.data());f.freeTokens({id});}
    assert(f.sc.sc_awdl_tx_status.rate(15)==0x87&&f.sc.sc_awdl_tx_status.dataAcked==96);
    auto id=f.submit(96);auto s=f.status(id,true);mt7921_mac_add_txs(&f.sc,s.data());f.freeTokens({id},2);
    assert(f.sc.sc_awdl_tx_status.rate(15)==0x4c);
    std::vector<unsigned> ids;std::vector<std::array<uint32_t,8>> statuses;
    assert(f.hal.awdlPeer(f.peer,false)==0);assert(f.hal.awdlPeer(f.peer,true)==0);
    for(unsigned i=0;i<16;++i){id=f.submit(i);ids.push_back(id);statuses.push_back(f.status(id));}
    assert(f.hal.awdlPeer(f.peer,false)==0);++f.peer[5];assert(f.hal.awdlPeer(f.peer,true)==0);
    for(auto &report:statuses)mt7921_mac_add_txs(&f.sc,report.data());
    // Known unrelated conditional stale-peer TXS adaptation intentionally NOT fixed.
    assert(f.sc.sc_awdl_tx_status.rate(15)==0x4a);f.freeTokens(ids);
    assert(f.tr.counters.freeNotCurrent==16);
    puts("PASS behavior boundary: actual Rust rate ladder unchanged; known stale-peer TXS promotion still reproduced, not covertly replaced by requested-rate TX-free feedback");
}

static void exhaustionAndInvalidation(){
    for(unsigned mode=0;mode<5;++mode){
        Fixture f;auto previous=bytes(f.tr.events,sizeof(f.tr.events));
        if(mode==0)f.tr.ticketCount=UINT64_MAX;
        if(mode==1)f.tr.eventCount=UINT64_MAX;
        if(mode==2){f.tr.peerIncarnation[0]=UINT64_MAX;assert(f.hal.awdlPeer(f.peer,false)==0);assert(f.hal.awdlPeer(f.peer,true)==0);}
        if(mode==3){f.tr.txsCount=UINT64_MAX;mtk_wlan_tx_status s{};s.wcid=15;s.pid=125;f.tr.observeTxs(s,f.observer(),now);}
        if(mode==4)f.tr.counters.transmissions=UINT64_MAX-1;
        auto id=f.submit();assert(f.sc.sc_txwi.mt_inuse==1);f.freeTokens({id},0,8191);
        assert(!f.sc.sc_txwi.mt_inuse&&empty(f.tokens[id].mt_awdl_tx)&&f.publish().faults);
        if(mode<4)assert(!f.tr.counters.freeAcked);
        if(mode==0)assert(f.tr.ticketCount==UINT64_MAX&&(f.tr.faults&Trace::TicketExhausted));
        if(mode==1)assert(f.tr.eventCount==UINT64_MAX&&(f.tr.faults&Trace::EventExhausted)&&bytes(f.tr.events,sizeof(f.tr.events))==previous);
        if(mode==2)assert(f.tr.peerIncarnation[0]==UINT64_MAX&&(f.tr.faults&Trace::PeerExhausted));
        if(mode==3)assert(f.tr.txsCount==UINT64_MAX&&(f.tr.faults&Trace::TxsExhausted));
        if(mode==4)assert(f.tr.counters.transmissions==UINT64_MAX&&(f.tr.faults&Trace::CounterOverflow));
        // Once incomplete/exhausted, no new diagnostic identity is fabricated.
        auto before=f.tr.counters.freeAcked;id=f.submit(7);f.freeTokens({id});assert(f.tr.counters.freeAcked==before);
    }
    {
        Fixture f;auto id=f.submit();f.tr.live[id]++; // explicitly lost diagnostic ownership
        f.freeTokens({id});assert(!f.tr.counters.freeAcked&&!f.sc.sc_txwi.mt_inuse);
        assert(f.publish().faults&Trace::SnapshotMismatch);assert(!f.publish().outstanding);
    }
    {
        Fixture f;f.sc.sc_awdl_native_session=0;auto id=f.submit();assert(f.publish().ambiguousOutstanding==1&&!f.publish().currentOutstanding);
        f.freeTokens({id});assert(!f.tr.counters.freeAcked&&f.tr.counters.freeUnattributed==1&&f.tr.counters.reclaimUnobserved==1);
    }
    puts("PASS diagnostic fail-closed: ticket/event/TXS/incarnation exhaustion, saturating counts, invalid ownership and unknown session; real DMA/mbuf reuse still completes");
}

int main(){
    assert(mwx_awdl_native_history_init());parserAndLayouts();submittingAndCompleting();descriptorControls();failurePaths();
    wrapAndOutstanding();staleTicketAndNormalReuse();sharedPoolSafety();peerAndSessionLifetimes();quiesceAndUnobservedReclaim();
    rawTxsObserverOnly();rateAdaptationUnchanged();exhaustionAndInvalidation();assert(!liveMbufs&&!heldLocks);
    printf("PASS HAL TX diagnostic extraction + fresh Wi-Fi FFI (C++ ASan/UBSan); trace heap=%zu, per-token sidecar=%zu bytes\n",sizeof(Trace),sizeof(Trace::Token));
}
