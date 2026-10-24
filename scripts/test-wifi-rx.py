#!/usr/bin/env python3
"""Exercise production STA RX admission with neighboring AP/client traffic."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kext/Wifi/vendor/mtk80211/openbsd/net80211/ieee80211_input.c').read_text()
start = source.index('static int\nieee80211_accept_sta_frame(')
helper = source[start:source.index('\n}\n', start) + 3]
# Admission must precede reordering, sequence tracking and RSSI mutation.
body = source[source.index('void\nieee80211_inputm('):]
call = body.index('sta_rx = ieee80211_accept_sta_frame(')
assert body.index('if (mbuf_len(m) < hdrlen)') < call
assert call < body.index('ieee80211_input_ba(ic,')
assert call < body.index('*orxseq = nrxseq;')
assert call < body.index('ni->ni_rssi =')
assert 'if (sta_rx == 1 && ieee80211_has_seq(wh)' in body
assert 'if (sta_rx == 1 && ic->ic_state > IEEE80211_S_SCAN)' in body
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#define IEEE80211_M_STA 1
#define IEEE80211_FC0_TYPE_MGT 0
#define IEEE80211_FC0_TYPE_CTL 4
#define IEEE80211_FC0_TYPE_DATA 8
#define IEEE80211_FC0_SUBTYPE_BEACON 0x80
#define IEEE80211_FC0_SUBTYPE_PROBE_RESP 0x50
#define IEEE80211_FC1_DIR_FROMDS 2
#define IEEE80211_IS_MULTICAST(a) ((a)[0] & 1)
#define IEEE80211_ADDR_EQ(a,b) (memcmp(a,b,6)==0)
struct ieee80211_frame { uint8_t i_addr1[6], i_addr2[6], i_addr3[6]; };
struct ieee80211_node { uint8_t ni_bssid[6]; };
struct ieee80211com { int ic_opmode; uint8_t ic_myaddr[6]; ieee80211_node *ic_bss; };
''' + helper + r'''
int main() {
    uint8_t me[6]={2,0,0,0,0,1}, ap[6]={2,0,0,0,1,1};
    uint8_t other[6]={2,0,0,0,2,1}, peer[6]={2,0,0,0,0,2};
    uint8_t group[6]={255,255,255,255,255,255};
    ieee80211_node bss; memcpy(bss.ni_bssid,ap,6);
    ieee80211com ic{IEEE80211_M_STA,{},&bss}; memcpy(ic.ic_myaddr,me,6);
    auto frame = [&](const uint8_t *ra,const uint8_t *ta,const uint8_t *bssid) {
        ieee80211_frame f; memcpy(f.i_addr1,ra,6); memcpy(f.i_addr2,ta,6);
        memcpy(f.i_addr3,bssid,6); return f;
    };
    auto accept = [&](const uint8_t *ra,const uint8_t *ta,const uint8_t *bssid,
                      int type=0,int subtype=0xa0,int dir=0) {
        auto f=frame(ra,ta,bssid);
        return ieee80211_accept_sta_frame(&ic,&f,type,subtype,dir);
    };
    // Real AP deauth/disassoc/auth/assoc/action must keep working.
    for (int subtype : {0xa0,0xc0,0xb0,0x10,0x30,0xd0}) {
        assert(accept(me,ap,ap,0,subtype));
        assert(accept(group,ap,ap,0,subtype));
        assert(!accept(peer,ap,ap,0,subtype));
        assert(!accept(me,other,other,0,subtype));
        assert(!accept(group,other,other,0,subtype));
        assert(!accept(me,ap,other,0,subtype));
    }
    // Scan cache still sees every AP's beacon and probe response.
    for (int subtype : {0x80,0x50}) {
        assert(accept(group,other,other,0,subtype)==2);
        assert(accept(me,other,other,0,subtype)==2);
    }
    // EAPOL M1/M3 to us and group traffic pass; traffic for another
    // station, other APs, and overheard M2/M4 (ToDS) cannot touch our BSS.
    assert(accept(me,ap,peer,8,0x80,2));
    assert(accept(group,ap,peer,8,0x80,2));
    assert(!accept(peer,ap,me,8,0x80,2));
    assert(!accept(me,other,peer,8,0x80,2));
    assert(!accept(ap,peer,me,8,0x80,1));
    assert(!accept(me,ap,peer,8,0x80,1));
    // A neighbor's BAR must not advance our reorder window.
    assert(accept(me,ap,ap,4,0x80));
    assert(!accept(peer,ap,ap,4,0x80));
    assert(!accept(me,other,other,4,0x80));
    // The same helper leaves non-STA modes alone.
    ic.ic_opmode=2;
    assert(accept(peer,other,other));
    puts("Wi-Fi RX admission regression tests passed");
}
'''
# initializer_list is needed for range-for initializer lists.
code = '#include <initializer_list>\n' + code
with tempfile.TemporaryDirectory(prefix='mtk-wifi-rx-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-g',str(path / 'test.cpp'),
                    '-o',str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')],check=True)
