#!/usr/bin/env python3
"""ASan/UBSan tests of production MCU allocation, TX-power, RX refill and reset.

Functions and wire structs are extracted verbatim. Mbufs, DMA, registers,
workloop dispatch and firmware transport/parsers are explicit host models;
this tests ownership/control flow, not firmware, RF or kernel execution.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HAL = ROOT / 'kext/Wifi/vendor/SimpleMtkWlan/hal_mwx/MtkMwx.cpp'
REG = HAL.with_name('if_mwxreg.h')


def function(path, signature):
    text = path.read_text()
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 3]


def structure(name):
    text = REG.read_text()
    start = text.index('struct ' + name + ' {')
    end = text.index('\n}', start)
    return text[start:text.index(';', end) + 1]


def definitions(names):
    result = []
    for path in (REG, HAL):
        lines = iter(path.read_text().splitlines())
        for line in lines:
            match = re.match(r'\s*#define\s+(\w+)', line)
            block = line
            while block.endswith('\\'):
                block += '\n' + next(lines)
            if match and match[1] in names:
                result.append(block)
    return '\n'.join(result)


SUPPORT = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mtk_wlan.h>
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define KASSERT(x) assert(x)
#define GENMASK(h,l) ((~uint32_t(0)<<(l))&(~uint32_t(0)>>(31-(h))))
#define htole16(x) uint16_t(x)
#define htole32(x) uint32_t(x)
#define M_DONTWAIT 0
#define BUS_DMA_READ 1
#define BUS_DMA_NOWAIT 2
#define BUS_DMASYNC_PREREAD 1
#define BUS_DMASYNC_PREWRITE 2
#define BUS_DMASYNC_POSTWRITE 3
#define MWX_DEV_LOG(...) ((void)0)
#define DPRINTF(...) ((void)0)
#define DEVNAME(sc) "host-model"
using IOReturn=int;
struct OSObject {};
bool gated=false, locked=false;
unsigned liveMbufs=0, unloads=0, resets=0, enables=0;
int allocationBudget=-1, mappingSegments=1, dmaError=0;
uint64_t mappingAddress=0x10000000;
size_t mappingLength=0;
struct mbuf {
    std::vector<uint8_t> bytes;
    size_t offset=0, length=0;
    explicit mbuf(size_t n):bytes(n,0xa5) { ++liveMbufs; }
    ~mbuf() { assert(liveMbufs); --liveMbufs; }
};
mbuf *MCLGETL(void *,int,size_t n) {
    if(allocationBudget==0) return nullptr;
    if(allocationBudget>0) --allocationBudget;
    return new mbuf(n);
}
void *mbuf_data(mbuf *m) { return m->bytes.data()+m->offset; }
#define mtod(m,t) reinterpret_cast<t>(mbuf_data(m))
void mbuf_setlen(mbuf *m,size_t n) { assert(m->offset+n<=m->bytes.size()); m->length=n; }
void mbuf_pkthdr_setlen(mbuf *m,size_t n) { assert(m->offset+n<=m->bytes.size()); m->length=n; }
int mbuf_pkthdr_len(mbuf *m) { return int(m->length); }
void m_adj(mbuf *m,int n) {
    if(n>=0) { assert(size_t(n)<=m->length); m->offset+=n; m->length-=n; }
    else { assert(size_t(-n)<=m->length); m->length-=size_t(-n); }
}
void m_freem(mbuf *m) { delete m; }
struct Segment { uint64_t ds_addr; size_t ds_len; };
struct Map { unsigned dm_nsegs=0; size_t dm_mapsize=0; Segment dm_segs[2]{}; bool loaded=false; };
int bus_dmamap_load_mbuf(void *,Map *map,mbuf *m,unsigned) {
    assert(!map->loaded); map->loaded=true; map->dm_nsegs=mappingSegments;
    map->dm_mapsize=m->length; map->dm_segs[0]={mappingAddress,mappingLength?mappingLength:m->length}; return 0;
}
void bus_dmamap_unload(void *,Map *m) { assert(m->loaded); m->loaded=false; ++unloads; }
void bus_dmamap_sync(void *,Map *m,size_t,size_t,unsigned) { assert(m); }
struct WorkLoop {
    unsigned calls=0;
    IOReturn runAction(IOReturn(*f)(OSObject *,void *,void *,void *,void *),OSObject *o,void *a,void *b=nullptr) {
        bool previous=gated; gated=true; ++calls; auto rv=f(o,a,b,nullptr,nullptr); gated=previous; return rv;
    }
};
struct Interrupt:OSObject { WorkLoop *workloop=nullptr; };
using pci_intr_handle_t=Interrupt *;
constexpr unsigned MT_WFDMA0_HOST_INT_ENA=1,MT_PCIE_MAC_INT_ENABLE=2;
constexpr unsigned MT_CONN_ON_MISC=3,MT_TOP_MISC2_FW_N9_RDY=4,MT_TOP_MISC2_FW_PWR_ON=8,MT_TOP_MISC_FW_STATE=12;
constexpr unsigned MCU_CMD_NIC_POWER_CTRL=20,MCU_CMD_FW_SCATTER=21,MCU_CE_CMD_SET_RATE_TX_POWER=22,MCU_CE_QUERY_REG_READ=23;
constexpr unsigned MT_PSE_BASE=0x820c8000u;
constexpr unsigned MWX_FW_CHUNK=4096;
constexpr int MWX_HW_MT7921=0,MWX_HW_MT7922=1,MWX_HW_MT7902=2,M_DEVBUF=0;
const char *MT7921_ROM_PATCH="patch1",*MT7921_FIRMWARE_WM="ram1",*MT7922_ROM_PATCH="patch2",*MT7922_FIRMWARE_WM="ram2",*MT7902_ROM_PATCH="patch3",*MT7902_FIRMWARE_WM="ram3";
'''

MODEL = r'''
struct mwx_queue_data { Map storage; Map *md_map=&storage; mbuf *md_mbuf=nullptr; };
struct mwx_queue {
    unsigned mq_count=4,mq_cons=0,mq_prod=0,mq_regbase=100;
    Map storage; Map *mq_map=&storage;
    mwx_queue_data mq_data[4]; mwx_desc mq_desc[4]{};
};
struct mwx_txwi { bool busy=true; };
struct mwx_txwi_desc { unsigned mt_count=2; mwx_txwi mt_data[2]; };
struct mwx_softc {
    void *sc_dmat=nullptr; Interrupt *sc_ih=nullptr;
    mwx_queue sc_txq,sc_txmcuq,sc_txfwdlq,sc_rxq,sc_rxmcuq,sc_rxfwdlq;
    mwx_txwi_desc sc_txwi;
    char sc_alpha2[4]={'0','0',0,0}; int sc_hwtype=MWX_HW_MT7921; bool sc_fw_restart=false;
    struct { mtk_wlan_fw_features fw_features{}; int fw_features_error=0; bool fw_features_loaded=false; } sc_capa;
    ~mwx_softc() {
        for(auto q:{&sc_txq,&sc_txmcuq,&sc_txfwdlq,&sc_rxq,&sc_rxmcuq,&sc_rxfwdlq})
            for(auto &d:q->mq_data) if(d.md_mbuf) { bus_dmamap_unload(nullptr,d.md_map); m_freem(d.md_mbuf); }
    }
};
std::vector<std::pair<unsigned,unsigned>> writes;
void mwx_write(mwx_softc *,unsigned reg,unsigned val) { writes.push_back({reg,val}); }
void *mwx_txq_lock=nullptr;
void IOLockLock(void *) { assert(!locked); locked=true; }
void IOLockUnlock(void *) { assert(locked); locked=false; }
void mwx_txwi_put_locked(mwx_softc *,mwx_txwi *mt) { assert(gated&&locked); mt->busy=false; }
int mwx_dma_disable(mwx_softc *,int) { assert(gated); return dmaError; }
int mwx_wfsys_reset(mwx_softc *) { assert(gated); return 0; }
void mwx_queue_reset(mwx_softc *,mwx_queue *q) {
    assert(gated); ++resets; q->mq_cons=q->mq_prod=0;
    for(auto &d:q->mq_data) if(d.md_mbuf) {
        bus_dmamap_unload(nullptr,d.md_map); m_freem(d.md_mbuf); d.md_mbuf=nullptr;
    }
}
void mwx_dma_enable(mwx_softc *) { assert(gated); ++enables; }
std::vector<std::vector<uint8_t>> messages;
int sendError=0;
unsigned pseReads=0,pseWaits=0;
void mt7921_mcu_build_sku(mwx_softc *,int band,int8_t *power) { memset(power,band,MT_SKU_POWER_LIMIT); }
int mwx_mcu_send_mbuf(mwx_softc *,unsigned cmd,mbuf *m,int *) {
    assert(cmd==MCU_CE_CMD_SET_RATE_TX_POWER);
    auto bytes=mtod(m,const uint8_t *); messages.emplace_back(bytes,bytes+m->length);
    m_freem(m); return sendError;
}
int mwx_mcu_send_msg(mwx_softc *,unsigned cmd,const void *data,size_t len,int *seqp) {
    if(cmd==MCU_CE_QUERY_REG_READ) {
        assert(len==8&&seqp!=nullptr);
        uint32_t addr=0; std::memcpy(&addr,data,4);
        assert(addr==MT_PSE_BASE);
        *seqp=7; ++pseReads;
    }
    return 0;
}
int mwx_mcu_wait_resp_int(mwx_softc *,unsigned cmd,int seq,uint32_t *val) {
    assert(cmd==MCU_CE_QUERY_REG_READ&&seq==7&&val!=nullptr);
    ++pseWaits; *val=0; return 0;
}
'''

FIRMWARE = r'''
int semaphoreResult=0,patchError=0,ramError=0,releaseError=0;
unsigned semGets=0,releases=0,patches=0,rams=0,starts=0,firmwareBuffers=0;
unsigned mwx_ebusy_sleeps=0,mwx_enomem_retries=0;
uint32_t mwx_read(mwx_softc *,unsigned) { return 0; }
int mwx_poll(mwx_softc *,unsigned,unsigned,unsigned,unsigned) { return 0; }
int loadfirmware(const char *,unsigned char **out,size_t *n) {
    *n=64; *out=static_cast<unsigned char *>(std::malloc(*n)); assert(*out); ++firmwareBuffers; return 0;
}
void free(void *p,int,size_t) { assert(p&&firmwareBuffers); --firmwareBuffers; std::free(p); }
int mwx_mcu_patch_sem_ctrl(mwx_softc *,int get) {
    if(get) { ++semGets; return semaphoreResult; }
    ++releases; return releaseError;
}
int32_t mtk_wlan_patch_parse(const uint8_t *,size_t,mtk_wlan_patch_info *p) { ++patches; *p={}; p->regions=1; return patchError; }
int32_t mtk_wlan_patch_region(const uint8_t *,size_t,uint32_t,mtk_wlan_fw_region *r) { *r={}; r->len=8; return 0; }
int32_t mtk_wlan_ram_parse(const uint8_t *,size_t,mtk_wlan_ram_info *p) { ++rams; *p={}; p->regions=1; return ramError; }
int32_t mtk_wlan_ram_region(const uint8_t *,size_t,uint32_t,mtk_wlan_fw_region *r) { *r={}; r->len=8; r->download=true; return 0; }
int32_t mtk_wlan_ram_features(const uint8_t *,size_t,mtk_wlan_fw_features *out) { *out={}; return 0; }
int mwx_mcu_init_download(mwx_softc *,uint32_t,uint32_t,uint32_t) { return 0; }
int mwx_mcu_send_firmware(mwx_softc *,unsigned,const unsigned char *,size_t,unsigned) { return 0; }
int mwx_mcu_start_patch(mwx_softc *) { return 0; }
int mwx_mcu_start_firmware(mwx_softc *,uint32_t,uint32_t) { ++starts; return 0; }
void delay(unsigned) {}
'''

MAIN = r'''
static void powerTables() {
    mwx_softc sc;
    for(int band:{1,2,3}) for(int count:{0,1,7,8,9,14,16,45,109}) for(int final:{0,1}) {
        std::vector<uint8_t> channels(count); for(int i=0;i<count;++i) channels[i]=i+1;
        messages.clear(); assert(mt7921_mcu_rate_txpower_band(&sc,band,channels.data(),count,final)==0);
        unsigned copied=0;
        for(size_t i=0;i<messages.size();++i) {
            auto &bytes=messages[i]; auto h=reinterpret_cast<const mt76_connac_tx_power_limit_tlv *>(bytes.data());
            const auto n=std::min(8u,unsigned(count)-copied);
            assert(h->n_chan==n&&h->band==band&&h->last_msg==(final&&i+1==messages.size()));
            assert(bytes.size()==sizeof(*h)+n*sizeof(mt76_connac_sku_tlv));
            assert(h->ver==0&&h->len==0&&h->pad0==0&&h->pad1==0);
            for(auto p:h->pad2) assert(p==0);
            assert(h->alpha2[0]=='0'&&h->alpha2[1]=='0'&&h->alpha2[2]==0&&h->alpha2[3]==0);
            auto sku=reinterpret_cast<const mt76_connac_sku_tlv *>(h+1);
            for(unsigned j=0;j<n;++j) { assert(sku[j].channel==channels[copied++]); for(auto p:sku[j].pwr_limit) assert(p==band); }
        }
        assert(copied==unsigned(count)&&liveMbufs==0);
    }
    allocationBudget=0; assert(mt7921_mcu_rate_txpower_band(&sc,1,reinterpret_cast<const uint8_t *>("1"),1,1)==ENOMEM);
    allocationBudget=-1; sendError=EIO; messages.clear(); const uint8_t channels[9]={};
    assert(mt7921_mcu_rate_txpower_band(&sc,1,channels,9,1)==EIO&&messages.size()==1&&!liveMbufs); sendError=0;
    assert(pseReads==0&&pseWaits==0);
    sc.sc_hwtype=MWX_HW_MT7902; messages.clear();
    assert(mt7921_mcu_rate_txpower_band(&sc,2,channels,9,1)==0&&messages.size()==2&&pseReads==2&&pseWaits==2&&!liveMbufs);
}
static void refill() {
    {
        mwx_softc sc; auto &q=sc.sc_rxq;
        allocationBudget=1; assert(mwx_queue_fill(&sc,&q)==ENOMEM&&q.mq_prod==1);
        auto first=q.mq_data[0].md_mbuf; assert(first&&liveMbufs==1);
        assert(writes.back().first==q.mq_regbase+MT_DMA_CPU_IDX&&writes.back().second==1);
        allocationBudget=-1; assert(mwx_queue_fill(&sc,&q)==0&&q.mq_prod==3);
        assert(q.mq_data[0].md_mbuf==first&&liveMbufs==3);
    }
    assert(liveMbufs==0);
    for(int failure:{0,1,2}) {
        mwx_softc sc; auto &d=sc.sc_rxq.mq_data[0]; auto &desc=sc.sc_rxq.mq_desc[0];
        mappingSegments=failure==0?2:1; mappingAddress=failure==1?uint64_t(UINT32_MAX)+1:0x10000000;
        mappingLength=failure==2?MT_RX_BUF_SIZE+1:0;
        const auto before=unloads; assert(mwx_buf_fill(&sc,&d,&desc)==EFBIG);
        assert(!d.md_mbuf&&!d.md_map->loaded&&unloads==before+1&&liveMbufs==0&&desc.buf0==0);
    }
    mappingSegments=1; mappingAddress=0x10000000; mappingLength=0;
}
static void reset() {
    mwx_softc sc; WorkLoop loop; Interrupt interrupt; interrupt.workloop=&loop; sc.sc_ih=&interrupt;
    writes.clear(); assert(mwx_dma_reset(&sc,1)==0&&loop.calls==1&&!gated&&!locked);
    assert(resets==6&&enables==1&&liveMbufs==9);
    assert(writes[0]==std::make_pair(MT_WFDMA0_HOST_INT_ENA,0u)&&writes[1]==std::make_pair(MT_PCIE_MAC_INT_ENABLE,0u));
    for(auto &mt:sc.sc_txwi.mt_data) assert(!mt.busy);
    dmaError=EIO; assert(mwx_dma_reset(&sc,0)==EIO&&resets==6&&enables==1); dmaError=0;
    sc.sc_ih=nullptr; assert(mwx_dma_reset(&sc,1)==ENXIO&&loop.calls==2);
    // MT7902 has no sc_rxmcuq (RX ring 4): reset succeeds with mq_map=nullptr,
    // resets 5 queues and refills only sc_rxq + sc_rxfwdlq.
    mwx_softc sc02; sc02.sc_hwtype=MWX_HW_MT7902; sc02.sc_rxmcuq.mq_map=nullptr;
    sc02.sc_ih=&interrupt; resets=0; enables=0;
    assert(mwx_dma_reset(&sc02,1)==0&&resets==5&&enables==1&&liveMbufs==15);
}
static void firmware() {
    for(int chip:{MWX_HW_MT7921,MWX_HW_MT7922,MWX_HW_MT7902}) for(int sem:{-1,0,EIO}) for(int badPatch:{0,EINVAL}) {
        mwx_softc sc; sc.sc_hwtype=chip; semaphoreResult=sem; patchError=badPatch;
        semGets=releases=patches=rams=starts=0;
        int rv=mwx_load_firmware(&sc);
        if(sem==-1) assert(rv==0&&patches==0&&releases==0&&rams==1&&starts==1);
        else if(sem!=0) assert(rv==sem&&patches==0&&releases==0&&rams==0&&starts==0);
        else if(badPatch) assert(rv==badPatch&&patches==1&&releases==1&&rams==0&&starts==0);
        else assert(rv==0&&patches==1&&releases==1&&rams==1&&starts==1);
        assert(semGets==1&&firmwareBuffers==0);
    }
}
int main() {
    powerTables(); refill(); reset(); assert(liveMbufs==0); firmware();
    std::puts("HAL safety: poisoned MCU buffers, all TX-power batch sizes, partial RX refill, invalid mappings, gated reset and preloaded ROM patch passed");
}
'''

code = SUPPORT + definitions({'MT_SKU_POWER_LIMIT', 'MT_RX_BUF_SIZE', 'MT_DMA_CPU_IDX',
                             'MT_DMA_CTL_SD_LEN0', 'MT_DMA_CTL_LAST_SEC0',
                             'MT_DMA_CTL_SD_LEN_MASK', 'MT_DMA_CTL_SD_LEN0_SHIFT'})
code += '\n' + '\n'.join(structure(name) for name in
                         ('mwx_mcu_txd', 'mwx_desc', 'mt76_connac_sku_tlv', 'mt76_connac_tx_power_limit_tlv'))
code += MODEL
for signature in ('struct mbuf *\nmwx_mcu_alloc_msg(', 'void\nmwx_mcu_set_len(',
                  'int\nmt7921_mcu_rate_txpower_band(', 'int\nmwx_buf_fill(',
                  'int\nmwx_queue_fill(', 'static IOReturn\nmwx_dma_reset_gated(',
                  'int\nmwx_dma_reset('):
    code += '\n' + function(HAL, signature)
code += FIRMWARE + function(HAL, 'int\nmwx_load_firmware(') + MAIN
with tempfile.TemporaryDirectory(prefix='mtk-hal-safety-') as directory:
    cpp = Path(directory) / 'safety.cpp'
    binary = cpp.with_suffix('')
    cpp.write_text(code)
    subprocess.run(['/usr/bin/clang++', '-std=c++17', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(ROOT / 'kext/Wifi'),
                    '-I' + str(ROOT / 'kext/include'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
