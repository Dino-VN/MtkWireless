#!/usr/bin/env python3
"""Exercise the production chunked firmware scan used while AWDL is on.

Extracts mt7921_mcu_hw_scan, the SCAN_DONE handler and the chunk timer/task
from MtkMwx.cpp and runs them against a host model of the firmware: every
START_HW_SCAN is recorded, SCAN_DONE is delivered by the test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
hal = (vendor / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()
reg = (vendor / 'SimpleMtkWlan/hal_mwx/if_mwxreg.h').read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


start = reg.index('struct mt76_connac_mcu_scan_ssid {')
structs = reg[start:reg.index('struct mwx_patch_hdr', start)]
functions = ''.join(function(hal, s) for s in (
    'static unsigned\nmwx_scan_chunk_channels(',
    'static int\nmwx_scan_chunk_gap_ms(',
    'int\nmt7921_mcu_hw_scan(',
    'int\nmt7921_mcu_hw_scan_cancel(',
    'void\nmt7921_mcu_scan_event(',
    'void\nmwx_scan_chunk_timeo(',
    'void\nmwx_scan_chunk_task(',
))

code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define __packed __attribute__((packed))
#define IEEE80211_NWID_LEN 32
#define IEEE80211_ADDR_LEN 6
#define ETHER_ADDR_LEN 6
#define IEEE80211_CHAN_MAX 64
#define IEEE80211_S_SCAN 1
#define IEEE80211_S_AUTH 2
#define IEEE80211_S_RUN 4
#define IEEE80211_F_BGSCAN 1
#define IFF_RUNNING 1
#define MWX_FLAG_SCANNING 1
#define MWX_FLAG_BGSCAN 2
#define MWX_BGSCAN_DWELL_AUTO 0xffffffffU
#define MTK_WLAN_SCAN_MAX_SSIDS 10
#define MCU_CE_CMD_START_HW_SCAN 3
#define MCU_CE_CMD_CANCEL_HW_SCAN 27
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define htole16(x) (x)
#define htole32(x) (x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define MWX_DEV_LOG(...) ((void)0)
#define DEVNAME(sc) "test"
''' + structs + r'''
struct ieee80211_channel { int ic_flags=0; };
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->ic_flags & 2)
#define IEEE80211_IS_CHAN_6GHZ(c) (false)
#define IEEE80211_IS_6GHZ_PSC(n) (false)
struct mtk_wlan_scan_ssids {uint8_t count=0;uint8_t lengths[10]{};uint8_t ssids[10][32]{};bool wildcard=false;};
struct join_diag { int Active=0; uint64_t ScanStartedAtUS=0,ScanDoneAtUS=0;
    unsigned Attempt=0, ScanSuspendedAWDL=0; };
struct _ifnet { int if_flags=IFF_RUNNING; };
struct ieee80211com {
    _ifnet ic_if; int ic_state=IEEE80211_S_SCAN, ic_flags=0;
    bool ic_wcl_scan_requested=false, ic_wcl_scan_inflight=false;
    bool ic_wcl_scan_active=false, ic_wcl_scan_restricted=false;
    bool ic_wcl_join_requested=false;
    unsigned ic_wcl_scan_ssid_length=0, ic_des_esslen=0, ic_wcl_join_chan=0;
    unsigned ic_wcl_scan_away_time_ms=0; uint64_t ic_awdl_data_at=0;
    uint8_t ic_wcl_scan_ssid[32]{}, ic_des_essid[32]{}, ic_wcl_scan_channels[32]{};
    mtk_wlan_scan_ssids ic_wcl_scan_targets;
    join_diag ic_join_diag;
    ieee80211_channel ic_channels[IEEE80211_CHAN_MAX+1];
};
unsigned ieee80211_chan2idx(ieee80211com*ic,ieee80211_channel*c){return unsigned(c-ic->ic_channels);}
unsigned ieee80211_chan2ieee(ieee80211com*ic,ieee80211_channel*c){return unsigned(c-ic->ic_channels);}
struct mbuf { std::vector<uint8_t> data; };
typedef mbuf *mbuf_t;
#define mtod(m,t) reinterpret_cast<t>((m)->data.data())
size_t mbuf_len(mbuf*m){return m->data.size();}
struct task { int queued=0; };
struct taskq {};
void task_add(taskq*,task*t){++t->queued;}
int splnet(){return 0;} void splx(int){}
uint64_t nowUS=10000000;
uint64_t mtk_join_now(){return nowUS;}
int timeout_add_sec(int*t,int s){*t=s*1000;return 0;}
int timeout_add_msec(int*t,int ms){*t=ms;return 0;}
int timeout_del(int*t){*t=0;return 0;}
uint32_t bootChunk=4, bootGap=250;
bool PE_parse_boot_argn(const char*name,void*out,int){
    if(!strcmp(name,"amtk_scan_chunk")){memcpy(out,&bootChunk,4);return true;}
    if(!strcmp(name,"amtk_scan_gap")){memcpy(out,&bootGap,4);return true;}
    return false;
}
struct mwx_softc {
    ieee80211com sc_ic; bool sc_awdl_enabled=true, sc_awdl_scan_suspended=false;
    bool sc_detaching=false; int sc_resetting=0;
    int sc_ns_state=IEEE80211_S_SCAN;
    unsigned sc_flags=0, sc_scan_join_attempt=0; uint8_t sc_scan_seq_num=0;
    uint32_t sc_bgscan_dwell=MWX_BGSCAN_DWELL_AUTO;
    uint64_t sc_awdl_scan_preempted=0;
    uint32_t sc_scan_done_count=0, sc_scan_last_ieee80211_nodes=0;
    uint32_t sc_rx_pkt_normal=0, sc_rx_fill_fail=0;
    int sc_scan_to=0, sc_scan_chunk_to=0;
    taskq queue; taskq*sc_nswq=&queue;
    task sc_scan_task, sc_scan_chunk_task;
    uint32_t sc_scan_chunk_size=0, sc_scan_chunk_next=0, sc_scan_chunk_gen=0,
        sc_scan_chunk_armed_gen=0;
    bool sc_scan_chunk_bgscan=false, sc_scan_chunk_resume=false;
    uint64_t sc_scan_chunked=0, sc_scan_chunks=0, sc_scan_chunk_abandoned=0;
};
unsigned aborts=0;
struct MwxAwdlInfraOperation { explicit MwxAwdlInfraOperation(mwx_softc*){} };
int mwx_awdl_abort(mwx_softc*){++aborts;return 0;}
int mwx_count_nodes(ieee80211com*){return 0;}
mbuf*mwx_mcu_alloc_msg(size_t n){auto m=new mbuf;m->data.resize(n+7);return m;}
std::vector<mt76_connac_hw_scan_req> sent;
int sendResult=0;
int mwx_mcu_send_mbuf(mwx_softc*,int cmd,mbuf*m,void*){
    assert(cmd==MCU_CE_CMD_START_HW_SCAN);
    if(!sendResult){ mt76_connac_hw_scan_req r; memcpy(&r,m->data.data(),sizeof r); sent.push_back(r); }
    delete m; return sendResult;
}
int mwx_mcu_send_msg(mwx_softc*,int cmd,void*,size_t,void*){assert(cmd==MCU_CE_CMD_CANCEL_HW_SCAN);return 0;}
int mt7921_mcu_hw_scan_cancel(mwx_softc*);
void mwx_scan_chunk_timeo(void*);
void mwx_scan_chunk_task(void*);
''' + functions + r'''
static std::vector<unsigned> channels(const mt76_connac_hw_scan_req&r){
    std::vector<unsigned> v;
    for(unsigned i=0;i<r.channels_num;++i) v.push_back(r.channels[i].channel_num);
    for(unsigned i=0;i<r.ext_channels_num;++i) v.push_back(r.ext_channels[i].channel_num);
    return v;
}
static void done(mwx_softc&sc){
    mbuf m; m.data.resize(sizeof(mt76_connac_hw_scan_done));
    mtod(&m,mt76_connac_hw_scan_done*)->seq_num=uint8_t(sc.sc_scan_seq_num);
    mt7921_mcu_scan_event(&sc,&m);
}
// Gap timer fires, then the newstate queue runs the chunk task.
static void gap(mwx_softc&sc){
    assert(sc.sc_scan_chunk_to);
    sc.sc_scan_chunk_to=0;
    unsigned before=sc.sc_scan_chunk_task.queued;
    mwx_scan_chunk_timeo(&sc);
    if(sc.sc_scan_chunk_task.queued!=before){ sc.sc_scan_chunk_task.queued=before; mwx_scan_chunk_task(&sc); }
}
static void reset(mwx_softc&sc){ sent.clear(); sc.sc_scan_task.queued=0; aborts=0; }
int main(){
    mwx_softc sc;
    // Channels 1..11 (2 GHz) and 36,40,44,48,149 (5 GHz); 52 disabled.
    for(unsigned c=1;c<=11;++c) sc.sc_ic.ic_channels[c].ic_flags=2;
    for(unsigned c: {36u,40u,44u,48u}) sc.sc_ic.ic_channels[c].ic_flags=4;
    sc.sc_ic.ic_wcl_scan_requested=true;

    // AWDL off: one firmware scan with every channel, ends at SCAN_DONE.
    sc.sc_awdl_enabled=false;
    assert(mt7921_mcu_hw_scan(&sc,0)==0 && sent.size()==1 && channels(sent[0]).size()==15);
    done(sc);
    assert(sc.sc_scan_task.queued==1 && !sc.sc_scan_chunk_to && !sc.sc_scan_chunked);
    reset(sc);

    // AWDL on but idle (no data for 3 s): still one whole scan.
    sc.sc_awdl_enabled=true; sc.sc_ic.ic_awdl_data_at=nowUS-3000000;
    assert(mt7921_mcu_hw_scan(&sc,0)==0 && channels(sent.back()).size()==15);
    done(sc); assert(sc.sc_scan_task.queued==1 && !sc.sc_scan_chunk_to);
    reset(sc);
    sc.sc_ic.ic_awdl_data_at=nowUS-100000;

    // AWDL data flowing: 4 + 4 + 4 + 3 channels, the radio released between chunks,
    // the logical scan ended once, after the last chunk.
    sc.sc_awdl_enabled=true;
    assert(mt7921_mcu_hw_scan(&sc,0)==0 && aborts==1);
    std::vector<unsigned> all;
    for(unsigned i=0;i<4;++i){
        auto v=channels(sent.back());
        assert(v.size()==(i<3?4u:3u));
        assert(sent.back().timeout_value==v.size()*120);
        all.insert(all.end(),v.begin(),v.end());
        assert(sc.sc_flags & MWX_FLAG_SCANNING);
        done(sc);
        assert(!(sc.sc_flags & MWX_FLAG_SCANNING));
        if(i<3){ assert(sc.sc_scan_chunk_to==250 && !sc.sc_scan_task.queued); gap(sc); }
    }
    assert(sc.sc_scan_task.queued==1 && !sc.sc_scan_chunk_next && sent.size()==4 && aborts==4);
    assert((all==std::vector<unsigned>{1,2,3,4,5,6,7,8,9,10,11,36,40,44,48}));
    assert(sc.sc_scan_chunked==1 && sc.sc_scan_chunks==4);
    reset(sc);

    // A fresh scan during the gap supersedes the pending chunk.
    assert(mt7921_mcu_hw_scan(&sc,0)==0); done(sc);
    assert(sc.sc_scan_chunk_to);
    assert(mt7921_mcu_hw_scan(&sc,0)==0 && !sc.sc_scan_chunk_to);
    assert(channels(sent.back())[0]==1 && sent.size()==2);
    mwx_scan_chunk_task(&sc); // stale task: nothing sent
    assert(sent.size()==2);
    reset(sc);

    // The scan ending (state left SCAN) during the gap abandons the rest.
    assert(mt7921_mcu_hw_scan(&sc,0)==0); done(sc);
    sc.sc_ic.ic_state=IEEE80211_S_AUTH;
    uint64_t abandoned=sc.sc_scan_chunk_abandoned;
    gap(sc);
    assert(sent.size()==1 && !sc.sc_scan_chunk_next && sc.sc_scan_chunk_abandoned==abandoned+1);
    sc.sc_ic.ic_state=IEEE80211_S_SCAN;
    reset(sc);

    // A chunk the firmware refuses ends the logical scan with what it has.
    assert(mt7921_mcu_hw_scan(&sc,0)==0); done(sc);
    sendResult=5; gap(sc); sendResult=0;
    assert(sc.sc_scan_task.queued==1 && !sc.sc_scan_chunk_next && !(sc.sc_flags & MWX_FLAG_SCANNING));
    reset(sc);

    // Joins are never chunked: neither the WCL channel nor the fallback.
    sc.sc_ic.ic_wcl_scan_requested=false; sc.sc_ic.ic_wcl_join_requested=true;
    sc.sc_ic.ic_des_esslen=4; memcpy(sc.sc_ic.ic_des_essid,"Dino",4);
    sc.sc_ic.ic_wcl_join_chan=6;
    assert(mt7921_mcu_hw_scan(&sc,0)==0 && channels(sent.back())==std::vector<unsigned>{6});
    done(sc); assert(sc.sc_scan_task.queued==1 && !sc.sc_scan_chunk_to);
    assert(mt7921_mcu_hw_scan(&sc,0)==0 && channels(sent.back()).size()==15);
    sc.sc_ic.ic_wcl_join_requested=false; sc.sc_ic.ic_des_esslen=0;
    reset(sc);

    // Associated background scan: chunks keep MWX_FLAG_BGSCAN and stop if
    // net80211's background scan ended meanwhile.
    sc.sc_ic.ic_state=IEEE80211_S_RUN; sc.sc_ic.ic_wcl_scan_requested=true;
    sc.sc_ic.ic_flags=IEEE80211_F_BGSCAN;
    assert(mt7921_mcu_hw_scan(&sc,1)==0); sc.sc_flags|=MWX_FLAG_BGSCAN;
    done(sc); gap(sc);
    assert(sent.size()==2 && (sc.sc_flags & MWX_FLAG_BGSCAN) && sc.sc_scan_to==12000);
    done(sc); sc.sc_ic.ic_flags=0; gap(sc);
    assert(sent.size()==2 && !sc.sc_scan_chunk_next);
    // Unrequested associated scans are still suspended for AWDL.
    sc.sc_ic.ic_wcl_scan_requested=false;
    assert(mt7921_mcu_hw_scan(&sc,1)==0 && sc.sc_awdl_scan_suspended);
    puts("PASS: chunked AWDL scan order/coverage, gap release, supersede, abandon, refusal, joins whole, bgscan");
}
'''

with tempfile.TemporaryDirectory(prefix='mtk-wifi-scan-chunk-') as directory:
    work = Path(directory)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wno-unused-function', '-Wno-unused-but-set-variable',
                    '-fsanitize=address,undefined', str(work / 'test.cpp'),
                    '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
