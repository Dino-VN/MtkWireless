#!/usr/bin/env python3
"""Run production hotspot RSN, descriptor, AWDL admission and key-batch logic."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
v = root / 'kext/Wifi/vendor'
net = v / 'mtk80211/openbsd/net80211'
hal = (v / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()
out = (net / 'ieee80211_output.c').read_text()
pae = (net / 'ieee80211_pae_input.c').read_text()
crypto = (net / 'ieee80211_crypto.h').read_text()

def function(s, signature):
    a = s.index(signature)
    return s[a:s.index('\n}\n', a) + 3]

def enum(name):
    a = crypto.index('enum ' + name)
    return crypto[a:crypto.index('\n};', a) + 3]

# Real 802.11 constants and AKM predicates, not test-specific replacements.
constants = ''
for p in [net / 'ieee80211.h', net / 'ieee80211_node.h', net / 'ieee80211_var.h']:
    for line in p.read_text().splitlines():
        if re.match(r'#define\s+(IEEE80211_(?:RSNCAP_|NODE_(?:MFP|PMKID|TXMGMTPROT|RXMGMTPROT)|C_MFP|F_(?:MFPR|PBAR)|PROTO_|PMKID_LEN|OUI)|MICROSOFT_OUI)', line):
            constants += line + '\n'
start = pae.index('    /* discard EAPOL-Key frames with an unknown descriptor version */')
desc = pae[start:pae.index('    /* make sure the key data', start)]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <vector>
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define DPRINTF(...) ((void)0)
#define MWX_DEV_LOG(...) ((void)0)
#define IEEE80211_M_STA 1
#define IEEE80211_M_MONITOR 2
#define IEEE80211_S_SCAN 1
#define IEEE80211_S_AUTH 2
#define IEEE80211_S_ASSOC 3
#define IEEE80211_S_RUN 4
#define IEEE80211_F_RSNON 1
#define MWX_NODE_FLAG_HAVE_PAIRWISE_KEY 1
#define MWX_NODE_FLAG_HAVE_GROUP_KEY 2
#define MWX_NODE_FLAG_KEY_FAILED 4
#define IEEE80211_KEY_GROUP 1
#define IEEE80211_NODE_TXRXPROT 0x3000
#define LINK_STATE_UP 1
#define MCU_UNI_CMD_STA_REC_UPDATE 1
#define EIO 5
#define EBUSY 16
#define ENXIO 6
#define ENOBUFS 55
#define EAPOL_KEY_DESC_V0 0
#define EAPOL_KEY_DESC_V1 1
#define EAPOL_KEY_DESC_V2 2
#define EAPOL_KEY_DESC_V3 3
#define EAPOL_KEY_VERSION_MASK 7
uint64_t now=100;
void microuptime(timeval*tv){tv->tv_sec=0;tv->tv_usec=++now;}
void LE_WRITE_2(uint8_t*p,uint16_t n){p[0]=n;p[1]=n>>8;}
void panic(const char*){assert(false);}
''' + constants + enum('ieee80211_cipher') + enum('ieee80211_akm') + \
    function(crypto, 'static __inline int\nieee80211_is_sha256_akm(') + \
    function(crypto, 'static __inline int\nieee80211_is_sae_akm(') + \
    (net / 'mtk_join_diag.h').read_text() + r'''
struct ieee80211_node {
    unsigned ni_flags=0,ni_rsnprotos=IEEE80211_PROTO_RSN,ni_rsnakms=IEEE80211_AKM_PSK;
    unsigned ni_rsnciphers=IEEE80211_CIPHER_CCMP,ni_rsncaps=0x8c;
    ieee80211_cipher ni_rsncipher=IEEE80211_CIPHER_CCMP,ni_rsngroupcipher=IEEE80211_CIPHER_CCMP;
    uint8_t ni_pmkid[16]{};bool ni_port_valid=false;
};
struct mwx_node : ieee80211_node { unsigned flags=0; };
struct ieee80211_key {int k_cipher=0;unsigned k_flags=0;int k_id=0;};
struct ieee80211com {
    int ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_SCAN;
    unsigned ic_flags=IEEE80211_F_RSNON,ic_caps=IEEE80211_C_MFP;
    ieee80211_cipher ic_rsngroupmgmtcipher=IEEE80211_CIPHER_BIP;
    mtk_join_diag ic_join_diag{};ieee80211_node*ic_bss=nullptr;
};
struct mbuf {};
struct mwx_setkey_task_arg {ieee80211_node*ni;ieee80211_key*k;};
struct mwx_softc {
    ieee80211com sc_ic;int sc_ns_state=IEEE80211_S_SCAN;
    mwx_setkey_task_arg sc_setkey_arg[8]{};unsigned sc_setkey_tail=0,sc_setkey_cur=0;
};
using mwx_vif=int;
int splnet(){return 0;} void splx(int){}
unsigned links=0,completions=0;int failedCipher=-1,sendError=0,waitError=0;uint32_t fwStatus=0;
void ieee80211_set_link_state(ieee80211com*ic,int){assert(ic->ic_bss->ni_port_valid);++links;}
int mwx_mcu_send_mbuf(mwx_softc*,unsigned,mbuf*,int*seq){*seq=2;return sendError;}
int mwx_mcu_wait_resp_int(mwx_softc*,unsigned,int seq,uint32_t*status){assert(seq==2);*status=fwStatus;return waitError;}
int mt7921_mcu_sta_key_update(mwx_softc*sc,ieee80211_node*ni,ieee80211_key*k){
    assert(!ni->ni_port_valid); // The port must stay closed through IGTK's wait.
    ++completions;
    if(k->k_cipher==failedCipher)return EIO;
    auto mn=static_cast<mwx_node*>(ni);
    if(k->k_cipher!=IEEE80211_CIPHER_BIP)
        mn->flags |= k->k_flags & IEEE80211_KEY_GROUP ? 2 : 1;
    return 0;
}
''' + function(out, 'u_int8_t *\nieee80211_add_rsn_body(') + \
    function(hal, 'static bool mwx_awdl_infra_joining(') + \
    function(hal, 'static void mwx_join_key_result(') + \
    function(hal, 'static int mwx_join_key_command(') + \
    function(hal, 'void\nmwx_setkey_task(') + r'''
bool acceptsDescriptor(ieee80211com*ic,ieee80211_node*ni,unsigned info){
    unsigned desc;
''' + desc + r'''
    return true;
done: return false;
}
int main(){
    mwx_softc sc; mwx_node peer; auto&ic=sc.sc_ic;ic.ic_bss=&peer;
    ic.ic_join_diag.Active=1;
    // An MFPC transition AP with a WCL PMK must get exactly PSK (suite 2),
    // CCMP and MFPC, not SAE/SHA256 merely because PMF is enabled.
    uint8_t rsn[257]{};
    auto end=ieee80211_add_rsn_body(rsn,&ic,&peer,0);
    assert(end-rsn==26 && rsn[17]==2 && rsn[18]==0x8c && rsn[25]==6);
    for(auto akm:{IEEE80211_AKM_PSK,IEEE80211_AKM_SHA256_PSK,IEEE80211_AKM_SAE}){
        peer.ni_rsnakms=akm;
        for(unsigned version=0;version<8;++version){
            unsigned expected=akm==IEEE80211_AKM_PSK?2:akm==IEEE80211_AKM_SAE?0:3;
            for(unsigned pmf:{0u,unsigned(IEEE80211_NODE_MFP)}){
                peer.ni_flags=pmf;
                assert(acceptsDescriptor(&ic,&peer,version)==(version==expected));
            }
        }
    }
    peer.ni_rsnakms=IEEE80211_AKM_PSK;
    end=ieee80211_add_rsn_body(rsn,&ic,&peer,1);
    assert(end-rsn==18 && rsn[3]==0x50 && rsn[4]==0xf2 && rsn[17]==2);
    ic.ic_caps=0;peer.ni_rsnakms=IEEE80211_AKM_PSK;peer.ni_flags=0;
    end=ieee80211_add_rsn_body(rsn,&ic,&peer,0);assert(end-rsn==20 && rsn[18]==0x0c);
    ic.ic_caps=IEEE80211_C_MFP;peer.ni_flags=IEEE80211_NODE_PMKID;
    end=ieee80211_add_rsn_body(rsn,&ic,&peer,0);assert(end-rsn==42 && rsn[20]==1);
    // Check queued AUTH/ASSOC and RUN setup while ic_state still says SCAN.
    for(int ns:{IEEE80211_S_AUTH,IEEE80211_S_ASSOC,IEEE80211_S_RUN}){
        sc.sc_ns_state=ns;assert(mwx_awdl_infra_joining(&sc));
    }
    sc.sc_ns_state=IEEE80211_S_SCAN;assert(!mwx_awdl_infra_joining(&sc));
    ic.ic_state=IEEE80211_S_RUN;sc.sc_ns_state=IEEE80211_S_RUN;
    assert(mwx_awdl_infra_joining(&sc));peer.ni_port_valid=true;
    assert(!mwx_awdl_infra_joining(&sc));peer.ni_port_valid=false;
    // Original key_info flags classify M1..M4, including WPA M4 without Secure.
    for(unsigned ver=0;ver<=3;++ver){
        assert(mtk_join_message(0x88|ver,0)==1);
        assert(mtk_join_message(0x108|ver,26)==2);
        assert(mtk_join_message(0x13c8|ver,80)==3);
        assert(mtk_join_message(0x108|ver,0)==4);
        assert(mtk_join_message(0x308|ver,0)==4);
        assert(mtk_join_message(0x808|ver,0)==0);
    }
    mtk_join_eapol(&ic.ic_join_diag,false,0x8a,0,3,1);
    mtk_join_eapol(&ic.ic_join_diag,true,0x10a,26,4,2);
    assert(ic.ic_join_diag.EapolRxM1==1 && ic.ic_join_diag.EapolRxState==3);
    assert(ic.ic_join_diag.EapolTxM2==1 && ic.ic_join_diag.EapolTxDescriptor==2);
    auto count=ic.ic_join_diag.EapolRxM1;ic.ic_join_diag.Active=0;
    mtk_join_eapol(&ic.ic_join_diag,false,0x8a,0,4,2);assert(ic.ic_join_diag.EapolRxM1==count);
    {   // A scan-only failure must not replace the associated one.
        mtk_join_diag a{},scanOnly{},failed{},assocFailed{};
        a.Attempt=1;a.WCLLinkUpAtUS=5;a.EapolRxM1=0;scanOnly.Attempt=2;
        mtk_join_record_failure(&a,&failed,&assocFailed);
        mtk_join_record_failure(&scanOnly,&failed,&assocFailed);
        assert(failed.Attempt==2 && assocFailed.Attempt==1);
    }
    {   // Associated + RUN with no EAPOL at all is the hotspot stall.
        mtk_join_diag s{};s.AssocRxCount=1;s.RunCommittedAtUS=7;s.UpperAuth=8;
        assert(mtk_join_eapol_stall(&s));
        s.EapolRawAtUS=9;assert(!mtk_join_eapol_stall(&s));
        s.EapolRawAtUS=0;s.AssocStatus=17;assert(!mtk_join_eapol_stall(&s));
        s.AssocStatus=0;s.PortValidAtUS=8;assert(!mtk_join_eapol_stall(&s));
        mtk_join_diag scanOnly{};assert(!mtk_join_eapol_stall(&scanOnly));
        // An open join has no EAPOL by design and must not be a stall.
        mtk_join_diag open{};open.AssocRxCount=1;open.RunCommittedAtUS=7;
        assert(!mtk_join_eapol_stall(&open));
    }
    ic.ic_join_diag.Active=1;
    {   // Association subtype zero must not count ordinary data or Start.
        auto &d=ic.ic_join_diag;d.Attempt=17;
        for(uint8_t fc:{uint8_t(0x08),uint8_t(0x88)}){
            assert(mtk_join_tx_kind(fc,false,false)==MTK_JOIN_TX_NONE);
            assert(mtk_join_tx_kind(fc,true,false)==MTK_JOIN_TX_EAPOL);
            assert(mtk_join_tx_kind(fc,true,true)==MTK_JOIN_TX_NONE);
        }
        assert(mtk_join_tx_kind(0xb0,false,false)==MTK_JOIN_TX_AUTH);
        assert(mtk_join_tx_kind(0x00,false,false)==MTK_JOIN_TX_ASSOC);
        assert(mtk_join_tx_kind(0x20,false,false)==MTK_JOIN_TX_ASSOC);
        assert(mtk_join_tx_kind(0x40,false,false)==MTK_JOIN_TX_NONE);
        assert(mtk_join_tx_kind(0xc0,false,false)==MTK_JOIN_TX_NONE);
        assert(mtk_join_tx_kind(0xd4,false,false)==MTK_JOIN_TX_NONE);
        mtk_join_tx_dma(&d,MTK_JOIN_TX_ASSOC,0);
        mtk_join_tx_dma(&d,MTK_JOIN_TX_EAPOL,0);
        mtk_join_tx_dma(&d,MTK_JOIN_TX_EAPOL,ENOBUFS);
        assert(d.AssocDMA==1 && d.EapolTxDMA==1 && d.EapolTxDMAErrors==1);
        mtk_join_tx_complete(&d,17,MTK_JOIN_TX_AUTH,0,1,0x10001);
        mtk_join_tx_complete(&d,17,MTK_JOIN_TX_ASSOC,0,2,0x20002);
        mtk_join_tx_complete(&d,17,MTK_JOIN_TX_EAPOL,2,15,0x3400f);
        assert(d.AuthTxAcked==1 && d.AssocTxAcked==1 && d.EapolTxFailed==1);
        assert(d.AssocTxLastCount==2 && d.EapolTxLastStatus==2);
        assert(d.EapolTxLastCount==15 && d.EapolTxLastRaw==0x3400f);
        auto at=d.EapolTxCompletedAtUS;
        // A delayed completion, an unclassified token, or a closed attempt
        // cannot overwrite the current attempt's last TX result.
        mtk_join_tx_complete(&d,16,MTK_JOIN_TX_EAPOL,0,1,1);
        mtk_join_tx_complete(&d,17,MTK_JOIN_TX_NONE,0,1,1);
        d.Active=0;mtk_join_tx_complete(&d,17,MTK_JOIN_TX_EAPOL,0,1,1);
        assert(d.EapolTxAcked==0 && d.EapolTxCompletedAtUS==at);
        d.Active=1;mtk_join_tx_complete(&d,17,MTK_JOIN_TX_EAPOL,0,1,1);
        assert(d.EapolTxAcked==1 && d.EapolTxFailed==1);
    }
    // Firmware failure is distinct from transport completion/error.
    mbuf m;assert(mwx_join_key_command(&sc,&m)==0);
    fwStatus=9;assert(mwx_join_key_command(&sc,&m)==EIO && ic.ic_join_diag.KeyFirmwareStatus==9);
    sendError=ENOBUFS;assert(mwx_join_key_command(&sc,&m)==ENOBUFS);
    sendError=0;fwStatus=0;waitError=EBUSY;assert(mwx_join_key_command(&sc,&m)==EBUSY);waitError=0;
    // Queue drains through IGTK before opening the port. A failed IGTK cannot
    // be hidden by a completed PTK/GTK; a pending leave cannot reopen it.
    ieee80211_key keys[]={{IEEE80211_CIPHER_CCMP,0,0},{IEEE80211_CIPHER_CCMP,IEEE80211_KEY_GROUP,0},{IEEE80211_CIPHER_BIP,0,4}};
    for(int fail:{-1,int(IEEE80211_CIPHER_BIP),int(IEEE80211_CIPHER_CCMP)}){
        peer.flags=0;peer.ni_flags=IEEE80211_NODE_MFP;peer.ni_port_valid=false;
        sc.sc_setkey_tail=0;sc.sc_setkey_cur=3;failedCipher=fail;links=completions=0;
        for(unsigned i=0;i<3;++i)sc.sc_setkey_arg[i]={&peer,&keys[i]};
        mwx_setkey_task(&sc);
        assert(completions==3 && links==(fail==-1?1u:0u));
        assert(peer.ni_port_valid==(fail==-1));
        if(fail==-1)assert(peer.ni_flags&IEEE80211_NODE_RXMGMTPROT);
    }
    // Normal WPA2 uses only PTK/GTK; legacy WPA installs PTK first and
    // obtains its GTK later through the group handshake. Both still authorize.
    failedCipher=-1;
    for(bool split:{false,true}){
        peer.flags=0;peer.ni_flags=0;peer.ni_port_valid=false;links=0;
        sc.sc_setkey_tail=0;sc.sc_setkey_cur=split?1:2;
        for(unsigned i=0;i<2;++i)sc.sc_setkey_arg[i]={&peer,&keys[i]};
        mwx_setkey_task(&sc);
        if(split){assert(!peer.ni_port_valid && links==0);sc.sc_setkey_cur=2;mwx_setkey_task(&sc);}
        assert(peer.ni_port_valid && links==1);
    }
    peer.flags=0;peer.ni_port_valid=false;sc.sc_setkey_tail=0;sc.sc_setkey_cur=3;
    failedCipher=-1;sc.sc_ns_state=IEEE80211_S_SCAN;links=0;
    for(unsigned i=0;i<3;++i)sc.sc_setkey_arg[i]={&peer,&keys[i]};
    mwx_setkey_task(&sc);assert(links==0 && !peer.ni_port_valid);
    puts("PASS: hotspot PSK/PMF RSN, descriptor matrix, AWDL pending states, key status/batch barrier and join-scoped TX ACK diagnostics");
}
'''
# Pin actual hook placement, not just helper behavior.
assert 'memcpy(ic->ic_firmware_mac, dev_req.tlv.omac_addr, 6)' in hal
auth = function(hal, 'void\nmwx_newstate_task(')
assert auth.index('rv = mwx_awdl_abort(sc)') < auth.index('rv = mt7921_set_channel(sc)')
assert 'ni->ni_port_valid = 1' not in function(hal, 'int\nmt7921_mcu_sta_key_update(')
rx=(net/'ieee80211_input.c').read_text()
assert rx.index('if (!mtk_wlan_eapol_observe(') < rx.index('    sta_rx = ieee80211_accept_sta_frame(')
tx = function(hal, 'int\nmwx_tx(')
assert tx.index('mt->mt_join_attempt =') < tx.index('rv = mwx_dma_txwi_enqueue(')
free = function(hal, 'void\nmwx_mac_tx_free(')
assert free.index('mtk_join_tx_complete(') < free.index('mwx_txwi_put(')
assert 'mt->mt_join_attempt = 0' in function(hal, 'void\nmwx_txwi_put(')
with tempfile.TemporaryDirectory(prefix='mtk-hotspot-') as tmp:
    p=Path(tmp);(p/'test.cpp').write_text(code)
    subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-Wall','-Wextra',
                    '-Wno-unused-parameter',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
