#!/usr/bin/env python3
"""Run production infrastructure BA builders, MCU status handling and admission."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
hal = (vendor / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()
reg = (vendor / 'SimpleMtkWlan/hal_mwx/if_mwxreg.h').read_text()


def function(signature):
    start = hal.index(signature)
    return hal[start:hal.index('\n}\n', start) + 3]


def structure(source, name):
    start = source.index('struct ' + name + ' {')
    end = source.index('\n}', start)
    return source[start:source.index(';', end) + 1]


constants = '\n'.join(line for source in [hal, reg] for line in source.splitlines()
                      if re.match(r'#define\s+(?:STA_REC_BA|STA_REC_WTBL|WTBL_BA|WTBL_SET|'
                                  r'MT_BA_TYPE_ORIGINATOR|MT_BA_TYPE_RECIPIENT|'
                                  r'RST_BA_MAC_TID_MATCH|MWX_KNOB_NO_RXBA|MWX_KNOB_NO_TXBA)\s', line))
wire = '\n'.join(structure(reg, name) for name in
                 ['sta_req_hdr', 'sta_rec_ba', 'sta_rec_wtbl', 'wtbl_ba', 'mt7921_mcu_uni_event'])
# Exercise the existing bounded unified-event status decoder and waiter too;
# only mbuf allocation, response delivery and transport are mocked.
a = hal.index('\t\t\tstruct mt7921_mcu_uni_event *event;', hal.index('mwx_mcu_rx_event('))
decode = hal[a:hal.index('\n\t\t} else if (cmd == MCU_CE_QUERY_REG_READ)', a)]
a = hal.index('sc->sc_mcu_wait[rxd_seq].mcu_int = mcu_int;')
store = hal[a:hal.index('        if(gMwxHal)', a)]
helper_signature = 'static int\nmwx_ba_command('
helper = function(helper_signature) if helper_signature in hal else ''
code = r'''
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/types.h>
#define __packed __attribute__((packed))
#define nitems(a) int(sizeof(a)/sizeof((a)[0]))
#define KASSERT(x) assert(x)
#define MWX_DEV_LOG(...) ((void)0)
#define DEVNAME(sc) "test"
#define printf(...) ((void)0)
#define htole16(x) uint16_t(x)
#define le16toh(x) uint16_t(x)
#define le32toh(x) uint32_t(x)
#define IEEE80211_ADDR_LEN 6
#define IEEE80211_S_SCAN 1
#define IEEE80211_S_RUN 4
#define IEEE80211_BA_INIT 0
#define IEEE80211_BA_REQUESTED 1
#define IEEE80211_BA_AGREED 2
#define IEEE80211_STATUS_UNSPECIFIED 1
#define MCU_UNI_CMD_STA_REC_UPDATE 1
#define M_DONTWAIT 0
''' + constants + '\n' + wire + r'''
unsigned live=0,allocations=0,prepends=0,waits=0,resets=0,accepted=0,refused=0,scheduled=0;
unsigned failAllocation=0,failPrepend=0;
struct mbuf {
    alignas(8) uint8_t bytes[512]{};
    size_t offset=64,length=0,packetLength=0;
    mbuf(){++live;}~mbuf(){assert(live);--live;}
};
size_t mbuf_len(const mbuf *m){return m->length;}
void *mbuf_data(mbuf *m){return m->bytes+m->offset;}
void mbuf_setlen(mbuf *m,size_t n){assert(m->offset+n<=sizeof(m->bytes));m->length=n;}
void mbuf_pkthdr_setlen(mbuf *m,size_t n){m->packetLength=n;}
int m_trailingspace(const mbuf *m){return sizeof(m->bytes)-m->offset-m->length;}
size_t m_leadingspace(const mbuf *m){return m->offset;}
#define mtod(m,t) reinterpret_cast<t>(mbuf_data(m))
void m_freem(mbuf *m){delete m;}
mbuf *m_prepend(mbuf *m,size_t n,int){
    if(++prepends==failPrepend){m_freem(m);return nullptr;}
    assert(m->offset>=n);m->offset-=n;m->length+=n;m->packetLength=m->length;
    return m;
}
mbuf *mwx_alloc_sta_req_tlv(int length){
    if(++allocations==failAllocation)return nullptr;
    auto m=new mbuf;m->offset+=length;
    return m;
}
struct ieee80211_rx_ba {unsigned ba_state=IEEE80211_BA_REQUESTED;uint16_t ba_winsize=64,ba_winstart=4095;};
struct ieee80211_tx_ba {unsigned ba_state=IEEE80211_BA_REQUESTED;uint16_t ba_winsize=64,ba_winstart=4095;};
struct ieee80211_node {
    uint8_t ni_macaddr[6]{2,9,8,7,6,5};
    ieee80211_rx_ba ni_rx_ba[8];ieee80211_tx_ba ni_tx_ba[8];
};
struct mwx_node : ieee80211_node {uint16_t wcid=0x143;};
struct ieee80211com {void *ic_softc=nullptr;int ic_state=IEEE80211_S_RUN;ieee80211_node *ic_bss=nullptr;};
struct mwx_vif {uint8_t idx=2,omac_idx=3;};
struct task {};
''' + structure(hal, 'mwx_ba_task_arg') + r'''
struct mwx_softc {
    ieee80211com sc_ic;mwx_vif sc_vif;
    struct {uint32_t mcu_int=0;mbuf *mcu_m=nullptr;bool mcu_done=false;} sc_mcu_wait[16];
    mwx_ba_task_arg sc_ba_arg[16]{};unsigned sc_ba_cur=0,sc_ba_tail=0,sc_knobs=0;
    void *sc_nswq=nullptr;task sc_ba_task;
    mwx_softc(){sc_ic.ic_softc=this;}
};
struct Reply {int sendError=0,waitError=0;uint32_t status=0;unsigned length=8;bool cancel=false;};
std::array<Reply,2> replies{};
std::vector<std::vector<uint8_t>> commands;
int splnet(){return 0;}void splx(int){}
void task_add(void *,task *){++scheduled;}
void mwx_reset(mwx_softc *){++resets;}
void ieee80211_addba_req_accept(ieee80211com *,ieee80211_node *ni,uint8_t tid){
    assert(commands.size()==2 && ni->ni_rx_ba[tid].ba_state==IEEE80211_BA_REQUESTED);
    ni->ni_rx_ba[tid].ba_state=IEEE80211_BA_AGREED;++accepted;
}
void ieee80211_addba_resp_accept(ieee80211com *,ieee80211_node *ni,uint8_t tid){
    assert(commands.size()==2 && ni->ni_tx_ba[tid].ba_state==IEEE80211_BA_REQUESTED);
    ni->ni_tx_ba[tid].ba_state=IEEE80211_BA_AGREED;++accepted;
}
void ieee80211_addba_req_refuse(ieee80211com *,ieee80211_node *ni,uint8_t tid){
    ni->ni_rx_ba[tid].ba_state=IEEE80211_BA_INIT;++refused;
}
void ieee80211_addba_resp_refuse(ieee80211com *,ieee80211_node *ni,uint8_t tid,unsigned reason){
    assert(reason==IEEE80211_STATUS_UNSPECIFIED);
    ni->ni_tx_ba[tid].ba_state=IEEE80211_BA_INIT;++refused;
}
''' + function('void *\nmwx_append_len(') + function('void *\nmwx_append_tlv(') + \
    function('struct mbuf *\nmwx_fill_sta_req_hdr(') + r'''
bool deliverResponse(mwx_softc *sc,int rxd_seq,mbuf *m){
    uint32_t mcu_int=0;
    switch(0){case 0:{
''' + decode + '\n' + store + r'''
        return true;
    }}
    m_freem(m);return false;
}
int mwx_mcu_send_mbuf(mwx_softc *sc,uint32_t cmd,mbuf *m,int *seq){
    assert(cmd==MCU_UNI_CMD_STA_REC_UPDATE && commands.size()<2);
    unsigned index=commands.size();*seq=int(index)+1;
    sc->sc_mcu_wait[*seq]={};
    commands.emplace_back(mtod(m,const uint8_t *),mtod(m,const uint8_t *)+mbuf_len(m));
    m_freem(m); // The transport owns submitted mbufs, including its errors.
    return replies[index].sendError;
}
int mwx_mcu_sleep_resp(mwx_softc *sc,uint32_t cmd,int seq){
    assert(cmd==MCU_UNI_CMD_STA_REC_UPDATE && seq>=1 && seq<=2);++waits;
    const auto &reply=replies[seq-1];
    if(reply.waitError)return reply.waitError;
    auto m=new mbuf;m->length=reply.length;
    auto body=mtod(m,uint8_t *);body[0]=3;
    for(unsigned i=0;i<4;++i)body[4+i]=uint8_t(reply.status>>(8*i));
    bool done=deliverResponse(sc,seq,m);
    if(reply.cancel && sc->sc_ic.ic_bss){
        for(unsigned i=0;i<8;++i){
            sc->sc_ic.ic_bss->ni_rx_ba[i].ba_state=IEEE80211_BA_INIT;
            sc->sc_ic.ic_bss->ni_tx_ba[i].ba_state=IEEE80211_BA_INIT;
        }
    }
    assert(sc->sc_mcu_wait[seq].mcu_done==done);
    return done?0:EWOULDBLOCK;
}
''' + function('int\nmwx_mcu_wait_resp_int(') + \
    function('int\nmwx_mcu_send_mbuf_wait(') + helper + \
    function('int\nmt7921_mcu_sta_rx_ba(') + function('int\nmt7921_mcu_sta_tx_ba(') + \
    function('static int\nmwx_ba_enqueue(') + function('int\nmwx_ampdu_rx_start(') + \
    function('void\nmwx_ampdu_rx_stop(') + function('int\nmwx_ampdu_tx_start(') + \
    function('void\nmwx_ampdu_tx_stop(') + function('void\nmwx_ba_task(') + r'''
void clear(){
    assert(live==0);replies={};commands.clear();
    allocations=prepends=waits=resets=accepted=refused=scheduled=0;
    failAllocation=failPrepend=0;
}
int program(mwx_softc &sc,mwx_node &peer,bool tx,uint8_t tid=5,bool enable=true){
    return tx?mt7921_mcu_sta_tx_ba(&sc,&peer,tid,enable):mt7921_mcu_sta_rx_ba(&sc,&peer,tid,enable);
}
int start(mwx_softc &sc,mwx_node &peer,bool tx,uint8_t tid=5){
    return tx?mwx_ampdu_tx_start(&sc.sc_ic,&peer,tid):mwx_ampdu_rx_start(&sc.sc_ic,&peer,tid);
}
void checkWire(const mwx_softc &sc,const mwx_node &peer,bool tx,uint8_t tid,bool enable,uint16_t window){
    assert(commands.size()==2);
    for(unsigned i=0;i<2;++i){
        const auto &bytes=commands[i];assert(bytes.size()>=sizeof(sta_req_hdr)+4);
        auto hdr=reinterpret_cast<const sta_req_hdr *>(bytes.data());
        assert(hdr->bss_idx==sc.sc_vif.idx && hdr->muar_idx==sc.sc_vif.omac_idx);
        assert(hdr->wlan_idx_lo==(peer.wcid&0xff) && hdr->wlan_idx_hi==(peer.wcid>>8));
        assert(hdr->tlv_num==1 && hdr->is_tlv_append==1);
        auto p=bytes.data()+sizeof(*hdr);
        if((tx && i==0)||(!tx && i==1)){
            assert(bytes.size()==sizeof(*hdr)+sizeof(sta_rec_ba));
            auto ba=reinterpret_cast<const sta_rec_ba *>(p);
            assert(ba->tag==STA_REC_BA && ba->len==sizeof(*ba));
            assert(ba->tid==tid && ba->ba_type==(tx?MT_BA_TYPE_ORIGINATOR:MT_BA_TYPE_RECIPIENT));
            assert(ba->amsdu==0 && ba->ba_en==(enable?1u<<tid:0));
            assert(ba->ssn==(enable?4095:0) && ba->winsize==(enable?window:0));
        }else{
            assert(bytes.size()==sizeof(*hdr)+sizeof(sta_rec_wtbl)+sizeof(wtbl_ba));
            auto table=reinterpret_cast<const sta_rec_wtbl *>(p);
            assert(table->tag==STA_REC_WTBL && table->len==sizeof(*table)+sizeof(wtbl_ba));
            assert(table->wlan_idx_lo==(peer.wcid&0xff) && table->wlan_idx_hi==(peer.wcid>>8));
            assert(table->operation==WTBL_SET && table->tlv_num==1);
            auto ba=reinterpret_cast<const wtbl_ba *>(p+sizeof(*table));
            assert(ba->tag==WTBL_BA && ba->len==sizeof(*ba) && ba->tid==tid);
            assert(ba->ba_type==(tx?MT_BA_TYPE_ORIGINATOR:MT_BA_TYPE_RECIPIENT));
            assert(ba->ba_winsize==(enable?window:0));
            if(tx){
                const unsigned ranges[]={4,8,12,24,36,48,54,64};unsigned index=0;
                for(unsigned j=0;j<8;++j)if(window>=ranges[j])index=j;
                assert(ba->sn==(enable?4095:0) && ba->ba_en==unsigned(enable));
                assert(ba->ba_winsize_idx==(enable?index:0));
            }else{
                assert(!memcmp(ba->peer_addr,peer.ni_macaddr,6));
                assert(ba->rst_ba_tid==tid && ba->rst_ba_sel==RST_BA_MAC_TID_MATCH && ba->rst_ba_sb==1);
            }
        }
    }
}
int main(){
    // Firmware nonzero status must fail either stage, not merely complete
    // the transport wait. The second command must not run after stage 1 fails.
    for(bool tx:{false,true})for(unsigned stage=0;stage<2;++stage){
        for(uint32_t status:{1u,9u,UINT32_MAX}){
            clear();mwx_softc sc;mwx_node peer;replies[stage].status=status;
            assert(program(sc,peer,tx)==EIO);
            assert(commands.size()==stage+1 && waits==stage+1 && resets==0 && live==0);
        }
        for(bool transport:{false,true}){
            clear();mwx_softc sc;mwx_node peer;
            if(transport)replies[stage].sendError=ENOBUFS;
            else replies[stage].waitError=EWOULDBLOCK;
            assert(program(sc,peer,tx)==(transport?ENOBUFS:EWOULDBLOCK));
            assert(commands.size()==stage+1 && waits==(transport?stage:stage+1));
            assert(resets==unsigned(!transport) && live==0);
        }
        for(unsigned length=0;length<sizeof(mt7921_mcu_uni_event);++length){
            clear();mwx_softc sc;mwx_node peer;replies[stage].length=length;
            assert(program(sc,peer,tx)==EWOULDBLOCK);
            assert(commands.size()==stage+1 && resets==1 && live==0);
        }
    }
    // The normal RX/TX ordering, wire fields, supported TIDs and disable
    // encodings remain unchanged, including TX's firmware window ladder.
    for(bool tx:{false,true})for(bool enable:{false,true})for(uint8_t tid=0;tid<8;++tid){
        for(uint16_t window:{4,8,12,24,36,48,54,64}){
            clear();mwx_softc sc;mwx_node peer;
            peer.ni_rx_ba[tid].ba_winsize=peer.ni_tx_ba[tid].ba_winsize=window;
            assert(program(sc,peer,tx,tid,enable)==0 && live==0);
            checkWire(sc,peer,tx,tid,enable,window);
        }
    }
    for(bool tx:{false,true})for(unsigned stage=1;stage<=2;++stage){
        for(bool prepend:{false,true}){
            clear();mwx_softc sc;mwx_node peer;
            if(prepend)failPrepend=stage;else failAllocation=stage;
            assert(program(sc,peer,tx)==ENOBUFS && commands.size()==stage-1 && live==0);
        }
    }
    // EBUSY is deferred admission, not firmware acceptance. Only after both
    // zero statuses may the existing BA task accept the pending agreement.
    for(bool tx:{false,true})for(unsigned stage=0;stage<2;++stage)for(unsigned fault=0;fault<4;++fault){
        clear();mwx_softc sc;mwx_node peer;sc.sc_ic.ic_bss=&peer;
        if(fault==1)replies[stage].status=9;
        if(fault==2)replies[stage].sendError=ENOBUFS;
        if(fault==3)replies[stage].waitError=EWOULDBLOCK;
        assert(start(sc,peer,tx)==EBUSY && commands.empty() && scheduled==1);
        assert(accepted==0 && refused==0);
        mwx_ba_task(&sc);
        assert(accepted==(fault?0u:1u) && refused==(fault?1u:0u));
        assert(sc.sc_ba_cur==sc.sc_ba_tail && live==0);
    }
    // A request canceled during the wait must not fabricate acceptance or
    // refusal; association/state guards must still discard stale queued work.
    for(bool tx:{false,true}){
        clear();mwx_softc sc;mwx_node peer;sc.sc_ic.ic_bss=&peer;replies[0].cancel=true;
        assert(start(sc,peer,tx)==EBUSY);mwx_ba_task(&sc);
        assert(!accepted && !refused && live==0);
        for(bool oldPeer:{false,true}){
            clear();mwx_softc stale;mwx_node replacement;
            stale.sc_ic.ic_bss=oldPeer?&replacement:&peer;
            if(!oldPeer)stale.sc_ic.ic_state=IEEE80211_S_SCAN;
            assert(start(stale,peer,tx)==EBUSY);mwx_ba_task(&stale);
            assert(commands.empty() && !accepted && !refused);
        }
        clear();mwx_softc disabled;disabled.sc_knobs=tx?MWX_KNOB_NO_TXBA:MWX_KNOB_NO_RXBA;
        assert(start(disabled,peer,tx)==ENOTSUP && scheduled==0 && commands.empty());
        clear();mwx_softc full;full.sc_ic.ic_bss=&peer;
        for(unsigned i=0;i<15;++i)assert(start(full,peer,tx)==EBUSY);
        assert(start(full,peer,tx)==ENOSPC && scheduled==15);
        clear();mwx_softc stopping;stopping.sc_ic.ic_bss=&peer;
        if(tx)mwx_ampdu_tx_stop(&stopping.sc_ic,&peer,5);
        else mwx_ampdu_rx_stop(&stopping.sc_ic,&peer,5);
        mwx_ba_task(&stopping);checkWire(stopping,peer,tx,5,false,64);
        assert(!accepted && !refused && live==0);
    }
    // The BA-only correction must not silently change generic MCU callers.
    clear();mwx_softc generic;replies[0].status=9;
    assert(mwx_mcu_send_mbuf_wait(&generic,MCU_UNI_CMD_STA_REC_UPDATE,new mbuf)==0);
    assert(live==0);
    puts("PASS: production BA status/barrier, RX/TX TLVs, transport faults, truncation, deferred admission and cancellation");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-wifi-ba-') as tmp:
    work = Path(tmp)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(work / 'test.cpp'),
                    '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
