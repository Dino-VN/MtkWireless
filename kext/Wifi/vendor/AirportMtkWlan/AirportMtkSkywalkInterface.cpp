//
//  AirportMtkSkywalkInterface.cpp
//  AirportMtkWlan-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//
#include "AirportMtkWlan.hpp"
#include "CountryLocale.hpp"
#include "AirportMtkSkywalkInterface.hpp"
#include <sys/CTimeout.hpp>
#include <libkern/c++/OSMetaClass.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_priv.h>

extern IOCommandGate *_fCommandGate;

#define super IO80211InfraProtocol
OSDefineMetaClassAndStructors(AirportMtkSkywalkInterface, IO80211InfraProtocol);

IOReturn AirportMtkSkywalkInterface::setVIRTUAL_IF_CREATE(apple80211_virt_if_create_data *request)
{
#ifdef MTK_NATIVE_AWDL
    auto controller=OSDynamicCast(AirportMtkWlan,getController());
    return controller ? controller->createAwdlInterfaceRequest(request) : kIOReturnNotReady;
#else
    return kIOReturnUnsupported;
#endif
}

IOReturn AirportMtkSkywalkInterface::getHT_CAPABILITY(apple80211_ht_capability *d)
{
    if(!d) return kIOReturnBadArgument;
    auto *ic=current80211();
    if(!ic) return kIOReturnNotReady;
    bzero(d,sizeof(*d));
    d->version=APPLE80211_VERSION; d->hc_id=45; d->hc_len=26;
    d->hc_cap=ic->ic_htcaps; d->hc_param=ic->ic_ampdu_params;
    memcpy(d->hc_mcsset,ic->ic_sup_mcs,sizeof(ic->ic_sup_mcs));
    d->hc_extcap=ic->ic_htxcaps; d->hc_txbf=ic->ic_txbfcaps;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::getVHT_CAPABILITY(apple80211_vht_capability *d)
{
    if(!d) return kIOReturnBadArgument;
    auto *ic=current80211();
    if(!ic) return kIOReturnNotReady;
    bzero(d,sizeof(*d)); d->version=APPLE80211_VERSION;
    d->cap=ic->ic_vhtcaps & 0xffff; d->unk1=ic->ic_vhtcaps;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::getHE_CAPABILITY(apple80211_he_capability *d)
{
    if(!d) return kIOReturnBadArgument;
    // The Tahoe SDK intentionally leaves this private payload opaque. The
    // AWDL manager only requires a successful capability query here; the
    // infrastructure capability bitmap already advertises HE support.
    uint8_t *raw=reinterpret_cast<uint8_t *>(d);
    // IO80211AWDLPeerManager supplies a 0x28-byte HE IE buffer.
    bzero(raw,0x28); *reinterpret_cast<uint32_t *>(raw)=APPLE80211_VERSION;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::getSIB_COEX_STATUS(apple80211_sib_coex_status *d)
{
    if(!d) return kIOReturnBadArgument;
    bzero(reinterpret_cast<uint8_t *>(d),16);
    return kIOReturnSuccess;
}

static int ieeeChanFlag2appleScanFlagVentura(int flags)
{
    int ret = 0;
    if (flags & IEEE80211_CHAN_2GHZ)
        ret |= APPLE80211_C_FLAG_2GHZ;
    if (flags & IEEE80211_CHAN_5GHZ)
        ret |= APPLE80211_C_FLAG_5GHZ;
    if (flags & IEEE80211_CHAN_6GHZ)
        ret |= APPLE80211_C_FLAG_6GHZ;
    ret |= APPLE80211_C_FLAG_20MHZ;
    if (!(flags & IEEE80211_CHAN_PASSIVE) || !(flags & IEEE80211_CHAN_6GHZ))
        ret |= APPLE80211_C_FLAG_ACTIVE;
    return ret;
}

static int ieeeChanFlag2apple(int flags, int bw)
{
    int ret = 0;
    if (flags & IEEE80211_CHAN_2GHZ)
        ret |= APPLE80211_C_FLAG_2GHZ;
    if (flags & IEEE80211_CHAN_5GHZ)
        ret |= APPLE80211_C_FLAG_5GHZ;
    if (flags & IEEE80211_CHAN_6GHZ)
        ret |= APPLE80211_C_FLAG_6GHZ;
    if (!(flags & IEEE80211_CHAN_PASSIVE))
        ret |= APPLE80211_C_FLAG_ACTIVE;
    if (flags & IEEE80211_CHAN_DFS)
        ret |= APPLE80211_C_FLAG_DFS;
    if (bw == -1) {
        if (flags & IEEE80211_CHAN_VHT) {
            if ((flags & IEEE80211_CHAN_VHT160) || (flags & IEEE80211_CHAN_VHT80_80))
                ret |= APPLE80211_C_FLAG_160MHZ;
            if (flags & IEEE80211_CHAN_VHT80)
                ret |= APPLE80211_C_FLAG_80MHZ;
        } else if ((flags & IEEE80211_CHAN_HT40) && (flags & IEEE80211_CHAN_HT)) {
            ret |= APPLE80211_C_FLAG_40MHZ;
            if (flags & IEEE80211_CHAN_HT40U)
                ret |= APPLE80211_C_FLAG_EXT_ABV;
        } else if (flags & IEEE80211_CHAN_HT20)
            ret |= APPLE80211_C_FLAG_20MHZ;
        else if ((flags & IEEE80211_CHAN_CCK) || (flags & IEEE80211_CHAN_OFDM))
            ret |= APPLE80211_C_FLAG_10MHZ;
    } else {
        switch (bw) {
            case IEEE80211_CHAN_WIDTH_80P80:
            case IEEE80211_CHAN_WIDTH_160:
                ret |= APPLE80211_C_FLAG_160MHZ;
                break;
            case IEEE80211_CHAN_WIDTH_80:
                ret |= APPLE80211_C_FLAG_80MHZ;
                break;
            case IEEE80211_CHAN_WIDTH_40:
                ret |= APPLE80211_C_FLAG_40MHZ;
                if (flags & IEEE80211_CHAN_HT40U)
                    ret |= APPLE80211_C_FLAG_EXT_ABV;
                break;
            case IEEE80211_CHAN_WIDTH_20:
                ret |= APPLE80211_C_FLAG_20MHZ;
                break;
            default:
                if (flags & IEEE80211_CHAN_HT20)
                    ret |= APPLE80211_C_FLAG_20MHZ;
                else if ((flags & IEEE80211_CHAN_CCK) || (flags & IEEE80211_CHAN_OFDM))
                    ret |= APPLE80211_C_FLAG_10MHZ;
                break;
        }
    }
    return ret;
}

void AirportMtkSkywalkInterface::setPTK(const u_int8_t *key, size_t key_len) {
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node    * ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
    ni->ni_rsn_supp_state = RNSA_SUPP_PTKDONE;
    
    if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP) {
        u_int64_t prsc;
        
        /* check that key length matches that of pairwise cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsncipher);
        if (key_len != keylen) {
            XYLog("PTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return;
        }
        prsc = /*(gtk == NULL) ? LE_READ_6(key->rsc) :*/ 0;
        
        /* map PTK to 802.11 key */
        k = &ni->ni_pairwise_key;
        memset(k, 0, sizeof(*k));
        k->k_cipher = ni->ni_rsncipher;
        k->k_rsc[0] = prsc;
        k->k_len = keylen;
        memcpy(k->k_key, key, k->k_len);
        /* install the PTK */
        if ((*ic->ic_set_key)(ic, ni, k) != 0) {
            XYLog("setting PTK failed\n");
            return;
        }
        else
            XYLog("setting PTK successfully\n");
        ni->ni_flags &= ~IEEE80211_NODE_RSN_NEW_PTK;
        ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
        ni->ni_flags |= IEEE80211_NODE_RXPROT;
    } else if (ni->ni_rsncipher != IEEE80211_CIPHER_USEGROUP)
        XYLog("%s: unexpected pairwise key update received from %s\n",
              ic->ic_if.if_xname, ether_sprintf(ni->ni_macaddr));
}

void AirportMtkSkywalkInterface::setGTK(const u_int8_t *gtk, size_t key_len, u_int8_t kid, u_int8_t *rsc) {
    struct ieee80211com *ic = fHalService->get80211Controller();
    struct ieee80211_node    * ni = ic->ic_bss;
    struct ieee80211_key *k;
    int keylen;
    
    if (gtk != NULL) {
        /* check that key length matches that of group cipher */
        keylen = ieee80211_cipher_keylen(ni->ni_rsngroupcipher);
        if (key_len != keylen) {
            XYLog("GTK length mismatch. expected %d, got %zu\n", keylen, key_len);
            return;
        }
        /* map GTK to 802.11 key */
        k = &ic->ic_nw_keys[kid];
        if (k->k_cipher == IEEE80211_CIPHER_NONE || k->k_len != keylen || memcmp(k->k_key, gtk, keylen) != 0) {
            memset(k, 0, sizeof(*k));
            k->k_id = kid;    /* 0-3 */
            k->k_cipher = ni->ni_rsngroupcipher;
            k->k_flags = IEEE80211_KEY_GROUP;
            //if (gtk[6] & (1 << 2))
            //  k->k_flags |= IEEE80211_KEY_TX;
            k->k_rsc[0] = LE_READ_6(rsc);
            k->k_len = keylen;
            memcpy(k->k_key, gtk, k->k_len);
            /* install the GTK */
            if ((*ic->ic_set_key)(ic, ni, k) != 0) {
                XYLog("setting GTK failed\n");
                return;
            }
            else
                XYLog("setting GTK successfully\n");
        }
    }
    
    if (true) {
        ni->ni_flags |= IEEE80211_NODE_TXRXPROT;
#ifndef IEEE80211_STA_ONLY
        if (ic->ic_opmode != IEEE80211_M_IBSS ||
            ++ni->ni_key_count == 2)
#endif
        {
            XYLog("marking port %s valid\n",
                  ether_sprintf(ni->ni_macaddr));
            ni->ni_port_valid = 1;
            ieee80211_set_link_state(ic, LINK_STATE_UP);
            ni->ni_assoc_fail = 0;
            if (ic->ic_opmode == IEEE80211_M_STA)
                ic->ic_rsngroupcipher = ni->ni_rsngroupcipher;
        }
    }
}

bool AirportMtkSkywalkInterface::
init(IOService *provider)
{
    bool ret = IO80211InfraInterface::init();
    if (!ret) {
        XYLog("%s IO80211InfraInterface init failed\n", __PRETTY_FUNCTION__);
        return false;
    }
    instance = OSDynamicCast(AirportMtkWlan, provider);
    if (!instance)
        return false;
    this->fHalService = instance->fHalService;
    if (!fHalService) return false;
    fHalService->retain();
    this->scanSource = instance->scanSource;
    if (scanSource) scanSource->retain();
    IOEthernetAddress address;
    if (instance->getHardwareAddress(&address) != kIOReturnSuccess)
        return false;
    memcpy(wclPermanentMAC, &address, sizeof(wclPermanentMAC));
    if (!IO80211SkywalkInterface::init(provider, reinterpret_cast<ether_addr *>(&address)))
        return false;
    if (!initWCL())
        return false;
    return ret;
}

//ifnet_t AirportMtkSkywalkInterface::
//getBSDInterface()
//{
//    if (instance->bsdInterface)
//        return instance->bsdInterface->getIfnet();
//    return NULL;
//}

ieee80211com *AirportMtkSkywalkInterface::current80211() const
{
    if (wclStopping || !fHalService)
        return nullptr;
    return fHalService->get80211Controller();
}

IOReturn AirportMtkSkywalkInterface::
getSSID(struct apple80211_ssid_data *sd)
{
    if (!sd) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !ic->ic_bss) return kIOReturnNotReady;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(sd, 0, sizeof(*sd));
        sd->version = APPLE80211_VERSION;
        sd->ssid_len = MIN(ic->ic_bss->ni_esslen, sizeof(sd->ssid_bytes));
        memcpy(sd->ssid_bytes, ic->ic_bss->ni_essid, sd->ssid_len);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getAUTH_TYPE(struct apple80211_authtype_data *ad)
{
    ad->version = APPLE80211_VERSION;
    ad->authtype_lower = current_authtype_lower;
    ad->authtype_upper = current_authtype_upper;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
setAUTH_TYPE(struct apple80211_authtype_data *ad)
{
    current_authtype_lower = ad->authtype_lower;
    current_authtype_upper = ad->authtype_upper;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
setCIPHER_KEY(struct apple80211_key *key)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(8), key);
}

IOReturn AirportMtkSkywalkInterface::
getPHY_MODE(struct apple80211_phymode_data *pd)
{
    if (!pd) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic) return kIOReturnNotReady;
    
    pd->version = APPLE80211_VERSION;
    pd->phy_mode = APPLE80211_MODE_11A
    | APPLE80211_MODE_11B
    | APPLE80211_MODE_11G
    | APPLE80211_MODE_11N;
    
    if (ic->ic_flags & IEEE80211_F_VHTON)
        pd->phy_mode |= APPLE80211_MODE_11AC;
    
    if (ic->ic_flags & IEEE80211_F_HEON)
        pd->phy_mode |= APPLE80211_MODE_11AX;
    
    switch (ic->ic_curmode) {
        case IEEE80211_MODE_AUTO:
            pd->active_phy_mode = APPLE80211_MODE_AUTO;
            break;
        case IEEE80211_MODE_11A:
            pd->active_phy_mode = APPLE80211_MODE_11A;
            break;
        case IEEE80211_MODE_11B:
            pd->active_phy_mode = APPLE80211_MODE_11B;
            break;
        case IEEE80211_MODE_11G:
            pd->active_phy_mode = APPLE80211_MODE_11G;
            break;
        case IEEE80211_MODE_11N:
            pd->active_phy_mode = APPLE80211_MODE_11N;
            break;
        case IEEE80211_MODE_11AC:
            pd->active_phy_mode = APPLE80211_MODE_11AC;
            break;
        case IEEE80211_MODE_11AX:
            pd->active_phy_mode = ic->ic_state == IEEE80211_S_RUN && ic->ic_bss &&
                (ic->ic_bss->ni_flags & IEEE80211_NODE_HE) ? APPLE80211_MODE_11AX : APPLE80211_MODE_AUTO;
            break;
            
        default:
            pd->active_phy_mode = APPLE80211_MODE_AUTO;
            break;
    }
    
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getCHANNEL(struct apple80211_channel_data *cd)
{
    if (!cd) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !ic->ic_bss || !ic->ic_bss->ni_chan ||
        ic->ic_bss->ni_chan == IEEE80211_CHAN_ANYC) return kIOReturnNotReady;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(cd, 0, sizeof(apple80211_channel_data));
        cd->version = APPLE80211_VERSION;
        cd->channel.version = APPLE80211_VERSION;
        cd->channel.channel = ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan);
        cd->channel.flags = ieeeChanFlag2apple(ic->ic_bss->ni_chan->ic_flags, ic->ic_bss->ni_chw);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getSTATE(struct apple80211_state_data *sd)
{
    memset(sd, 0, sizeof(*sd));
    sd->version = APPLE80211_VERSION;
    sd->state = fHalService->get80211Controller()->ic_state;
    auto controller = static_cast<AirportMtkWlan *>(getController());
    __atomic_store_n(&controller->wifiReportedState, uint32_t(sd->state), __ATOMIC_RELAXED);
    __atomic_add_fetch(&controller->wifiStateQueryCount, 1u, __ATOMIC_RELAXED);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getMCS_INDEX_SET(struct apple80211_mcs_index_set_data *ad)
{
    if (!ad) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !ic->ic_bss) return kIOReturnNotReady;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(ad, 0, sizeof(*ad));
        ad->version = APPLE80211_VERSION;
        size_t size = min(ARRAY_SIZE(ic->ic_bss->ni_rxmcs), ARRAY_SIZE(ad->mcs_set_map));
        for (int i = 0; i < size; i++)
            ad->mcs_set_map[i] = ic->ic_bss->ni_rxmcs[i];
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getVHT_MCS_INDEX_SET(struct apple80211_vht_mcs_index_set_data *data)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_bss == NULL || ic->ic_curmode != IEEE80211_MODE_11AC) {
        return kIOReturnError;
    }
    memset(data, 0, sizeof(struct apple80211_vht_mcs_index_set_data));
    data->version = APPLE80211_VERSION;
    data->mcs_map = ic->ic_bss->ni_vht_mcsinfo.tx_mcs_map;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getMCS_VHT(struct apple80211_mcs_vht_data *data)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_bss == NULL || ic->ic_curmode != IEEE80211_MODE_11AC) {
        return kIOReturnError;
    }
    memset(data, 0, sizeof(struct apple80211_mcs_vht_data));
    data->version = APPLE80211_VERSION;
    data->guard_interval = (ieee80211_node_supports_vht_sgi80(ic->ic_bss) || ieee80211_node_supports_vht_sgi160(ic->ic_bss)) ? APPLE80211_GI_SHORT : APPLE80211_GI_LONG;
    data->index = ic->ic_bss->ni_txmcs;
    data->nss = fHalService->getDriverInfo()->getTxNSS();
    switch (ic->ic_bss->ni_chw) {
        case IEEE80211_CHAN_WIDTH_40:
            data->bw = 40;
            break;
        case IEEE80211_CHAN_WIDTH_80:
            data->bw = 80;
            break;
        case IEEE80211_CHAN_WIDTH_80P80:
        case IEEE80211_CHAN_WIDTH_160:
            data->bw = 160;
            break;
            
        default:
            data->bw = 20;
            break;
    }
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getRATE_SET(struct apple80211_rate_set_data *ad)
{
    if (!ad) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !ic->ic_bss) return kIOReturnNotReady;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(ad, 0, sizeof(*ad));
        ad->version = APPLE80211_VERSION;
        size_t size = MIN(ic->ic_bss->ni_rates.rs_nrates,
            MIN(ARRAY_SIZE(ad->rates), ARRAY_SIZE(ic->ic_bss->ni_rates.rs_rates)));
        ad->num_rates = size;
        for (int i=0; i < size; i++) {
            struct apple80211_rate &apple_rate = ad->rates[i];
            apple_rate.version = APPLE80211_VERSION;
            apple_rate.rate = ic->ic_bss->ni_rates.rs_rates[i];
            apple_rate.flags = 0;
        }
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getOP_MODE(struct apple80211_opmode_data *od)
{
    od->version = APPLE80211_VERSION;
    od->op_mode = APPLE80211_M_STA;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getTXPOWER(struct apple80211_txpower_data *txd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(txd, 0, sizeof(*txd));
        txd->version = APPLE80211_VERSION;
        txd->txpower = ic->ic_txpower;
        txd->txpower_unit = APPLE80211_UNIT_PERCENT;
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getRATE(struct apple80211_rate_data *rd)
{
    if (!rd) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !ic->ic_bss) return kIOReturnNotReady;
    int nss;
    int sgi;
    int index = 0;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(rd, 0, sizeof(*rd));
        rd->version = APPLE80211_VERSION;
        rd->num_radios = 1;
        sgi = ieee80211_node_supports_sgi(ic->ic_bss);
        if (ic->ic_curmode == IEEE80211_MODE_11AC) {
            if (sgi)
                index += 1;
            /* ni_vht_ss comes from firmware TX status once data flows. */
            nss = ic->ic_bss->ni_vht_ss > 0 ? ic->ic_bss->ni_vht_ss :
                fHalService->getDriverInfo()->getTxNSS();
            nss = nss > 1 ? 2 : 1;
            switch (ic->ic_bss->ni_chw) {
                case IEEE80211_CHAN_WIDTH_40:
                    index += 4;
                    break;
                case IEEE80211_CHAN_WIDTH_80:
                    index += 8;
                    break;
                case IEEE80211_CHAN_WIDTH_80P80:
                case IEEE80211_CHAN_WIDTH_160:
                    index += 12;
                    break;

                default:
                    break;
            }
            index += 2 * (nss - 1);
            const struct ieee80211_vht_rateset *rs = &ieee80211_std_ratesets_11ac[index];
            rd->rate[0] = rs->rates[ic->ic_bss->ni_txmcs % rs->nrates] / 2;
        } else if (ic->ic_curmode == IEEE80211_MODE_11AX) {
            /* HE rates with the 0.8 us guard interval. */
            nss = ic->ic_bss->ni_vht_ss > 0 ? ic->ic_bss->ni_vht_ss :
                fHalService->getDriverInfo()->getTxNSS();
            nss = nss > 1 ? 2 : 1;
            switch (ic->ic_bss->ni_chw) {
                case IEEE80211_CHAN_WIDTH_40:
                    index = IEEE80211_HE_RATESET_SISO_40;
                    break;
                case IEEE80211_CHAN_WIDTH_80:
                    index = IEEE80211_HE_RATESET_SISO_80;
                    break;
                case IEEE80211_CHAN_WIDTH_80P80:
                case IEEE80211_CHAN_WIDTH_160:
                    index = IEEE80211_HE_RATESET_SISO_160;
                    break;

                default:
                    index = IEEE80211_HE_RATESET_SISO;
                    break;
            }
            index += nss - 1;
            const struct ieee80211_he_rateset *rs = &ieee80211_std_ratesets_11ax[index];
            rd->rate[0] = rs->rates[ic->ic_bss->ni_txmcs % rs->nrates] / 2;
        } else if (ic->ic_curmode == IEEE80211_MODE_11N) {
            int is_40mhz = ic->ic_bss->ni_chw == IEEE80211_CHAN_WIDTH_40;
            if (sgi)
                index += 1;
            if (is_40mhz)
                index += (IEEE80211_HT_RATESET_MIMO4_SGI + 1);
            const unsigned mcs = MIN(ic->ic_bss->ni_txmcs, 15);
            nss = mcs / 8 + 1;
            index += 2 * (nss - 1);
            rd->rate[0] = ieee80211_std_ratesets_11n[index].rates[mcs % 8] / 2;
        } else {
            if (ic->ic_bss->ni_txrate >= MIN(ic->ic_bss->ni_rates.rs_nrates,
                ARRAY_SIZE(ic->ic_bss->ni_rates.rs_rates))) return kIOReturnNotReady;
            rd->rate[0] = ic->ic_bss->ni_rates.rs_rates[ic->ic_bss->ni_txrate];
        }
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getBSSID(struct apple80211_bssid_data *bd)
{
    if (!bd) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !ic->ic_bss) return kIOReturnNotReady;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(bd, 0, sizeof(*bd));
        bd->version = APPLE80211_VERSION;
        memcpy(bd->bssid.octet, ic->ic_bss->ni_bssid, APPLE80211_ADDR_LEN);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getRSSI(struct apple80211_rssi_data *rd)
{
    if (!rd) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !ic->ic_bss) return kIOReturnNotReady;
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(rd, 0, sizeof(*rd));
        rd->num_radios = 1;
        rd->rssi_unit = APPLE80211_UNIT_DBM;
        rd->rssi[0] = rd->aggregate_rssi
        = rd->rssi_ext[0]
        = rd->aggregate_rssi_ext
        = -(0 - MTK_MIN_DBM - ic->ic_bss->ni_rssi);
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getRSN_IE(struct apple80211_rsn_ie_data *data)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(10), data);
}

