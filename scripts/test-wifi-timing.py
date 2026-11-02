#!/usr/bin/env python3
"""Exercise the production band/slot callback and its actual MMIO writes."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
net = vendor / 'mtk80211/openbsd/net80211'
hal = (vendor / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()

def function(signature):
    start = hal.index(signature)
    return hal[start:hal.index('\n}\n', start) + 3]

def definitions(path, names):
    lines = path.read_text().splitlines()
    result = ''
    for i, line in enumerate(lines):
        match = re.match(r'#define\s+(\w+)', line)
        if not match or match[1] not in names:
            continue
        result += line + '\n'
        while line.endswith('\\'):
            i += 1
            line = lines[i]
            result += line + '\n'
    return result

registers = definitions(vendor / 'SimpleMtkWlan/hal_mwx/if_mwxreg.h', {
    'MT_BAND_BASE0', 'MT_BAND_BASE1', 'MT_BAND_OFF', 'MT_BAND_ADDR',
    'MT_TMAC_CDTR', 'MT_TMAC_ODTR', 'MT_TMAC_ICR0',
    'MT_TIMEOUT_CCK_DEF_VAL', 'MT_TIMEOUT_OFDM_DEF_VAL',
    'MT_IFS_EIFS_DEF', 'MT_IFS_RIFS_DEF', 'MT_IFS_SIFS_MASK',
    'MT_IFS_SIFS_SHIFT', 'MT_IFS_SLOT_MASK', 'MT_IFS_SLOT_SHIFT',
    'MT_AGG_ACR0', 'MT_AGG_ACR_CFEND_RATE_MASK',
    'MT7921_CFEND_RATE_DEFAULT', 'MT7921_CFEND_RATE_11B',
    'MT_ARB_SCR', 'MT_ARB_SCR_TX_DISABLE', 'MT_ARB_SCR_RX_DISABLE',
})
constants = definitions(net / 'ieee80211_var.h', {
    'IEEE80211_CHAN_2GHZ', 'IEEE80211_CHAN_5GHZ', 'IEEE80211_CHAN_6GHZ',
    'IEEE80211_CHAN_ANYC', 'IEEE80211_IS_CHAN_2GHZ', 'IEEE80211_IS_CHAN_6GHZ',
    'IEEE80211_F_SHSLOT',
}) + definitions(net / 'ieee80211.h', {
    'IEEE80211_DUR_DS_SHSLOT', 'IEEE80211_DUR_DS_SLOT',
})
diagnostics = (net / 'mtk_join_diag.h').read_text()
# The timing routine needs the production layout, not unrelated PAE helpers.
diagnostics = diagnostics[:diagnostics.index('/* MtkWireless: a failed attempt.')]
diagnostics += '#endif\n'
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>
#include <sys/time.h>
#define IEEE80211_M_STA 1
#define IFF_RUNNING 0x40
uint64_t now=0;
void microuptime(timeval *tv){tv->tv_sec=0;tv->tv_usec=++now;}
''' + registers + constants + diagnostics + r'''
struct ieee80211_channel {unsigned ic_flags;};
struct ieee80211_node {ieee80211_channel *ni_chan;};
struct ifnet {unsigned if_flags=0;void *if_softc=nullptr;};
struct ieee80211com {
    int ic_opmode=IEEE80211_M_STA;unsigned ic_flags=0;
    ieee80211_node *ic_bss=nullptr;ieee80211_channel *ic_ibss_chan=nullptr;
    ifnet ic_if;mtk_join_diag ic_join_diag{};
};
struct mwx_softc {ieee80211com sc_ic;std::map<uint32_t,uint32_t> regs;};
std::vector<unsigned> operations;unsigned pause=MT_ARB_SCR_TX_DISABLE|MT_ARB_SCR_RX_DISABLE;
void mwx_set(mwx_softc *sc,uint32_t reg,uint32_t value){assert(reg==MT_ARB_SCR(0));sc->regs[reg]|=value;operations.push_back(1);}
void delay(unsigned us){assert(us==1);operations.push_back(2);}
void mwx_write(mwx_softc *sc,uint32_t reg,uint32_t value){assert((sc->regs[MT_ARB_SCR(0)]&pause)==pause);sc->regs[reg]=value;operations.push_back(3);}
void mwx_rmw(mwx_softc *sc,uint32_t reg,uint32_t value,uint32_t mask){assert((sc->regs[MT_ARB_SCR(0)]&pause)==pause);sc->regs[reg]=(sc->regs[reg]&~mask)|(value&mask);operations.push_back(4);}
void mwx_clear(mwx_softc *sc,uint32_t reg,uint32_t value){assert(reg==MT_ARB_SCR(0));sc->regs[reg]&=~value;operations.push_back(5);}
uint32_t mwx_read(mwx_softc *sc,uint32_t reg){assert((sc->regs[MT_ARB_SCR(0)]&pause)==0);operations.push_back(6);return sc->regs[reg];}
''' + function('void\nmt7921_mac_set_timing(') + function('void\nmwx_update_slot(') + r'''
int main(){
    mwx_softc sc;auto &ic=sc.sc_ic;ic.ic_if.if_softc=&sc;
    ieee80211_channel channels[]={{IEEE80211_CHAN_2GHZ},{IEEE80211_CHAN_5GHZ},{IEEE80211_CHAN_6GHZ}};
    ieee80211_node bss{&channels[1]};ic.ic_bss=&bss;ic.ic_ibss_chan=&channels[0];
    ic.ic_join_diag.Active=1;
    // Standard band/ERP values, including 5/6 GHz with short-slot flag clear.
    for(unsigned band=0;band<3;++band)for(bool shortslot:{false,true}){
        bss.ni_chan=&channels[band];ic.ic_flags=shortslot?IEEE80211_F_SHSLOT:0;
        sc.regs[MT_AGG_ACR0(0)]=0x80000000;operations.clear();
        mt7921_mac_set_timing(&sc);
        unsigned sifs=band?16:10,slot=band||shortslot?9:20;
        uint32_t icr=sc.regs[MT_TMAC_ICR0(0)];
        assert(((icr&MT_IFS_SIFS_MASK)>>MT_IFS_SIFS_SHIFT)==sifs);
        assert(((icr&MT_IFS_SLOT_MASK)>>MT_IFS_SLOT_SHIFT)==slot);
        assert(sc.regs[MT_TMAC_CDTR(0)]==MT_TIMEOUT_CCK_DEF_VAL);
        assert(sc.regs[MT_TMAC_ODTR(0)]==MT_TIMEOUT_OFDM_DEF_VAL);
        assert(sc.regs[MT_AGG_ACR0(0)]==(0x80000000|(slot==20?MT7921_CFEND_RATE_11B:MT7921_CFEND_RATE_DEFAULT)));
        assert((operations==std::vector<unsigned>{1,2,3,3,3,4,5,6}));
        auto &d=ic.ic_join_diag;
        assert(d.MacTimingBand==band && d.MacTimingSIFSUS==sifs && d.MacTimingSlotUS==slot);
        assert(d.MacTimingICR0==icr && d.MacTimingICR0Readback==icr);
    }
    // A STA uses its BSS channel, not the unrelated IBSS fallback.
    bss.ni_chan=&channels[1];ic.ic_flags=0;mt7921_mac_set_timing(&sc);
    assert(ic.ic_join_diag.MacTimingBand==1);
    ic.ic_bss=nullptr;mt7921_mac_set_timing(&sc);
    assert(ic.ic_join_diag.MacTimingBand==0 && ic.ic_join_diag.MacTimingSlotUS==20);
    ic.ic_bss=&bss;ic.ic_opmode=2;mt7921_mac_set_timing(&sc);
    assert(ic.ic_join_diag.MacTimingBand==0);
    ic.ic_opmode=IEEE80211_M_STA;
    // Invalid channels must not touch hardware or clobber retained diagnostics.
    for(auto channel:{static_cast<ieee80211_channel *>(nullptr),IEEE80211_CHAN_ANYC}){
        bss.ni_chan=channel;operations.clear();mt7921_mac_set_timing(&sc);
        assert(operations.empty() && ic.ic_join_diag.MacTimingSlotUS==20);
    }
    // Association and later ERP updates change slot time without a channel switch.
    bss.ni_chan=&channels[0];operations.clear();mwx_update_slot(&ic);
    assert(operations.empty()); // Interface not running.
    ic.ic_if.if_flags=IFF_RUNNING;
    for(bool shortslot:{true,false,true}){
        ic.ic_flags=shortslot?IEEE80211_F_SHSLOT:0;mwx_update_slot(&ic);
        assert(ic.ic_join_diag.MacTimingSlotUS==(shortslot?9u:20u));
    }
    puts("PASS: production Wi-Fi timing MMIO, 2/5/6 GHz SIFS, ERP slots, callback and readback");
}
'''
assert 'ic->ic_updateslot = mwx_update_slot;' in hal
state = function('void\nmwx_newstate_task(')
run = state[state.index('    case IEEE80211_S_RUN:'):]
assert run.index('mt7921_mcu_set_associated_peer(sc)') < run.index('mt7921_mac_set_timing(sc)') < run.index('mt7921_mcu_set_rts_thresh')
with tempfile.TemporaryDirectory(prefix='mtk-wifi-timing-') as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
