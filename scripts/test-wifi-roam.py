#!/usr/bin/env python3
"""Check WCL roaming wire formats, BSS switching and PMF auth retry."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'


def definition(path, signature):
    source = (vendor / path).read_text()
    start = source.index(signature + '\n{')
    return source[start:source.index('\n}\n', start) + 3]


code = r'''
#include <cassert>
#include <cstdio>
#include "WCL.hpp"
int main() {
    using namespace AirportWCL;
    uint8_t buf[0xbc] = {};
    buf[0] = 36; buf[1] = 0xd0; buf[2] = 6; buf[3] = 0x10;   // chanspecs 36, 6
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(buf + 0x64, mac, 6);
    buf[0x90] = 1; buf[0x94] = 2; buf[0xbb] = 4; memcpy(buf + 0x9b, "home", 4);
    ReassocRequest r;
    assert(decodeReassoc(buf, sizeof(buf), r));
    assert(r.bssidCount == 1 && r.channelCount == 2 && r.ssidLength == 4);
    assert(!memcmp(r.bssid, mac, 6) && !memcmp(r.ssid, "home", 4));
    assert(r.channels[36 / 8] & (1 << (36 % 8)) && r.channels[0] & (1 << 6));
    assert(!decodeReassoc(buf, 0xbb, r));                    // short buffer
    buf[0x64] = 1; assert(!decodeReassoc(buf, sizeof(buf), r));   // multicast
    buf[0x64] = 2; buf[0x94] = 51; assert(!decodeReassoc(buf, sizeof(buf), r));
    buf[0x94] = 0; buf[0x90] = 8; assert(!decodeReassoc(buf, sizeof(buf), r));
    buf[0x90] = 0; buf[0xbb] = 33; assert(!decodeReassoc(buf, sizeof(buf), r));
    buf[0xbb] = 0; memset(buf + 0x64, 0, 6);
    assert(decodeReassoc(buf, sizeof(buf), r) && r.bssidCount == 0);
    RoamPeer from = {}, to = {};
    from.rssi = -72; from.channel = 6; from.bssid[0] = 1;
    to.rssi = -55; to.channel = 149; to.bssid[0] = 6; to.phyMode = 0x100;
    RoamStatus st;
    makeRoamStatus(0, RoamReasonLowRssi, 100, 90, from, to, st);
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&st);
    assert(st.endMs == 100 && p[0x20] == uint8_t(-72) && p[0x2c] == 149);
    assert(p[0x58] == 1 && p[0x5e] == 6 && p[0x55] == 1 && p[4] == 0x01 && p[7] == 0xe0);
    puts("PASS: WCL_REASSOC decode, ROAM_START/PREP/REASSOC/ROAM_DONE layouts");
}
'''

roam_task = definition('SimpleMtkWlan/hal_mwx/MtkMwx.cpp', 'mwx_bgscan_done_task(void *arg)')
release_node = definition('mtk80211/openbsd/net80211/ieee80211_node.c',
                          'ieee80211_release_node(struct ieee80211com *ic, struct ieee80211_node *ni)')
auth_open = definition('mtk80211/openbsd/net80211/ieee80211_proto.c',
                       'ieee80211_auth_open(struct ieee80211com *ic, const struct ieee80211_frame *wh,\n'
                       '    struct ieee80211_node *ni, struct ieee80211_rxinfo *rxi, u_int16_t seq,\n'
                       '    u_int16_t status)')
watchdog = definition('mtk80211/openbsd/net80211/ieee80211.c', 'ieee80211_watchdog(struct _ifnet *ifp)')
handoff = r'''
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

enum {
    IEEE80211_S_RUN = 4, IEEE80211_F_BGSCAN = 1, IEEE80211_F_RSNON = 2,
    IEEE80211_NODE_TXRXPROT = 1, IEEE80211_NODE_TXMGMTPROT = 2,
    IEEE80211_NODE_RXMGMTPROT = 4, IEEE80211_NODE_MFP = 8,
    IEEE80211_STA_COLLECT = 7, IEEE80211_CIPHER_NONE = 0,
    IEEE80211_CIPHER_CCMP = 1, IEEE80211_KEY_GROUP = 1,
    IEEE80211_KEY_IGTK = 2, RSNA_SUPP_INITIALIZE = 0, M_DEVBUF = 0,
    IEEE80211_M_STA = 1, IEEE80211_S_SCAN = 1, IEEE80211_S_AUTH = 2,
    IEEE80211_S_ASSOC = 3, IEEE80211_F_AUTO_JOIN = 4,
    IEEE80211_STATUS_TRY_AGAIN_LATER = 30, IEEE80211_AUTH_OPEN_REQUEST = 1,
    IEEE80211_AUTH_OPEN_RESPONSE = 2, IEEE80211_FC0_SUBTYPE_AUTH = 0xb0,
    IEEE80211_FC0_SUBTYPE_MASK = 0xf0, IFF_DEBUG = 1,
};
enum ieee80211_akm { AKM_OPEN, AKM_SAE };
struct _ifnet { int if_flags = 0, if_timer = 0; };
struct ieee80211_frame { unsigned i_fc[2] = {}; char i_addr2[6] = {}, i_addr3[6] = {}; };
struct ieee80211_rxinfo {};
struct ieee80211com;
struct ieee80211_key { int k_cipher = 0, k_flags = 0; };
struct ieee80211_node {
    ieee80211_key ni_pairwise_key;
    unsigned ni_refcnt = 0;
    int ni_flags = 0, ni_state = 0, ni_rsn_supp_state = 1;
    int ni_rsnakms = AKM_OPEN, ni_fails = 0;
    char ni_bssid[6] = {}, ni_macaddr[6] = {};
    bool ni_replaycnt_ok = false;
    bool ni_port_valid = true;
    void *ni_unref_arg = nullptr;
    size_t ni_unref_arg_size = 0;
    void (*ni_unref_cb)(ieee80211com *, ieee80211_node *) = nullptr;
};
struct ieee80211com {
    _ifnet ic_if;
    ieee80211_node *ic_bss = nullptr;
    int ic_state = IEEE80211_S_RUN, ic_flags = IEEE80211_F_BGSCAN;
    int ic_opmode = IEEE80211_M_STA, ic_mgt_timer = 0;
    bool ic_auth_comeback_pending = false, ic_auth_comeback_retried = false;
    struct { int is_rx_bad_auth = 0, is_rx_auth_fail = 0; } ic_stats;
    int ic_def_txkey = 0, ic_igtk_kid = 0;
    ieee80211_key ic_nw_keys[6];
    void (*ic_delete_key)(ieee80211com *, ieee80211_node *, ieee80211_key *) = nullptr;
};
struct mwx_softc {
    ieee80211com sc_ic;
    void *sc_bgscan_arg = nullptr;
    size_t sc_bgscan_arg_size = 0;
};
int splnet() { return 0; }
void splx(int) {}
#define DPRINTF(args) ((void)0)
#define XYLog(...) ((void)0)
#define IEEE80211_STA_ONLY 1
#define IEEE80211_ADDR_EQ(a, b) (!std::memcmp(a, b, 6))
#define ISSET(flags, test) ((flags) & (test))
void free(void *p, int, size_t) { std::free(p); }
unsigned ieee80211_node_decref(ieee80211_node *ni) { assert(ni->ni_refcnt); return --ni->ni_refcnt; }
void ieee80211_ref_node(ieee80211_node *ni) { ++ni->ni_refcnt; }
void ieee80211_free_node(ieee80211com *, ieee80211_node *) { assert(false); }
unsigned switches = 0;
void ieee80211_node_switch_bss(ieee80211com *ic, ieee80211_node *ni) {
    assert(ni->ni_refcnt == 0 && ni->ni_unref_arg && ni->ni_unref_arg_size == 1);
    if (ic->ic_flags & IEEE80211_F_RSNON) {
        assert(!ni->ni_port_valid && ni->ni_rsn_supp_state == RSNA_SUPP_INITIALIZE);
        assert(!(ni->ni_flags & IEEE80211_NODE_TXRXPROT));
    }
    ++switches;
    std::free(ni->ni_unref_arg);
}
void
''' + release_node + '\nvoid\n' + roam_task + r'''

bool ieee80211_is_sae_akm(ieee80211_akm akm) { return akm == AKM_SAE; }
void ieee80211_try_another_bss(ieee80211com *ic) { ++ic->ic_bss->ni_fails; }
void ieee80211_new_state(ieee80211com *ic, int state, int) { ic->ic_state = state; }
ieee80211_node *ieee80211_find_node(ieee80211com *, const char *) { return nullptr; }
void ieee80211_node_join_bss(ieee80211com *, ieee80211_node *) { assert(false); }
void ieee80211_deselect_ess(ieee80211com *) { assert(false); }
unsigned auth_requests = 0;
int IEEE80211_SEND_MGMT(ieee80211com *ic, ieee80211_node *node, int subtype, int arg) {
    assert(ic->ic_bss == node && subtype == IEEE80211_FC0_SUBTYPE_AUTH && arg == IEEE80211_AUTH_OPEN_REQUEST);
    ic->ic_mgt_timer = 5;
    ++auth_requests;
    return 0;
}
void
''' + auth_open + '\nvoid\n' + watchdog + r'''

void test_auth_comeback() {
    ieee80211com ic;
    ieee80211_node node;
    ieee80211_frame response;
    ic.ic_bss = &node;
    ic.ic_state = IEEE80211_S_AUTH;
    node.ni_flags = IEEE80211_NODE_MFP;
    auto reject = [&] {
        ieee80211_auth_open(&ic, &response, &node, nullptr, IEEE80211_AUTH_OPEN_RESPONSE,
                           IEEE80211_STATUS_TRY_AGAIN_LATER);
    };
    reject();
    assert(ic.ic_auth_comeback_pending && ic.ic_mgt_timer == 2 && !node.ni_fails);
    ieee80211_watchdog(&ic.ic_if);
    reject(); // Duplicate response must leave the original retry deadline alone.
    assert(ic.ic_mgt_timer == 1 && !node.ni_fails && !auth_requests);
    ieee80211_watchdog(&ic.ic_if);
    assert(auth_requests == 1 && !ic.ic_auth_comeback_pending && ic.ic_auth_comeback_retried);
    reject();
    assert(node.ni_fails == 1 && ic.ic_stats.is_rx_auth_fail == 1 && !ic.ic_auth_comeback_pending);
    ic.ic_auth_comeback_retried = false;
    node.ni_flags = 0;
    reject();
    assert(node.ni_fails == 2 && !ic.ic_auth_comeback_pending && auth_requests == 1);
    puts("PASS: PMF Open System status 30 retries the same AP once without extending the delay");
}

int main() {
    for (unsigned pmf = 0; pmf <= 1; ++pmf) {
        for (unsigned queued = 0; queued <= 2; ++queued) {
            mwx_softc sc;
            ieee80211_node node;
            sc.sc_ic.ic_bss = &node;
            sc.sc_bgscan_arg = std::malloc(1);
            sc.sc_bgscan_arg_size = 1;
            node.ni_refcnt = queued;
            if (pmf) {
                sc.sc_ic.ic_flags |= IEEE80211_F_RSNON;
                node.ni_flags = IEEE80211_NODE_MFP | IEEE80211_NODE_TXRXPROT;
            }
            switches = 0;
            mwx_bgscan_done_task(&sc);
            assert(!sc.sc_bgscan_arg && sc.sc_bgscan_arg_size == 0);
            assert(switches == (queued == 0 ? 1u : 0u));
            for (unsigned i = queued; i; --i) {
                ieee80211_release_node(&sc.sc_ic, &node);
                assert(switches == (i == 1 ? 1u : 0u));
            }
            assert(switches == 1 && !node.ni_unref_cb && !node.ni_unref_arg);
            assert(node.ni_refcnt == 0);
        }
    }
    puts("PASS: BSS switch runs once whether DMA submission precedes or follows the roam task");
    test_auth_comeback();
}
'''

with tempfile.TemporaryDirectory(prefix='mtk-wifi-roam-') as tmp:
    src = Path(tmp) / 'test.cpp'
    src.write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-fsanitize=address,undefined',
                    '-I', str(root / 'kext/include'), '-I', str(root / 'kext/Wifi'),
                    '-I', str(root / 'kext/Wifi/vendor/AirportMtkWlan/WCL'),
                    '-Wl,-U,_mtk_wlan_scan_ssids_parse',
                    str(src), '-o', str(Path(tmp) / 'test')], check=True)
    subprocess.run([str(Path(tmp) / 'test')], check=True)
    src.write_text(handoff)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-fsanitize=address,undefined',
                    str(src), '-o', str(Path(tmp) / 'handoff')], check=True)
    subprocess.run([str(Path(tmp) / 'handoff')], check=True)
