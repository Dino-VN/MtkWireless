//
//  ieee80211_sae.c
//  MtkWireless
//
//  WPA3-Personal SAE authentication, station side (IEEE 802.11-2020
//  12.4): Commit and Confirm frames before association. The group 19
//  arithmetic, hunting-and-pecking PWE and key derivation run in the Rust
//  core (rust/mtk-wlan/src/sae, mtk_wlan_sae_* in kext/Wifi/mtk_wlan.h);
//  this file moves frames and feeds the PMK to the 4-way handshake through
//  the PMKSA cache (AKM 00-0F-AC:8).
//
//  Not supported: H2E (hash-to-element), SAE-PK, password identifiers,
//  groups other than 19, PMKSA caching across joins.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/endian.h>
#include <sys/errno.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_priv.h>

#include "mtk_wlan.h"

enum {
	IEEE80211_SAE_IDLE = 0,
	IEEE80211_SAE_COMMITTED,	/* our Commit sent */
	IEEE80211_SAE_CONFIRMED,	/* AP Commit taken, our Confirm sent */
};

/* Auth transaction sequence numbers of the two SAE messages. */
#define IEEE80211_SAE_SEQ_COMMIT	1
#define IEEE80211_SAE_SEQ_CONFIRM	2

/* Random draws before giving up on mtk_wlan_sae_commit's EAGAIN. */
#define IEEE80211_SAE_RANDOM_TRIES	8

int
ieee80211_sae_set_password(struct ieee80211com *ic, const u_int8_t *pw,
    size_t len)
{
	ieee80211_sae_forget(ic);
	if (pw == NULL || len == 0 || len > sizeof(ic->ic_sae_password))
		return EINVAL;
	memcpy(ic->ic_sae_password, pw, len);
	ic->ic_sae_password_len = (u_int)len;
	return 0;
}

void
ieee80211_sae_clear(struct ieee80211com *ic)
{
	if (ic->ic_sae != NULL) {
		mtk_wlan_sae_clear(ic->ic_sae);
		explicit_bzero(ic->ic_sae, sizeof(*ic->ic_sae));
		free(ic->ic_sae);
		ic->ic_sae = NULL;
	}
	ic->ic_sae_state = IEEE80211_SAE_IDLE;
	ic->ic_sae_send_confirm = 0;
}

void
ieee80211_sae_forget(struct ieee80211com *ic)
{
	ieee80211_sae_clear(ic);
	explicit_bzero(ic->ic_sae_password, sizeof(ic->ic_sae_password));
	ic->ic_sae_password_len = 0;
}

/*
 * SAE is used for this BSS when it offers AKM 8, we have a password and
 * both sides can do PMF (802.11-2020 12.4.1: SAE requires MFPC).
 */
int
ieee80211_sae_usable(struct ieee80211com *ic, const struct ieee80211_node *ni)
{
	return ic->ic_sae_password_len != 0 &&
	    (ic->ic_rsnakms & IEEE80211_AKM_SAE) &&
	    (ni->ni_rsnakms & IEEE80211_AKM_SAE) &&
	    (ni->ni_rsnprotos & IEEE80211_PROTO_RSN) &&
	    (ic->ic_caps & IEEE80211_C_MFP) &&
	    (ni->ni_rsncaps & IEEE80211_RSNCAP_MFPC);
}

static int
ieee80211_sae_send(struct ieee80211com *ic, struct ieee80211_node *ni,
    u_int16_t seq, const u_int8_t *body, size_t len)
{
	int error;

	/* ieee80211_get_auth() builds the frame from the staged body. */
	ic->ic_sae_txbuf = body;
	ic->ic_sae_txlen = len;
	error = IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_AUTH, seq);
	ic->ic_sae_txbuf = NULL;
	ic->ic_sae_txlen = 0;
	return error;
}

static int
ieee80211_sae_send_commit(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	u_int8_t body[MTK_WLAN_SAE_COMMIT_MAX];
	size_t len = 0;
	int error;

	error = mtk_wlan_sae_write_commit(ic->ic_sae, body, sizeof(body), &len);
	if (error == 0)
		error = ieee80211_sae_send(ic, ni, IEEE80211_SAE_SEQ_COMMIT,
		    body, len);
	if (error == 0)
		ic->ic_sae_state = IEEE80211_SAE_COMMITTED;
	return error;
}

