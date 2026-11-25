#!/usr/bin/env python3
"""Source-bound EDCA wire/ACI and software-only scan-node regressions.

Firmware sends, allocation and WTBL writes are host boundaries. Runs the actual
builders under ASan/UBSan, not a formula copied in place of production code.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HAL = ROOT / 'kext/Wifi/vendor/SimpleMtkWlan/hal_mwx/MtkMwx.cpp'
REG = HAL.with_name('if_mwxreg.h')
VAR = ROOT / 'kext/Wifi/vendor/mtk80211/openbsd/net80211/ieee80211_var.h'
source = HAL.read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


params = VAR.read_text()
start = params.index('struct ieee80211_edca_ac_params {')
params = params[start:params.index('\n};', start) + 3]
wcid = '\n'.join(re.findall(r'(?m)^#define[ \t]+(?:MWX_WTBL_\w+|MWX_MAX_INTERFACES)[ \t]+[^\n]+', REG.read_text()))
code = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <sys/types.h>
#define __packed __attribute__((packed))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define htole16(n) uint16_t(n)
#define IEEE80211_NUM_ACS 4
#define IEEE80211_F_QOS 1
#define IEEE80211_NODE_QOS 1
#define IEEE80211_AID(n) ((n)&0x3fff)
#define M_DEVBUF 0
#define M_NOWAIT 1
#define M_ZERO 2
#define DEVNAME(sc) "edca-host"
constexpr unsigned MCU_CE_CMD_SET_EDCA_PARMS=7;
struct ieee80211_node { unsigned ni_flags=0; uint16_t ni_associd=0; uint8_t ni_macaddr[6]{}; };
''' + wcid + '\n' + params + r'''
struct ieee80211com { void *ic_softc=nullptr; unsigned ic_flags=0; ieee80211_node *ic_bss=nullptr; ieee80211_edca_ac_params ic_edca_ac[4]{}; };
struct mwx_node { ieee80211_node ni; uint16_t wcid; unsigned flags; };
struct mwx_vif { uint8_t idx=0,wmm_idx=0; };
struct mwx_softc { ieee80211com sc_ic; mwx_vif sc_vif; mwx_vif sc_awdl_vif; };
int awdlEdcaBootArg=-1;
bool PE_parse_boot_argn(const char *arg,void *buf,int len) {
    if(strcmp(arg,"amtk_awdl_edca")!=0||awdlEdcaBootArg<0||len!=sizeof(int)) return false;
    memcpy(buf,&awdlEdcaBootArg,sizeof(int)); return true;
}
std::vector<uint8_t> message;
int sendError=0;
int mwx_mcu_send_msg(mwx_softc *,unsigned cmd,void *data,size_t n,int *seq) {
    assert(cmd==MCU_CE_CMD_SET_EDCA_PARMS&&!seq&&n==44);
    auto p=static_cast<const uint8_t *>(data); message.assign(p,p+n); return sendError;
}
bool allocationFails=false;
unsigned clears=0; uint16_t clearedWcid=0;
void *malloc(size_t n,int,unsigned flags) { assert(flags==(M_NOWAIT|M_ZERO)); return allocationFails?nullptr:std::calloc(1,n); }
int mwx_mac_wtbl_update(mwx_softc *,uint16_t id) { ++clears; clearedWcid=id; return 0; }
const char *ether_sprintf(const uint8_t *) { return "host-node"; }
''' + function('int\nmt7921_mcu_set_tx(') + function('struct ieee80211_node *\nmwx_node_alloc(') + function('void\nmwx_newassoc(') + r'''
static uint16_t word(size_t offset) { assert(offset+2<=message.size()); uint16_t n; memcpy(&n,message.data()+offset,2); return n; }
static void checkDefault() {
    assert(message[41]==0&&message[43]==0);
    for(unsigned ac=0;ac<4;++ac) {
        assert(word(ac*10)==5&&word(ac*10+2)==10&&word(ac*10+4)==0&&word(ac*10+6)==2);
        assert(message[ac*10+8]==0&&message[ac*10+9]==0);
    }
}
int main() {
    mwx_softc sc; sc.sc_ic.ic_softc=&sc;
    sc.sc_vif.idx=2; sc.sc_vif.wmm_idx=3;
    assert(mt7921_mcu_set_tx(&sc,&sc.sc_vif)==0); checkDefault();
    ieee80211_node ap; ap.ni_flags=IEEE80211_NODE_QOS;
    sc.sc_ic.ic_bss=&ap; sc.sc_ic.ic_flags=IEEE80211_F_QOS;
    for(unsigned ac=0;ac<4;++ac) sc.sc_ic.ic_edca_ac[ac]={uint8_t(2+ac),uint8_t(6+ac),uint8_t(3+ac),uint16_t(17+ac),uint8_t(ac&1)};
    assert(mt7921_mcu_set_tx(&sc,&sc.sc_vif)==0&&message[40]==2&&message[41]==1&&message[42]==3);
    for(unsigned ac=0;ac<4;++ac) {
        // net80211 ACI order is already the firmware's BE/BK/VI/VO order.
        assert(word(ac*10)==((1u<<(2+ac))-1)&&word(ac*10+2)==((1u<<(6+ac))-1));
        assert(word(ac*10+4)==17+ac&&word(ac*10+6)==3+ac&&message[ac*10+9]==(ac&1));
    }
    sc.sc_ic.ic_edca_ac[0].ac_ecwmin=0; sc.sc_ic.ic_edca_ac[0].ac_ecwmax=255;
    assert(mt7921_mcu_set_tx(&sc,&sc.sc_vif)==0&&word(0)==5&&word(2)==32767);
    mwx_vif secondary; secondary.idx=4; assert(mt7921_mcu_set_tx(&sc,&secondary)==0); checkDefault();
    sc.sc_awdl_vif.idx=5; sc.sc_awdl_vif.wmm_idx=1;
    assert(mt7921_mcu_set_tx(&sc,&sc.sc_awdl_vif)==0&&message[40]==5&&message[41]==1&&message[42]==1);
    for(unsigned ac=0;ac<4;++ac) {
        assert(word(ac*10)==3&&word(ac*10+2)==7&&word(ac*10+4)==94&&word(ac*10+6)==2);
        assert(message[ac*10+8]==0&&message[ac*10+9]==0);
    }
    awdlEdcaBootArg=0;
    assert(mt7921_mcu_set_tx(&sc,&sc.sc_awdl_vif)==0&&message[40]==5&&message[41]==0&&message[42]==1); checkDefault();
    awdlEdcaBootArg=-1;
    ap.ni_flags=0; assert(mt7921_mcu_set_tx(&sc,&sc.sc_vif)==0); checkDefault();
    sendError=EIO; assert(mt7921_mcu_set_tx(&sc,&sc.sc_vif)==EIO); sendError=0;
    // More than one WCID wrap must not touch the associated AP's WTBL entry.
    for(unsigned i=0;i<MWX_WTBL_STA*3;++i) {
        auto ni=mwx_node_alloc(&sc.sc_ic); assert(ni&&clears==0);
        auto node=reinterpret_cast<mwx_node *>(ni); assert(node->wcid>0&&node->wcid<MWX_WTBL_STA); std::free(node);
    }
    auto selected=mwx_node_alloc(&sc.sc_ic); assert(selected);
    mwx_newassoc(&sc.sc_ic,selected,1); assert(clears==0);
    selected->ni_associd=17; mwx_newassoc(&sc.sc_ic,selected,0); assert(clears==0);
    mwx_newassoc(&sc.sc_ic,selected,1); assert(clears==1&&clearedWcid==reinterpret_cast<mwx_node *>(selected)->wcid); std::free(selected);
    allocationFails=true; assert(!mwx_node_alloc(&sc.sc_ic)&&clears==1);
    std::puts("EDCA: negotiated ACI/CW/TXOP/AIFS/ACM, secondary/non-QoS defaults, transport error and scan WCID wrap passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-wifi-edca-') as directory:
    cpp = Path(directory) / 'edca.cpp'
    binary = cpp.with_suffix('')
    cpp.write_text(code)
    subprocess.run(['/usr/bin/clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
