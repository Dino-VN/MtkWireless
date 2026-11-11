#!/usr/bin/env python3
"""ASan/UBSan production UMI submission, peer removal, TXS and polling tests.

Transport and workloop boundaries are mocks, not a proof of a device fence.
History is deliberately never cleared, including between test instances.
The ordinary-unicast diagnostic trace is unavailable in this UMI fixture; the
separate test-awdl-tx-trace.py covers its enabled production hooks and exclusions.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
base = root / 'kext/Wifi/vendor'
source = (base / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()
header = (base / 'SimpleMtkWlan/hal_mwx/MtkMwx.hpp').read_text()
registers = (base / 'SimpleMtkWlan/hal_mwx/if_mwxreg.h').read_text()
interface = (base / 'AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def structure(text, name):
    start = text.index('struct ' + name + ' {')
    end = text.index('\n}', start)
    return text[start:text.index(';', end) + 1]


constants = '\n'.join(line for line in registers.splitlines()
                      if re.match(r'#define\s+(?:MWX_MAX_INTERFACES|MWX_WTBL_SIZE|'
                                  r'MWX_WTBL_RESERVED|MWX_WTBL_STA|MCU_UNI_CMD_STA_REC_UPDATE|'
                                  r'STA_REC_BASIC|STA_REC_WTBL|WTBL_RESET_AND_SET|WTBL_GENERIC|'
                                  r'WTBL_RX|WTBL_HDR_TRANS|STA_TYPE_STA|NETWORK_INFRA|'
                                  r'CONN_STATE_PORT_SECURE|CONN_STATE_DISCONNECT)\s', line))
wire = '\n'.join(structure(registers, name) for name in
                 ['sta_req_hdr', 'sta_rec_basic', 'sta_rec_wtbl', 'wtbl_generic',
                  'wtbl_rx', 'wtbl_hdr_trans'])
start = source.index('class MwxAwdlCall {')
operations = source[start:source.index('\nstatic bool mwx_awdl_current(', start)]
peer_update = function(source, 'IOReturn MtkMwx::awdlPeer(')
start = source.index('// Native UMI containment,')
bookkeeping = source[start:source.index('\nenum mwx_hw_type', start)]
start = header.index('    struct AwdlTxResult {')
result = header[start:header.index('    };', start) + 6]
start = source.index('\tconst uint8_t *raw=', source.index('void\nmt7921_mac_add_txs('))
status_arm = source[start:source.index('\tstruct ieee80211com *ic =', start)]
# Integration invariants beyond the extracted portions.
assert 'awdlSetTxCompletion' not in source + header + interface
assert 'sc_awdl_native_tx.cancel();' in function(source, 'static void mwx_awdl_quiesce(mwx_softc *sc,bool drainCalls)')
assert 'sc_awdl_native_tx.cancel(wcid);' in function(source, 'IOReturn MtkMwx::awdlPeer(')
assert 'case MCU_EVENT_TX_DONE:\n\t\tmt7921_mcu_tx_done_event(sc, m);' in source
assert 'mt7921_mac_add_txs(sc, &data[2 + 8 * i]);' in source
assert 'txSession=enabled ? hal->awdlTxSession() : 0;' in interface
assert 'if(delay>2000) delay=2000;' in interface
assert 'memset(mwx_awdl_native_issued' not in source
assert 'sc_awdl_native_tx' not in function(source, 'void\nmwx_mac_tx_free(')

code = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/types.h>
#include <AWDL/AwdlTxTrace.hpp>
using IOReturn=unsigned;
constexpr unsigned kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBadArgument=2,
    kIOReturnNoResources=3,kIOReturnNotFound=4,kIOReturnNoMemory=5,
    kIOReturnIOError=6,kIOReturnAborted=7;
struct IOLock {std::mutex mutex;};
bool failLock=false;
thread_local std::vector<IOLock *> heldLocks;
void (*lockAcquired)(IOLock *)=nullptr,(*lockReleasing)(IOLock *)=nullptr;
bool IOLockHeld(IOLock *l){for(auto held:heldLocks)if(held==l)return true;return false;}
IOLock *IOLockAlloc(){return failLock?nullptr:new IOLock;}
void IOLockFree(IOLock *l){assert(!IOLockHeld(l));delete l;}
void IOLockLock(IOLock *l){
    assert(l && !IOLockHeld(l));l->mutex.lock();heldLocks.push_back(l);
    if(lockAcquired)lockAcquired(l);
}
void IOLockUnlock(IOLock *l){
    assert(!heldLocks.empty() && heldLocks.back()==l);
    if(lockReleasing)lockReleasing(l);
    heldLocks.pop_back();l->mutex.unlock();
}
struct mbuf {std::vector<uint8_t> bytes;size_t offset=0;};using mbuf_t=mbuf *;
unsigned live=0, dmaOwned=0, dmaReclaimed=0;
size_t mbuf_len(mbuf *m){return m->bytes.size()-m->offset;}
size_t mbuf_pkthdr_len(mbuf *m){return mbuf_len(m);}
void *mbuf_data(mbuf *m){return m->bytes.data()+m->offset;}
void mbuf_freem(mbuf *m){assert(m && live);--live;delete m;}
void mbuf_setlen(mbuf *m,size_t n){assert(m->offset+n<=m->bytes.capacity());m->bytes.resize(m->offset+n);}
void mbuf_pkthdr_setlen(mbuf *m,size_t n){assert(n==mbuf_len(m));}
int m_trailingspace(mbuf *m){return int(m->bytes.capacity()-m->bytes.size());}
size_t m_leadingspace(mbuf *m){return m->offset;}
#define MBUF_DONTWAIT 0
#define M_DONTWAIT 0
#define IFF_RUNNING 1
#define IEEE80211_ADDR_LEN 6
#define KASSERT(x) assert(x)
#define MWX_DEV_LOG(...) ((void)0)
#define htole16(x) uint16_t(x)
#define htole32(x) uint32_t(x)
#define le16toh(x) uint16_t(x)
#define le32toh(x) uint32_t(x)
#define __packed __attribute__((packed))
#define mtod(m,t) reinterpret_cast<t>(mbuf_data(m))
''' + constants + '\n' + wire + r'''
int mbuf_dup(mbuf *m,int,mbuf **out){*out=new mbuf(*m);++live;return 0;}
namespace mtkawdl {
const uint8_t bssid[6]={0,1,2,3,4,5};
bool unicast(const uint8_t *address){return !(address[0]&1) && memcmp(address,"\0\0\0\0\0\0",6);}
}
struct mwx_softc;
class MtkMwx {public:
''' + result + r'''
    mwx_softc *com;
    IOReturn awdlTryTransmit(mbuf_t *,uint8_t);
    bool awdlCanTransmit(uint8_t){return true;}
    bool awdlTakeTxResult(AwdlTxResult &);
    uint64_t awdlTxSession();
    IOReturn awdlPeer(const uint8_t *,bool);
};
''' + bookkeeping + r'''
uint64_t now=100;
uint64_t mwx_awdl_now(){return now;}
struct ieee80211_node {uint8_t ni_macaddr[6];void *ni_chan;};
struct mwx_node {ieee80211_node ni;uint16_t wcid,flags;};
struct mwx_vif {mwx_node vif_mn;uint8_t idx=2,omac_idx=3;};
struct mwx_softc {
    IOLock *sc_awdl_lock=IOLockAlloc();
    mwx_awdl_native_tx_table sc_awdl_native_tx{};
    mtkawdl::HalTxTrace *sc_awdl_tx_trace=nullptr;
    uint64_t sc_awdl_native_session=0;
    bool sc_awdl_enabled=true,sc_awdl_stopping=false,sc_resetting=false,sc_detaching=false;
    bool sc_awdl_busy=false;unsigned sc_awdl_calls=0;
    uint32_t sc_awdl_epoch=1;
    uint8_t sc_awdl_address[6]={2,3,4,5,6,7};
    mwx_vif sc_awdl_vif{};
    mwx_node sc_awdl_peers[3]{};bool sc_awdl_peer_used[3]={true,false,false};
    bool sc_awdl_station_pending[3]={true,false,false};
    struct {char ic_channels[256];struct {unsigned if_flags=IFF_RUNNING;} ic_if;uint64_t ic_awdl_data_at=0;} sc_ic;
    struct {uint32_t rates[3]={0x12345678,0,0};uint64_t dataAcked=0,dataFailed=0;
        bool observe(const uint8_t *p,size_t){uint32_t v;memcpy(&v,p+8,4);return (v>>16)>=15 && (v>>16)<=18;}} sc_awdl_tx_status;
    mbuf *sc_awdl_native_tx_frame=nullptr;uint8_t sc_awdl_native_tx_pid=0;
    bool sc_awdl_native_tx_override=false;
    unsigned sc_awdl_tx_bad_frame=0,sc_awdl_tx_busy=0,sc_awdl_tx_no_grant=0,
        sc_awdl_tx_no_peer=0,sc_awdl_tx_backpressure=0,sc_awdl_tx_copy_failed=0,
        sc_awdl_tx_dma_failed=0,sc_awdl_tx_submitted=0;int sc_awdl_tx_errno=0;
    mwx_softc(){sc_awdl_native_session=mwx_awdl_native_new_session();
        sc_awdl_peers[0].wcid=15;sc_awdl_peers[0].flags=0x41;uint8_t peer[6]={2,9,8,7,6,5};
        memcpy(sc_awdl_peers[0].ni.ni_macaddr,peer,6);}
    ~mwx_softc(){assert(!sc_awdl_busy && !sc_awdl_calls);IOLockFree(sc_awdl_lock);}
};
static bool mwx_awdl_current(mwx_softc *,uint32_t);
// Model only the production trace-unavailable early return. Do not pretend
// that this fixture validates the enabled parser/diagnostic path.
static void mwx_awdl_tx_trace_txs(mwx_softc *sc,const uint8_t *,size_t){
    assert(!sc->sc_awdl_tx_trace);
}
static uint64_t mtk_join_now(){return 1;}
static void mwx_awdl_reset_rate(mwx_softc *sc,unsigned index){sc->sc_awdl_tx_status.rates[index]=0;}
''' + operations + function(source, 'static bool mwx_awdl_current(mwx_softc *sc,') + r'''
bool mwx_tx_full(mwx_softc *){return false;}
void mt7921_mac_add_txs(mwx_softc *sc,const uint32_t *txs_data){
''' + status_arm + r'''
}
''' + function(source, 'void\nmt7921_mcu_tx_done_event(') + r'''
void txs(mwx_softc &sc,const MtkMwx::AwdlTxResult &r,bool failed=false,bool mcu=false,unsigned format=0){
    uint32_t words[8]{};words[0]=(failed?0x10000:0)|(format<<23);
    words[1]=uint32_t(r.sequence)<<20;words[2]=uint32_t(r.wcid)<<16;words[3]=uint32_t(r.pid)<<24;
    if(mcu){mbuf m;m.bytes.resize(88);memcpy(m.bytes.data()+28,words,28);mt7921_mcu_tx_done_event(&sc,&m);}
    else mt7921_mac_add_txs(&sc,words);
}
int txError=0;bool synchronousTXS=false;
int mwx_tx(mwx_softc *sc,mbuf *m,ieee80211_node *){
    assert(sc->sc_awdl_native_tx_override && sc->sc_awdl_native_tx_frame==m);
    if(synchronousTXS){for(auto &p:sc->sc_awdl_native_tx.entries)
        if(p.submitting){txs(*sc,p.result);break;}}
    // Transport consumes attempts; DMA resource accounting is separate from
    // the UMI table, so timeout/cancel must not change dmaOwned.
    if(!txError)++dmaOwned;
    mbuf_freem(m);return txError;
}
// The complete production awdlPeer and TLV builders run below. Only mbuf
// allocation/prepend, MCU transport/reply and lifecycle interleavings are
// injected. The production operation/current-epoch guards are also extracted.
enum class PeerFault { None, Allocate, Prepend, Transport, Wait, Status };
enum class Invalidate { None, SendEpoch, WaitEpoch, CommitEpoch, CommitStopping, SendSession, WaitSession };
struct PeerBoundary {
    mwx_softc *sc=nullptr;PeerFault fault=PeerFault::None;Invalidate invalidate=Invalidate::None;
    uint32_t status=0,epoch=0;uint64_t canceled=0;
    unsigned allocations=0,prepends=0,sends=0,waits=0,postWaitLocks=0,commits=0;
    bool waitSucceeded=false,commitAttempted=false;
    std::vector<std::vector<uint8_t>> commands;
} peerBoundary;
void peerBoundaryUnlocked(mwx_softc *sc){
    assert(peerBoundary.sc==sc && sc->sc_awdl_busy && sc->sc_awdl_calls);
    assert(!IOLockHeld(sc->sc_awdl_lock));
    // Failed attempts may not silently drop either Active or Terminal entries
    // before any allocation/send/wait boundary, even if no TXS follows.
    assert(sc->sc_awdl_native_tx.canceled==peerBoundary.canceled);
}
void invalidatePeer(mwx_softc *sc,bool session){
    IOLockLock(sc->sc_awdl_lock);++sc->sc_awdl_epoch;
    if(session){sc->sc_awdl_enabled=false;sc->sc_awdl_stopping=true;sc->sc_awdl_native_session=0;}
    IOLockUnlock(sc->sc_awdl_lock);
}
void peerLockAcquired(IOLock *lock){
    auto &b=peerBoundary;auto sc=b.sc;
    if(!sc || lock!=sc->sc_awdl_lock || !sc->sc_awdl_busy || !b.waitSucceeded ||
       b.status || sc->sc_awdl_epoch!=b.epoch || sc->sc_awdl_stopping)return;
    // After a successful wait, operation.valid() takes the first lock and the
    // update's final epoch/stopping guard takes the second. Mutate only at
    // that second acquisition to exercise the actual guarded success branch.
    if(++b.postWaitLocks==2){
        b.commitAttempted=true;
        if(b.invalidate==Invalidate::CommitEpoch)++sc->sc_awdl_epoch;
        if(b.invalidate==Invalidate::CommitStopping)sc->sc_awdl_stopping=true;
    }
}
void peerLockReleasing(IOLock *lock){
    auto &b=peerBoundary;auto sc=b.sc;
    if(!sc || lock!=sc->sc_awdl_lock)return;
    if(sc->sc_awdl_native_tx.canceled!=b.canceled){
        assert(b.commitAttempted && b.waitSucceeded && !b.status && b.sends==1 && b.waits==1);
        assert(sc->sc_awdl_epoch==b.epoch && !sc->sc_awdl_stopping && sc->sc_awdl_busy);
        b.canceled=sc->sc_awdl_native_tx.canceled;
    }
    if(b.commitAttempted && b.postWaitLocks==2 && sc->sc_awdl_epoch==b.epoch && !sc->sc_awdl_stopping)
        ++b.commits;
}
void beginPeerBoundary(mwx_softc &sc,PeerFault fault=PeerFault::None,uint32_t status=0,
                       Invalidate invalidate=Invalidate::None){
    assert(!peerBoundary.sc && !sc.sc_awdl_busy && !sc.sc_awdl_calls && !live);
    peerBoundary={};peerBoundary.sc=&sc;peerBoundary.fault=fault;peerBoundary.status=status;
    peerBoundary.invalidate=invalidate;peerBoundary.epoch=sc.sc_awdl_epoch;
    peerBoundary.canceled=sc.sc_awdl_native_tx.canceled;
    lockAcquired=peerLockAcquired;lockReleasing=peerLockReleasing;
}
void endPeerBoundary(){
    assert(peerBoundary.sc && !peerBoundary.sc->sc_awdl_busy && !peerBoundary.sc->sc_awdl_calls);
    assert(!live && heldLocks.empty());peerBoundary.sc=nullptr;
    lockAcquired=lockReleasing=nullptr;
}
mbuf *mwx_alloc_sta_req_tlv(int len){
    peerBoundaryUnlocked(peerBoundary.sc);++peerBoundary.allocations;
    assert(len==int(sizeof(sta_req_hdr)));
    if(peerBoundary.fault==PeerFault::Allocate)return nullptr;
    auto m=new mbuf;++live;m->bytes.reserve(512);m->offset=64+len;m->bytes.resize(m->offset);return m;
}
mbuf *m_prepend(mbuf *m,size_t len,int){
    peerBoundaryUnlocked(peerBoundary.sc);++peerBoundary.prepends;
    if(peerBoundary.fault==PeerFault::Prepend){mbuf_freem(m);return nullptr;}
    assert(m->offset>=len);m->offset-=len;return m;
}
''' + function(source, 'void *\nmwx_append_len(') + \
    function(source, 'void *\nmwx_append_tlv(') + \
    function(source, 'struct mbuf *\nmwx_fill_sta_req_hdr(') + r'''
int mwx_awdl_remove_station(mwx_softc *,uint32_t,unsigned){
    // No test enters cleanup of a previously pending failed add. Keep this
    // unrelated firmware boundary explicit rather than fabricating success.
    assert(false);return EIO;
}
int mwx_mcu_send_mbuf(mwx_softc *sc,uint32_t cmd,mbuf *m,int *seq){
    peerBoundaryUnlocked(sc);assert(cmd==MCU_UNI_CMD_STA_REC_UPDATE && seq);
    ++peerBoundary.sends;*seq=23;
    peerBoundary.commands.emplace_back(mtod(m,const uint8_t *),mtod(m,const uint8_t *)+mbuf_len(m));
    mbuf_freem(m); // Transport consumes mbufs on both success and error.
    if(peerBoundary.invalidate==Invalidate::SendEpoch)invalidatePeer(sc,false);
    if(peerBoundary.invalidate==Invalidate::SendSession)invalidatePeer(sc,true);
    return peerBoundary.fault==PeerFault::Transport?ENOBUFS:0;
}
int mwx_mcu_wait_resp_int(mwx_softc *sc,uint32_t cmd,int seq,uint32_t *status){
    peerBoundaryUnlocked(sc);assert(cmd==MCU_UNI_CMD_STA_REC_UPDATE && seq==23 && status);
    ++peerBoundary.waits;*status=peerBoundary.status;
    if(peerBoundary.invalidate==Invalidate::WaitEpoch)invalidatePeer(sc,false);
    if(peerBoundary.invalidate==Invalidate::WaitSession)invalidatePeer(sc,true);
    peerBoundary.waitSucceeded=peerBoundary.fault!=PeerFault::Wait;
    return peerBoundary.waitSucceeded?0:EWOULDBLOCK;
}
''' + peer_update + function(source, 'IOReturn MtkMwx::awdlTryTransmit(') + \
    function(source, 'uint64_t MtkMwx::awdlTxSession(') + \
    function(source, 'bool MtkMwx::awdlTakeTxResult(') + r'''
struct AirportMtkAwdlInterface {
    MtkMwx *hal;void *owner=nullptr;bool enabled=true,stopping=false;
    uint64_t txSession,txStatusCompletions=0,txStatusDiscarded=0;
    void setProperty(const char *,uint64_t,unsigned){}
    void drainTxStatus();
};
unsigned ackMessages=0,failedMessages=0;bool cancelOnMessage=false;uint8_t lastMessage[7]{};
IOReturn awdlControllerMessage(void *,AirportMtkAwdlInterface *d,unsigned message,const uint8_t *p,size_t n,bool){
    assert(message==0x45 && n==7);memcpy(lastMessage,p,7);if(p[6])++failedMessages;else ++ackMessages;
    if(cancelOnMessage){d->enabled=false;d->txSession=0;}
    return 0;
}
''' + function(interface, 'void AirportMtkAwdlInterface::drainTxStatus(') + r'''
mbuf *frame(mwx_softc &sc,uint16_t sequence,unsigned peer=0){
    auto m=new mbuf;m->bytes.resize(32);++live;m->bytes[0]=0xd0;m->bytes[24]=127;
    assert(peer<3);memcpy(m->bytes.data()+4,sc.sc_awdl_peers[peer].ni.ni_macaddr,6);
    memcpy(m->bytes.data()+10,sc.sc_awdl_address,6);memcpy(m->bytes.data()+16,mtkawdl::bssid,6);
    uint16_t seq=sequence<<4;memcpy(m->bytes.data()+22,&seq,2);return m;
}
MtkMwx::AwdlTxResult submit(MtkMwx &h,uint16_t seq,unsigned peer=0){
    auto m=frame(*h.com,seq,peer);assert(h.awdlTryTransmit(&m,6)==0 && !m);
    for(auto &p:h.com->sc_awdl_native_tx.entries)if(p.state && p.result.sequence==seq &&
        p.result.wcid==h.com->sc_awdl_peers[peer].wcid)return p.result;
    assert(false);return {};
}
const mwx_awdl_native_tx_pending &pending(const mwx_softc &sc,const MtkMwx::AwdlTxResult &r){
    for(const auto &p:sc.sc_awdl_native_tx.entries)if(p.state && p.result.wcid==r.wcid &&
        p.result.sequence==r.sequence && p.result.pid==r.pid && p.result.session==r.session)return p;
    assert(false);return sc.sc_awdl_native_tx.entries[0];
}
void assertIdentity(const MtkMwx::AwdlTxResult &a,const MtkMwx::AwdlTxResult &b){
    assert(a.wcid==b.wcid && a.sequence==b.sequence && a.pid==b.pid && a.session==b.session);
    assert(!memcmp(a.address,b.address,6));
}
void assertUnchanged(const mwx_softc &sc,const mwx_awdl_native_tx_table &before){
    const auto &t=sc.sc_awdl_native_tx;
    assert(t.serial==before.serial && t.canceled==before.canceled && t.timeouts==before.timeouts);
    assert(t.stale==before.stale && t.exhausted==before.exhausted && t.backpressure==before.backpressure &&
           t.lastReason==before.lastReason);
    for(unsigned i=0;i<32;++i){const auto &a=t.entries[i],&b=before.entries[i];
        assert(a.state==b.state && a.submitting==b.submitting && a.identity==b.identity && a.issuedAt==b.issuedAt);
        assertIdentity(a.result,b.result);assert(a.result.failed==b.result.failed && a.result.reason==b.result.reason);
    }
}
void assertPeerUnchanged(const mwx_softc &sc,const mwx_node &before,uint32_t rate){
    const auto &node=sc.sc_awdl_peers[0];
    assert(sc.sc_awdl_peer_used[0] && sc.sc_awdl_station_pending[0]);
    assert(node.wcid==before.wcid && node.flags==before.flags && node.ni.ni_chan==before.ni.ni_chan);
    assert(!memcmp(node.ni.ni_macaddr,before.ni.ni_macaddr,6) && sc.sc_awdl_tx_status.rates[0]==rate);
}
void assertPeerCommand(const mwx_softc &sc,const uint8_t *address,bool add){
    assert(peerBoundary.commands.size()==1);const auto &bytes=peerBoundary.commands[0];
    auto hdr=reinterpret_cast<const sta_req_hdr *>(bytes.data());
    auto basic=reinterpret_cast<const sta_rec_basic *>(bytes.data()+sizeof(*hdr));
    auto wtbl=reinterpret_cast<const sta_rec_wtbl *>(reinterpret_cast<const uint8_t *>(basic)+sizeof(*basic));
    size_t wlen=sizeof(*wtbl)+(add?sizeof(wtbl_generic)+sizeof(wtbl_rx)+sizeof(wtbl_hdr_trans):0);
    assert(bytes.size()==sizeof(*hdr)+sizeof(*basic)+wlen);
    assert(hdr->bss_idx==sc.sc_awdl_vif.idx && hdr->muar_idx==sc.sc_awdl_vif.omac_idx &&
           hdr->wlan_idx_lo==15 && !hdr->wlan_idx_hi && hdr->tlv_num==2 && hdr->is_tlv_append==1);
    assert(basic->tag==STA_REC_BASIC && basic->len==sizeof(*basic) && !memcmp(basic->peer_addr,address,6));
    assert(basic->conn_type==(STA_TYPE_STA|NETWORK_INFRA) &&
           basic->conn_state==(add?CONN_STATE_PORT_SECURE:CONN_STATE_DISCONNECT));
    assert(basic->extra_info==(EXTRA_INFO_VER|(add?EXTRA_INFO_NEW:0)));
    assert(wtbl->tag==STA_REC_WTBL && wtbl->len==wlen && wtbl->operation==WTBL_RESET_AND_SET &&
           wtbl->wlan_idx_lo==15 && wtbl->tlv_num==(add?3:0));
    if(add){auto generic=reinterpret_cast<const wtbl_generic *>(reinterpret_cast<const uint8_t *>(wtbl)+sizeof(*wtbl));
        auto rx=reinterpret_cast<const wtbl_rx *>(reinterpret_cast<const uint8_t *>(generic)+sizeof(*generic));
        auto trans=reinterpret_cast<const wtbl_hdr_trans *>(reinterpret_cast<const uint8_t *>(rx)+sizeof(*rx));
        assert(generic->tag==WTBL_GENERIC && !memcmp(generic->peer_addr,address,6) && generic->muar_idx==hdr->muar_idx);
        assert(rx->tag==WTBL_RX && rx->rca1==1 && rx->rca2==1 && rx->rv==1);
        assert(trans->tag==WTBL_HDR_TRANS && trans->no_rx_trans==1);
    }
}
void pollGenuinePair(MtkMwx &h,AirportMtkAwdlInterface &d,const MtkMwx::AwdlTxResult &active,
                     const MtkMwx::AwdlTxResult &acked,bool mcu){
    auto before=ackMessages,failures=failedMessages;MtkMwx::AwdlTxResult result{};
    assert(pending(*h.com,active).state==mwx_awdl_native_tx_pending::Active);
    const auto &terminal=pending(*h.com,acked);
    assert(terminal.state==mwx_awdl_native_tx_pending::Terminal && !terminal.result.failed && terminal.result.reason==1);
    d.drainTxStatus();assert(ackMessages==before+1 && failedMessages==failures && d.txStatusCompletions==1);
    assert(!memcmp(lastMessage,acked.address,6) && !lastMessage[6] && !h.awdlTakeTxResult(result));
    txs(*h.com,active,false,mcu);txs(*h.com,active,false,!mcu);d.drainTxStatus();
    assert(ackMessages==before+2 && failedMessages==failures && d.txStatusCompletions==2 && !d.txStatusDiscarded);
    assert(!memcmp(lastMessage,active.address,6) && !lastMessage[6]);
    txs(*h.com,acked,false,mcu);d.drainTxStatus();assert(ackMessages==before+2 && !h.awdlTakeTxResult(result));
}
void peerRemovalRegression(){
    uint16_t sequence=600;unsigned failures=0,epochAborts=0,sessionAborts=0;
    auto failedRemoval=[&](PeerFault fault,uint32_t status,Invalidate invalidate){
        mwx_softc sc;MtkMwx h{&sc};AirportMtkAwdlInterface d{&h,nullptr,true,false,h.awdlTxSession()};
        auto active=submit(h,sequence++),acked=submit(h,sequence++);txs(sc,acked,false,true);
        const auto table=sc.sc_awdl_native_tx;const auto node=sc.sc_awdl_peers[0];
        const uint32_t rate=sc.sc_awdl_tx_status.rates[0];const uint64_t session=h.awdlTxSession();
        const unsigned dma=dmaOwned;
        beginPeerBoundary(sc,fault,status,invalidate);
        IOReturn expected=invalidate!=Invalidate::None?kIOReturnAborted:
            (fault==PeerFault::Allocate?kIOReturnNoMemory:kIOReturnIOError);
        assert(h.awdlPeer(sc.sc_awdl_peers[0].ni.ni_macaddr,false)==expected);
        assert(peerBoundary.allocations==1 && peerBoundary.prepends==unsigned(fault!=PeerFault::Allocate));
        unsigned sends=unsigned(fault!=PeerFault::Allocate && fault!=PeerFault::Prepend);
        assert(peerBoundary.sends==sends && peerBoundary.waits==unsigned(sends && fault!=PeerFault::Transport &&
               invalidate!=Invalidate::SendEpoch && invalidate!=Invalidate::SendSession));
        assert(!peerBoundary.commits);if(sends)assertPeerCommand(sc,node.ni.ni_macaddr,false);
        endPeerBoundary();assertUnchanged(sc,table);assertPeerUnchanged(sc,node,rate);assert(dmaOwned==dma);
        assert(h.awdlTxSession()==session);pollGenuinePair(h,d,active,acked,false);
    };
    for(auto fault:{PeerFault::Allocate,PeerFault::Prepend,PeerFault::Transport,PeerFault::Wait}){
        failedRemoval(fault,0,Invalidate::None);++failures;
    }
    for(uint32_t status:{1u,9u,UINT32_MAX}){failedRemoval(PeerFault::Status,status,Invalidate::None);++failures;}
    for(auto invalidation:{Invalidate::SendEpoch,Invalidate::WaitEpoch,Invalidate::CommitEpoch}){
        failedRemoval(PeerFault::None,0,invalidation);++epochAborts;
    }
    // Successful firmware removal cancels both an Active attempt and a genuine
    // unpolled ACK, under the metadata lock. A different WCID remains intact.
    {mwx_softc sc;MtkMwx h{&sc};AirportMtkAwdlInterface d{&h,nullptr,true,false,h.awdlTxSession()};
     auto active=submit(h,sequence++),acked=submit(h,sequence++);txs(sc,acked,false,true);
     sc.sc_awdl_peers[1]=sc.sc_awdl_peers[0];sc.sc_awdl_peers[1].wcid=16;sc.sc_awdl_peers[1].ni.ni_macaddr[5]^=0x80;
     sc.sc_awdl_peer_used[1]=sc.sc_awdl_station_pending[1]=true;
     auto other=submit(h,sequence++,1);txs(sc,other);const auto otherBefore=pending(sc,other);
     uint64_t session=h.awdlTxSession(),canceled=sc.sc_awdl_native_tx.canceled;unsigned dma=dmaOwned;
     uint8_t replacementAddress[6];memcpy(replacementAddress,active.address,6);replacementAddress[5]^=0x40;
     beginPeerBoundary(sc);assert(h.awdlPeer(sc.sc_awdl_peers[0].ni.ni_macaddr,false)==kIOReturnSuccess);
     assert(peerBoundary.commits==1 && peerBoundary.sends==1 && peerBoundary.waits==1);
     assertPeerCommand(sc,active.address,false);endPeerBoundary();
     assert(sc.sc_awdl_native_tx.canceled==canceled+2 && dmaOwned==dma && h.awdlTxSession()==session);
     assert(!sc.sc_awdl_peer_used[0] && !sc.sc_awdl_station_pending[0] && !sc.sc_awdl_tx_status.rates[0]);
     const auto &node=sc.sc_awdl_peers[0];assert(node.wcid==15 && !node.flags && !node.ni.ni_chan);
     const uint8_t zero[6]{};assert(!memcmp(node.ni.ni_macaddr,zero,6));
     assert(pending(sc,other).identity==otherBefore.identity && pending(sc,other).state==otherBefore.state);
     unsigned before=ackMessages,failed=failedMessages;
     txs(sc,active);txs(sc,active,false,true);txs(sc,acked);txs(sc,acked,false,true);d.drainTxStatus();
     assert(ackMessages==before+1 && failedMessages==failed && d.txStatusCompletions==1 && !memcmp(lastMessage,other.address,6));
     beginPeerBoundary(sc);assert(h.awdlPeer(replacementAddress,true)==kIOReturnSuccess);
     assert(peerBoundary.commits==1);assertPeerCommand(sc,replacementAddress,true);endPeerBoundary();
     assert(sc.sc_awdl_peer_used[0] && sc.sc_awdl_station_pending[0] && h.awdlTxSession()==session);
     auto replacement=submit(h,active.sequence);assert(replacement.wcid==active.wcid && replacement.pid!=active.pid);
     assert(!memcmp(replacement.address,replacementAddress,6));
     txs(sc,active);txs(sc,active,false,true);txs(sc,acked);txs(sc,acked,false,true);
     MtkMwx::AwdlTxResult result{};assert(!h.awdlTakeTxResult(result));
     txs(sc,replacement,false,true);assert(h.awdlTakeTxResult(result));assertIdentity(result,replacement);
     assert(!result.failed && result.reason==1 && !h.awdlTakeTxResult(result));}
    // Session invalidation is not an ordinary same-session removal failure.
    // Admission closes during send/wait or at the final stopping guard. Old
    // genuine ACKs remain metadata until polled, but must never be posted to
    // an obsolete/new native session. This models closure, not a device fence.
    for(auto invalidation:{Invalidate::SendSession,Invalidate::WaitSession,Invalidate::CommitStopping}){
        mwx_softc sc;MtkMwx h{&sc};AirportMtkAwdlInterface d{&h,nullptr,true,false,h.awdlTxSession()};
        auto active=submit(h,sequence++),acked=submit(h,sequence++);txs(sc,acked,false,true);
        const auto table=sc.sc_awdl_native_tx;const auto node=sc.sc_awdl_peers[0];
        uint32_t rate=sc.sc_awdl_tx_status.rates[0];uint64_t oldSession=h.awdlTxSession();
        unsigned before=ackMessages,failed=failedMessages,dma=dmaOwned;
        beginPeerBoundary(sc,PeerFault::None,0,invalidation);
        assert(h.awdlPeer(sc.sc_awdl_peers[0].ni.ni_macaddr,false)==kIOReturnAborted && !peerBoundary.commits);
        assertPeerCommand(sc,node.ni.ni_macaddr,false);endPeerBoundary();
        assertUnchanged(sc,table);assertPeerUnchanged(sc,node,rate);assert(!h.awdlTxSession() && dmaOwned==dma);
        d.drainTxStatus();assert(d.txStatusDiscarded==1 && !d.txStatusCompletions);
        txs(sc,active);d.drainTxStatus();assert(d.txStatusDiscarded==2 && ackMessages==before && failedMessages==failed);
        IOLockLock(sc.sc_awdl_lock);++sc.sc_awdl_epoch;sc.sc_awdl_stopping=false;sc.sc_awdl_enabled=true;
        sc.sc_awdl_native_session=mwx_awdl_native_new_session();IOLockUnlock(sc.sc_awdl_lock);
        d.txSession=h.awdlTxSession();assert(d.txSession && d.txSession!=oldSession);
        auto replacement=submit(h,active.sequence);assert(replacement.pid!=active.pid);
        txs(sc,active);txs(sc,active,false,true);d.drainTxStatus();assert(ackMessages==before);
        txs(sc,replacement,false,true);d.drainTxStatus();assert(ackMessages==before+1 && failedMessages==failed);
        ++sessionAborts;
    }
    assert(failures==7 && epochAborts==3 && sessionAborts==3 && !live && !dmaReclaimed);
    std::puts("native UMI peer removal production extraction: PASS (7 failures, scoped success/replacement, 3 epoch guards, 3 session closures)");
}
int main(){
    // Unavailable global lock fails closed. Concurrent attach publishes only
    // one permanent lock; no losing initializer clears existing history.
    failLock=true;assert(!mwx_awdl_native_history_init());
    {mwx_awdl_native_tx_table t{};uint64_t id;uint8_t pid;uint8_t a[6]{};
     assert(t.reserve(a,15,1,1,now,id,pid)<0 && t.lastReason==7);}
    failLock=false;
    std::thread a([]{assert(mwx_awdl_native_history_init());});
    std::thread b([]{assert(mwx_awdl_native_history_init());});a.join();b.join();
    assert(sizeof(mwx_awdl_native_issued)==48*1024);
    // Independent device tables must share tuple burning atomically.
    {mwx_awdl_native_tx_table left{},right{};uint8_t address[6]{};
     auto reserve=[&](mwx_awdl_native_tx_table &table){for(unsigned i=0;i<16;++i){
         uint64_t id=0;uint8_t pid=0;assert(table.reserve(address,17,2048,1,now,id,pid)>=0);}};
     std::thread l([&]{reserve(left);}),r([&]{reserve(right);});l.join();r.join();
     uint32_t seen=0;for(auto *table:{&left,&right})for(auto &p:table->entries)if(p.state){
         uint32_t bit=uint32_t(1)<<(p.result.pid-80);assert(!(seen&bit));seen|=bit;}
     assert(seen==UINT32_MAX);}
    mwx_softc sc;MtkMwx h{&sc};AirportMtkAwdlInterface d{&h,nullptr,true,false,h.awdlTxSession()};
    // 32 missing TXS -> 32 local failures -> slots available again; no ACK.
    std::vector<MtkMwx::AwdlTxResult> old;
    for(unsigned i=0;i<32;++i)old.push_back(submit(h,uint16_t(i)));
    auto full=frame(sc,90);assert(h.awdlTryTransmit(&full,6)==kIOReturnNoResources && full);mbuf_freem(full);
    now+=1000000;d.drainTxStatus();assert(d.txStatusCompletions==32 && !ackMessages && failedMessages==32);
    assert(sc.sc_awdl_native_tx.timeouts==32 && dmaOwned==32 && !dmaReclaimed);
    auto current=submit(h,0);assert(current.pid!=old[0].pid);
    txs(sc,old[0]);txs(sc,old[0],false,true);d.drainTxStatus();assert(d.txStatusCompletions==32);
    txs(sc,current,false,false,2);d.drainTxStatus();assert(d.txStatusCompletions==32);
    txs(sc,current);txs(sc,current);d.drainTxStatus();assert(ackMessages==1);
    // Positive and negative real statuses are idempotent across both routes.
    current=submit(h,100);txs(sc,current,true,true);txs(sc,current,false);d.drainTxStatus();
    assert(failedMessages==33 && ackMessages==1);
    // Every PID for a fixed WCID/sequence is permanently burned, including
    // rollback. Sequence wrap changes nothing and history survives reattach.
    for(unsigned i=0;i<32;++i){current=submit(h,4095);txs(sc,current);d.drainTxStatus();}
    full=frame(sc,4095);assert(h.awdlTryTransmit(&full,6)==kIOReturnNoResources);mbuf_freem(full);
    assert(sc.sc_awdl_native_tx.lastReason==5);
    {mwx_softc reattached;MtkMwx h2{&reattached};auto m=frame(reattached,4095);
     assert(h2.awdlTryTransmit(&m,6)==kIOReturnNoResources);mbuf_freem(m);}
    // Peer removal and same-WCID replacement cannot let old statuses complete
    // the new peer, even with an identical sequence.
    old[0]=submit(h,101);sc.sc_awdl_native_tx.cancel(15);
    sc.sc_awdl_peers[0].ni.ni_macaddr[5]++;
    current=submit(h,101);txs(sc,old[0]);txs(sc,old[0],false,true);
    MtkMwx::AwdlTxResult result{};assert(!h.awdlTakeTxResult(result));
    txs(sc,current);assert(h.awdlTakeTxResult(result) && !result.failed);
    // A TXS received while submission is in progress cannot be consumed until
    // the identity-checked commit; error rollback discards it without ACK.
    synchronousTXS=true;txError=1;auto m=frame(sc,102);
    assert(h.awdlTryTransmit(&m,6)==kIOReturnNoResources && m);mbuf_freem(m);
    assert(!h.awdlTakeTxResult(result));txError=0;
    current=submit(h,102);assert(h.awdlTakeTxResult(result) && !result.failed);synchronousTXS=false;
    // Stale rollback identity must not touch a replacement slot. Submitting
    // records never expire, and their early TXS terminal is not pollable.
    auto &t=sc.sc_awdl_native_tx;uint64_t id=0;uint8_t pid=0;
    int slot=t.reserve(sc.sc_awdl_address,16,200,sc.sc_awdl_native_session,now,id,pid);assert(slot>=0);
    uint64_t oldId=id;t.cancel(16);
    int replacement=t.reserve(sc.sc_awdl_address,16,200,sc.sc_awdl_native_session,now,id,pid);assert(replacement==slot);
    t.finish(slot,oldId,false);assert(t.entries[slot].identity==id && t.entries[slot].submitting);
    t.expire(now+9000000);assert(t.entries[slot].state==mwx_awdl_native_tx_pending::Submitting);
    t.match(16,200,pid,false);assert(!t.take(result));t.finish(slot,id,true);assert(t.take(result));
    // Terminal backpressure is bounded by the same 32 slots, never overwritten.
    for(unsigned i=0;i<32;++i){current=submit(h,uint16_t(300+i));txs(sc,current);}
    full=frame(sc,400);assert(h.awdlTryTransmit(&full,6)==kIOReturnNoResources);mbuf_freem(full);
    unsigned before=ackMessages;cancelOnMessage=true;d.drainTxStatus();cancelOnMessage=false;
    assert(ackMessages==before+1 && d.txStatusDiscarded==31);
    // Old session results cannot be posted after disable/re-enable.
    d.enabled=true;old[0]=submit(h,401);txs(sc,old[0]);
    sc.sc_awdl_native_session=mwx_awdl_native_new_session();d.txSession=h.awdlTxSession();
    before=ackMessages;d.drainTxStatus();assert(ackMessages==before && d.txStatusDiscarded==32);
    current=submit(h,402);sc.sc_awdl_enabled=false;sc.sc_awdl_native_session=0;t.cancel();
    txs(sc,current);txs(sc,current,false,true);assert(!h.awdlTakeTxResult(result));
    assert(!live && !dmaReclaimed); // table never reclaims the DMA-owned attempt
    // Timer/TXS/cancel races serialize solely on the short metadata lock.
    // No interleaving may produce more than one result or a fabricated ACK.
    for(unsigned iteration=0;iteration<100;++iteration){
        uint64_t identity=0;uint8_t wirePid=0;uint8_t address[6]{};
        int index=t.reserve(address,17,uint16_t(1000+iteration),1,now,identity,wirePid);assert(index>=0);
        t.finish(index,identity,true);
        std::thread timeout([&]{IOLockLock(sc.sc_awdl_lock);t.expire(now+1000000);IOLockUnlock(sc.sc_awdl_lock);});
        std::thread status([&]{IOLockLock(sc.sc_awdl_lock);t.match(17,uint16_t(1000+iteration),wirePid,true);IOLockUnlock(sc.sc_awdl_lock);});
        std::thread cancel([&]{IOLockLock(sc.sc_awdl_lock);t.cancel(17);IOLockUnlock(sc.sc_awdl_lock);});
        timeout.join();status.join();cancel.join();
        if(t.take(result)){assert(result.failed && (result.reason==2 || result.reason==3));assert(!t.take(result));}
    }
    peerRemovalRegression();
    t.serial=UINT64_MAX;uint8_t addr[6]{};
    assert(t.reserve(addr,17,3,1,now,id,pid)<0 && t.lastReason==7);
    mwx_awdl_native_session_serial=UINT64_MAX;assert(!mwx_awdl_native_new_session());
    // Production's installed module-lifetime lock is intentionally retained.
    std::puts("native UMI reservation production extraction: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-native-tx-') as td:
    cpp = Path(td) / 'test.cpp'
    exe = Path(td) / 'test'
    cpp.write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I'+str(base / 'include'), '-I'+str(root / 'kext/Wifi'),
                    '-I'+str(root / 'kext/include'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