static int
ieee80211_sae_send_confirm(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	u_int8_t body[MTK_WLAN_SAE_CONFIRM_LEN];
	size_t len = 0;
	int error;

	/* Send-Confirm counts up and stops at 0xffff (12.4.8.6.6). */
	if (ic->ic_sae_send_confirm < 0xffff)
		ic->ic_sae_send_confirm++;
	error = mtk_wlan_sae_write_confirm(ic->ic_sae, ic->ic_sae_send_confirm,
	    body, sizeof(body), &len);
	if (error == 0)
		error = ieee80211_sae_send(ic, ni, IEEE80211_SAE_SEQ_CONFIRM,
		    body, len);
	explicit_bzero(body, sizeof(body));
	return error;
}

/* Starts SAE with ic_bss: derive the PWE, pick random scalars, send Commit. */
static int
ieee80211_sae_start(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	u_int8_t rand[32], mask[32];
	int error, i;

	ieee80211_sae_clear(ic);
	ic->ic_sae = (struct mtk_wlan_sae *)malloc(sizeof(*ic->ic_sae), 0, 0);
	if (ic->ic_sae == NULL)
		return ENOMEM;
	memset(ic->ic_sae, 0, sizeof(*ic->ic_sae));

	error = mtk_wlan_sae_init(ic->ic_sae, ic->ic_myaddr, ni->ni_macaddr,
	    ic->ic_sae_password, ic->ic_sae_password_len);
	for (i = 0; error == 0 && i < IEEE80211_SAE_RANDOM_TRIES; i++) {
		arc4random_buf(rand, sizeof(rand));
		arc4random_buf(mask, sizeof(mask));
		error = mtk_wlan_sae_commit(ic->ic_sae, rand, mask);
		if (error != EAGAIN)
			break;
	}
	explicit_bzero(rand, sizeof(rand));
	explicit_bzero(mask, sizeof(mask));
	if (error == 0)
		error = ieee80211_sae_send_commit(ic, ni);
	if (error != 0) {
		XYLog("%s: SAE start for %s failed (%d)\n", __FUNCTION__,
		    ether_sprintf(ni->ni_macaddr), error);
		ieee80211_sae_clear(ic);
	}
	return error;
}

/*
 * First authentication frame to ic_bss: SAE Commit when the AKM is SAE,
 * else Open System. On an SAE error no frame goes out and the management
 * timer moves on to the next scan, as for a lost Open System response.
 */
void
ieee80211_send_auth_request(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	if (ic->ic_opmode == IEEE80211_M_STA &&
	    (ic->ic_flags & IEEE80211_F_RSNON) &&
	    ieee80211_is_sae_akm((enum ieee80211_akm)ni->ni_rsnakms)) {
		if (ieee80211_sae_start(ic, ni) != 0)
			ic->ic_mgt_timer = IEEE80211_TRANS_WAIT;
		return;
	}
	IEEE80211_SEND_MGMT(ic, ni, IEEE80211_FC0_SUBTYPE_AUTH,
	    IEEE80211_AUTH_OPEN_REQUEST);
}

/* The AP refused us (or SAE failed): end this attempt like an open
 * authentication failure, and tell WCL when the AP gave a status. */
static void
ieee80211_sae_fail(struct ieee80211com *ic, struct ieee80211_node *ni,
    u_int16_t status, const char *why)
{
	XYLog("%s: SAE with %s failed: %s (status %u)\n", __FUNCTION__,
	    ether_sprintf(ni->ni_macaddr), why, status);
	ieee80211_sae_clear(ic);
	ic->ic_sae_fail++;
	ic->ic_sae_last_status = status;
	ic->ic_stats.is_rx_auth_fail++;
	if (status != IEEE80211_STATUS_SUCCESS) {
		ic->ic_assoc_status = status;
		if (ic->ic_event_handler)
			(*ic->ic_event_handler)(ic,
			    IEEE80211_EVT_STA_ASSOC_DONE, NULL);
		ieee80211_try_another_bss(ic);
	}
}