IOReturn AirportMtkSkywalkInterface::
setRSN_IE(struct apple80211_rsn_ie_data *data)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(9), data);
}

IOReturn AirportMtkSkywalkInterface::
getAP_IE_LIST(struct apple80211_ap_ie_data *data)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (!data)
        return kIOReturnError;
    if (ic->ic_bss == NULL || ic->ic_bss->ni_rsnie_tlv == NULL || ic->ic_bss->ni_rsnie_tlv_len == 0 || ic->ic_bss->ni_rsnie_tlv_len > data->len || ic->ic_bss->ni_rsnie_tlv_len > 1024)
        return kIOReturnError;
    data->version = APPLE80211_VERSION;
    data->len = ic->ic_bss->ni_rsnie_tlv_len;
    memcpy(data->ie_data, ic->ic_bss->ni_rsnie_tlv, data->len);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getNOISE(struct apple80211_noise_data *nd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state == IEEE80211_S_RUN) {
        memset(nd, 0, sizeof(*nd));
        nd->version = APPLE80211_VERSION;
        nd->num_radios = 1;
        nd->noise[0]
        = nd->aggregate_noise = -fHalService->getDriverInfo()->getBSSNoise();
        nd->noise_unit = APPLE80211_UNIT_DBM;
        return kIOReturnSuccess;
    }
    return 6;
}

IOReturn AirportMtkSkywalkInterface::
getPOWERSAVE(struct apple80211_powersave_data *pd)
{
    pd->version = APPLE80211_VERSION;
    pd->powersave_level = APPLE80211_POWERSAVE_MODE_DISABLED;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getNSS(struct apple80211_nss_data *data)
{
    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->nss = fHalService->getDriverInfo()->getTxNSS();
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
setASSOCIATE(struct apple80211_assoc_data *)
{
    // Tahoe joins belong to WCL_ASSOCIATE. The legacy format bypasses its
    // bounds checks, security negotiation and per-attempt state.
    return kIOReturnUnsupported;
}

IOReturn AirportMtkSkywalkInterface::
setDISASSOCIATE(struct apple80211_disassoc_data *ad)
{
    XYLog("%s\n", __FUNCTION__);
    struct ieee80211com *ic = fHalService->get80211Controller();

    if (ic->ic_state < IEEE80211_S_SCAN)
        return kIOReturnSuccess;
    
    if (ic->ic_state > IEEE80211_S_AUTH && ic->ic_bss != NULL)
        IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH, IEEE80211_REASON_AUTH_LEAVE);
    
    if (ic->ic_state == IEEE80211_S_ASSOC || ic->ic_state == IEEE80211_S_AUTH)
        return kIOReturnSuccess;
    
    disassocIsVoluntary = true;

    ieee80211_del_ess(ic, nullptr, 0, 1);
    ieee80211_deselect_ess(ic);
#ifdef USE_APPLE_SUPPLICANT
    ic->ic_rsn_ie_override[1] = 0;
#endif
    ic->ic_assoc_status = APPLE80211_STATUS_UNAVAILABLE;
    ic->ic_deauth_reason = APPLE80211_REASON_ASSOC_LEAVING;
    ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getSUPPORTED_CHANNELS(struct apple80211_sup_channel_data *ad)
{
    if (!ad)
        return kIOReturnError;
    ad->version = APPLE80211_VERSION;
    ad->num_channels = 0;
    struct ieee80211com *ic = fHalService->get80211Controller();
    for (int i = 0; i < IEEE80211_CHAN_MAX &&
         ad->num_channels < APPLE80211_MAX_CHANNELS; i++) {
        if (ic->ic_channels[i].ic_freq != 0) {
            ad->supported_channels[ad->num_channels].channel = ieee80211_chan2ieee(ic, &ic->ic_channels[i]);
            ad->supported_channels[ad->num_channels].flags = ieeeChanFlag2appleScanFlagVentura(ic->ic_channels[i].ic_flags);
            ad->num_channels++;
        }
    }
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getLOCALE(struct apple80211_locale_data *ld)
{
    if (!ld)
        return kIOReturnError;
    ld->version = APPLE80211_VERSION;
    // MtkWireless (PR #7): follow the reported country code (ETSI in the EU).
    // Tahoe's native IOC 28 handler derives the locale from COUNTRY_CODE
    // itself; this legacy method only matters to older callers.
    apple80211_country_code_data country = {};
    int locale = APPLE80211_LOCALE_FCC;
    if (instance && instance->getCOUNTRY_CODE(static_cast<OSObject *>(this), &country) == kIOReturnSuccess)
        locale = mtkcc::locale(reinterpret_cast<const char *>(country.cc));
    ld->locale  = static_cast<decltype(ld->locale)>(locale);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getDEAUTH(struct apple80211_deauth_data *da)
{
    if (!da)
        return kIOReturnError;
    da->version = APPLE80211_VERSION;
    struct ieee80211com *ic = fHalService->get80211Controller();
    da->deauth_reason = ic->ic_deauth_reason;
//    XYLog("%s, %d\n", __FUNCTION__, da->deauth_reason);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getASSOCIATION_STATUS(struct apple80211_assoc_status_data *hv)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    if (!hv)
        return kIOReturnError;
    memset(hv, 0, sizeof(*hv));
    hv->version = APPLE80211_VERSION;
    if (ic->ic_state == IEEE80211_S_RUN)
        hv->status = APPLE80211_STATUS_SUCCESS;
    else
        hv->status = APPLE80211_STATUS_UNAVAILABLE;
//    XYLog("%s, %d\n", __FUNCTION__, hv->status);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
setSCANCACHE_CLEAR(void *req)
{
    XYLog("%s\n", __FUNCTION__);
    struct ieee80211com *ic = fHalService->get80211Controller();
    //if doing background or active scan, don't free nodes.
    if ((ic->ic_flags & IEEE80211_F_BGSCAN) || (ic->ic_flags & IEEE80211_F_ASCAN))
        return kIOReturnSuccess;
    ieee80211_free_allnodes(ic, 0);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
setDEAUTH(struct apple80211_deauth_data *da)
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getMCS(struct apple80211_mcs_data* md)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (ic->ic_state != IEEE80211_S_RUN ||  ic->ic_bss == NULL || !md)
        return 6;
    md->version = APPLE80211_VERSION;
    md->index = ic->ic_bss->ni_txmcs;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getLINK_CHANGED_EVENT_DATA(struct apple80211_link_changed_event_data *ed)
{
    if (!ed) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || !instance) return kIOReturnNotReady;
    
    bzero(ed, sizeof(apple80211_link_changed_event_data));
    ed->isLinkDown = !(instance->currentStatus & kIONetworkLinkActive);
    if (ed->isLinkDown) {
        ed->voluntary = disassocIsVoluntary;
        ed->reason = APPLE80211_LINK_DOWN_REASON_DEAUTH;
    } else {
        if (!ic->ic_bss) return kIOReturnNotReady;
        ed->rssi = -(0 - MTK_MIN_DBM - ic->ic_bss->ni_rssi);
    }
    XYLog("Link %s, reason: %d, voluntary: %d\n", ed->isLinkDown ? "down" : "up", ed->reason, ed->voluntary);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
setSCAN_REQ(struct apple80211_scan_data *)
{
    // Only WCL_SCAN_REQ owns the scan timer and completion on Tahoe.
    return kIOReturnUnsupported;
}

extern OSDictionary *convertScanToDictionary(apple80211_scan_result *a1);

static int convertNodeToScanResult(MtkHalService *fHalService, struct ieee80211_node *node, apple80211_scan_result *result)
{
    if (!result) return kIOReturnBadArgument;
    if (!fHalService || !node || !node->ni_chan ||
        node->ni_chan == IEEE80211_CHAN_ANYC) return kIOReturnNotReady;
    bzero(result, sizeof(*result));
    result->version = APPLE80211_VERSION;
    if (node->ni_rsnie_tlv && node->ni_rsnie_tlv_len > 0) {
        result->asr_ie_len = MIN(node->ni_rsnie_tlv_len, sizeof(result->asr_ie_data));
        memcpy(result->asr_ie_data, node->ni_rsnie_tlv, result->asr_ie_len);
    }
    result->asr_beacon_int = node->ni_intval;
    result->asr_nrates = MIN(node->ni_rates.rs_nrates,
        MIN(ARRAY_SIZE(result->asr_rates), ARRAY_SIZE(node->ni_rates.rs_rates)));
    for (unsigned i = 0; i < result->asr_nrates; i++)
        result->asr_rates[i] = node->ni_rates.rs_rates[i];
    result->asr_age = (uint32_t)(airport_up_time() - node->ni_age_ts);
    result->asr_cap = node->ni_capinfo;
    result->asr_channel.version = APPLE80211_VERSION;
    result->asr_channel.channel = ieee80211_chan2ieee(fHalService->get80211Controller(), node->ni_chan);
    result->asr_channel.flags = ieeeChanFlag2appleScanFlagVentura(node->ni_chan->ic_flags);
    result->asr_noise = -fHalService->getDriverInfo()->getBSSNoise();
    result->asr_rssi = -(0 - MTK_MIN_DBM - node->ni_rssi);
    memcpy(result->asr_bssid, node->ni_bssid, IEEE80211_ADDR_LEN);
    result->asr_ssid_len = MIN(node->ni_esslen,
        MIN(sizeof(result->asr_ssid), sizeof(node->ni_essid)));
    if (result->asr_ssid_len != 0)
        memcpy(&result->asr_ssid, node->ni_essid, result->asr_ssid_len);
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getCURRENT_NETWORK(apple80211_scan_result *sr)
{
    if (!sr) return kIOReturnBadArgument;
    auto *ic = current80211();
    if (!ic || ic->ic_state != IEEE80211_S_RUN || !ic->ic_bss)
        return kIOReturnNotReady;
    return convertNodeToScanResult(fHalService, ic->ic_bss, sr);
}

IOReturn AirportMtkSkywalkInterface::
getCOLOCATED_NETWORK_SCOPE_ID(apple80211_colocated_network_scope_id *as)
{
    if (!as)
        return kIOReturnBadArgument;
    as->version = APPLE80211_VERSION;
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::
getSCAN_RESULT(struct apple80211_scan_result *)
{
    // WCL owns scan results; never keep a borrowed node across legacy calls.
    return kIOReturnUnsupported;
}

IOReturn AirportMtkSkywalkInterface::getHW_ADDR(apple80211_hw_mac_address *data)
{
    if (!data) return kIOReturnBadArgument;
    AirportWCL::HardwareAddress value;
    if (!AirportWCL::copyHardwareAddress(wclPermanentMAC, value)) return kIOReturnNotReady;
    memcpy(data, &value, sizeof(value));
    return kIOReturnSuccess;
}

IOReturn AirportMtkSkywalkInterface::setMacAddress(mloAddrArray &addresses)
{
    if (wclStopping || !instance) return kIOReturnNotReady;
    return instance->setHardwareAddress(&addresses, IEEE80211_ADDR_LEN);
}