/* AP confirmed: PMK and PMKID go to the PMKSA cache for message 1/4. */
static void
ieee80211_sae_accepted(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	struct ieee80211_pmk *pmk;
	u_int8_t key[IEEE80211_PMK_LEN], pmkid[IEEE80211_PMKID_LEN];

	if (mtk_wlan_sae_keys(ic->ic_sae, key, pmkid) != 0) {
		ieee80211_sae_fail(ic, ni, IEEE80211_STATUS_SUCCESS, "no keys");
		return;
	}
	ieee80211_sae_clear(ic);
	pmk = ieee80211_pmksa_add(ic, IEEE80211_AKM_SAE, ni->ni_macaddr, key, 0);
	if (pmk == NULL) {
		explicit_bzero(key, sizeof(key));
		ieee80211_sae_fail(ic, ni, IEEE80211_STATUS_SUCCESS, "no memory");
		return;
	}
	/* 12.4.5.4: PMKID = L((commit-scalar + peer-commit-scalar) mod r,
	 * 0, 128), not HMAC("PMK Name") as ieee80211_pmksa_add derives. */
	memcpy(pmk->pmk_pmkid, pmkid, IEEE80211_PMKID_LEN);
	memcpy(ni->ni_pmk, key, IEEE80211_PMK_LEN);
	memcpy(ni->ni_pmkid, pmkid, IEEE80211_PMKID_LEN);
	ni->ni_flags |= IEEE80211_NODE_PMK | IEEE80211_NODE_PMKID;
	explicit_bzero(key, sizeof(key));
	ic->ic_sae_ok++;
	ic->ic_sae_last_status = IEEE80211_STATUS_SUCCESS;
	if (ic->ic_if.if_flags & IFF_DEBUG)
		XYLog("%s: SAE with %s done\n", ic->ic_if.if_xname,
		    ether_sprintf(ni->ni_macaddr));
	ieee80211_new_state(ic, IEEE80211_S_ASSOC,
	    IEEE80211_FC0_SUBTYPE_AUTH);
}

/*
 * SAE Authentication frame from the AP. `body` and `len` cover what
 * follows algorithm, sequence and status.
 */
void
ieee80211_sae_recv_auth(struct ieee80211com *ic,
    const struct ieee80211_frame *wh, const u_int8_t *body, size_t len,
    u_int16_t seq, u_int16_t status)
{
	struct ieee80211_node *ni = ic->ic_bss;
	int error;

	if (ic->ic_state != IEEE80211_S_AUTH || ni == NULL ||
	    ic->ic_sae == NULL ||
	    !IEEE80211_ADDR_EQ(wh->i_addr2, ni->ni_bssid)) {
		ic->ic_stats.is_rx_bad_auth++;
		return;
	}

	switch (seq) {
	case IEEE80211_SAE_SEQ_COMMIT:
		if (status == MTK_WLAN_SAE_STATUS_ANTI_CLOGGING) {
			/* Resend the same Commit with the AP's token. */
			if (ic->ic_sae_state != IEEE80211_SAE_COMMITTED ||
			    mtk_wlan_sae_set_token(ic->ic_sae, body, len) != 0 ||
			    ieee80211_sae_send_commit(ic, ni) != 0)
				ieee80211_sae_fail(ic, ni, status,
				    "anti-clogging token");
			return;
		}
		if (status != IEEE80211_STATUS_SUCCESS) {
			ieee80211_sae_fail(ic, ni, status,
			    status == MTK_WLAN_SAE_STATUS_H2E ?
			    "AP requires hash-to-element" :
			    status == MTK_WLAN_SAE_STATUS_UNSUPPORTED_GROUP ?
			    "AP refused group 19" : "Commit refused");
			return;
		}
		if (ic->ic_sae_state == IEEE80211_SAE_CONFIRMED) {
			/* A retransmitted AP Commit: our Confirm was lost. */
			if (ieee80211_sae_send_confirm(ic, ni) != 0)
				ieee80211_sae_fail(ic, ni,
				    IEEE80211_STATUS_SUCCESS, "Confirm resend");
			return;
		}
		if (ic->ic_sae_state != IEEE80211_SAE_COMMITTED)
			return;
		error = mtk_wlan_sae_process_commit(ic->ic_sae, body, len);
		if (error == 0)
			error = ieee80211_sae_send_confirm(ic, ni);
		if (error != 0) {
			ieee80211_sae_fail(ic, ni, IEEE80211_STATUS_SUCCESS,
			    "bad AP Commit");
			return;
		}
		ic->ic_sae_state = IEEE80211_SAE_CONFIRMED;
		break;

	case IEEE80211_SAE_SEQ_CONFIRM:
		if (status != IEEE80211_STATUS_SUCCESS) {
			ieee80211_sae_fail(ic, ni, status, "Confirm refused");
			return;
		}
		if (ic->ic_sae_state != IEEE80211_SAE_CONFIRMED)
			return;
		error = mtk_wlan_sae_check_confirm(ic->ic_sae, body, len);
		if (error != 0) {
			/* Wrong password, or a forged frame: stay, the AP's
			 * real Confirm may still come before the timer. */
			XYLog("%s: SAE Confirm from %s rejected (%d)\n",
			    __FUNCTION__, ether_sprintf(ni->ni_macaddr), error);
			ic->ic_stats.is_rx_auth_fail++;
			return;
		}
		ieee80211_sae_accepted(ic, ni);
		break;

	default:
		ic->ic_stats.is_rx_bad_auth++;
		break;
	}
}
