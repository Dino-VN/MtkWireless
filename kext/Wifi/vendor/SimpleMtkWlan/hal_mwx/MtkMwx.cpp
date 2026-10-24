//
//  MtkMwx.cpp
//  SimpleMtkWlan
//
//  Created by laobamac (王孝慈) on 2026/5/5.
//  Copyright © 2026 laobamac (王孝慈). All rights reserved.
//

/*	$OpenBSD: if_mwx.c,v 1.9 2026/05/09 12:13:15 claudio Exp $ */
/*
 * Copyright (c) 2022 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2021 Lorenzo Bianconi <lorenzo.bianconi83@gmail.com>
 * Copyright (c) 2017 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2016 Felix Fietkau <nbd@nbd.name>
 * Copyright (c) 2007-2010 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "MtkMwx.hpp"
#include "mtk_wlan.h"
#include <AWDL/AwdlProtocol.hpp>

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/bitfield.h>
#include <sys/kpi_mbuf.h>
#include <sys/_if_ether.h>
#include <sys/_if_media.h>
#include <sys/_malloc.h>
#include <sys/_mbuf.h>
#include <sys/_task.h>
#include <net/ethernet.h>
#include <net80211/ieee80211_proto.h>
#include <net80211/ieee80211_radiotap.h>
#include <FwData.h>

#include "if_mwxreg.h"
#include <ClientKit/Common.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>

/*
 * TX rings are touched by the interrupt (workloop thread), the firmware
 * loader (IOConfigThread), MCU senders and if_start without any real
 * spl exclusion; serialise descriptor and mbuf ownership changes.
 * Never call wakeup() or sleep while holding it: wakeup enters the
 * workloop gate, which the interrupt thread holds while taking this lock.
 */
static IOLock *mwx_txq_lock;

OSDefineMetaClassAndStructors(MtkMwx, MtkHalService)

#ifndef NBPFILTER
#define NBPFILTER 0
#endif

#define PCI_VENDOR_MEDIATEK            0x14c3
#define PCI_PRODUCT_MEDIATEK_MT7921    0x7961
#define PCI_PRODUCT_MEDIATEK_MT7921K   0x0608
#define PCI_PRODUCT_MEDIATEK_MT7922    0x7922
#define PCI_PRODUCT_MEDIATEK_RZ616     0x0616	/* MT7922 variant; Linux mt7921e loads MT7922 firmware */

#define MWX_LOG(x...) printf("SimpleMtkWlan-mwx: " x)
#define MWX_DEV_LOG(sc, fmt, args...) \
	printf("SimpleMtkWlan-mwx: %s: " fmt, DEVNAME(sc), ##args)

#define ifnet _ifnet
#define mbuf __mbuf

#undef KASSERT
#define KASSERT(exp, ...) do { if (!(exp)) panic("mwx assertion failed: %s", #exp); } while (0)
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef ISSET
#define ISSET(t, f) (((t) & (f)) != 0)
#endif
#ifndef IEEE80211_CHAN_40MHZ
#define IEEE80211_CHAN_40MHZ IEEE80211_CHAN_HT40
#endif
#ifndef IEEE80211_CHAN_6GHZ
#define IEEE80211_CHAN_6GHZ 0
#endif
#ifndef IEEE80211_IS_CHAN_6GHZ
#define IEEE80211_IS_CHAN_6GHZ(_c) false
#endif

#ifndef M_DEVBUF
#define M_DEVBUF 0
#endif
#ifndef M_NOWAIT
#define M_NOWAIT 0
#endif
#ifndef M_WAITOK
#define M_WAITOK 0
#endif
#ifndef M_ZERO
#define M_ZERO 0
#endif

static MtkMwx *gMwxHal;

static inline void *
mallocarray(size_t n, size_t size, int type, int flags)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return malloc(n * size, type, flags);
}

static inline void
mwx_free_mem(void *addr)
{
	free(addr);
}

#define free(addr, type, size) mwx_free_mem(addr)

static inline int
loadfirmware(const char *name, u_char **buf, size_t *len)
{
	MWX_LOG("firmware lookup start name=%s\n", name);
	OSData *data = getFWDescByName(name);
	if (data == NULL) {
		MWX_LOG("firmware lookup failed name=%s error=%d\n", name, ENOENT);
		return ENOENT;
	}
	*len = data->getLength();
	*buf = (u_char *)malloc(*len, M_DEVBUF, M_WAITOK);
	if (*buf == NULL) {
		data->release();
		MWX_LOG("firmware copy allocation failed name=%s size=%zu error=%d\n",
		    name, *len, ENOMEM);
		return ENOMEM;
	}
	memcpy(*buf, data->getBytesNoCopy(), *len);
	data->release();
	MWX_LOG("firmware lookup ok name=%s size=%zu\n", name, *len);
	return 0;
}

static inline int
tsleep_nsec(void *ident, int priority, const char *wmesg, uint64_t timo)
{
	if (gMwxHal == NULL)
		return EWOULDBLOCK;
	return gMwxHal->mwxSleepNsec(ident, priority, wmesg, timo);
}

static inline void
config_mountroot(struct device *self, void (*fn)(struct device *))
{
	fn(self);
}

static inline void delay(int usec) {
    if (usec <= 0) return;
    if (usec < 1000) IODelay(usec);
    else IOSleep((usec + 999) / 1000);
}

static inline struct mbuf *
m_pullup(struct mbuf *m, size_t len)
{
	struct mbuf *n = NULL;
	size_t pktlen;
	u_int32_t chunks = 1;
	static uint32_t pullup_rebuild_log_count;

	if (m == NULL)
		return NULL;
	if (mbuf_len(m) >= len)
		return m;

	pktlen = mbuf_pkthdr_len(m);
	if (len > pktlen) {
		m_freem(m);
		return NULL;
	}

	if (pullup_rebuild_log_count < 32) {
		MWX_LOG("m_pullup rebuild len=%zu first_len=%zu pktlen=%zu\n",
		    len, mbuf_len(m), pktlen);
		pullup_rebuild_log_count++;
	}
	if (mbuf_allocpacket(M_DONTWAIT, pktlen, &chunks, &n) != 0) {
		m_freem(m);
		return NULL;
	}
	mbuf_setlen(n, pktlen);
	mbuf_pkthdr_setlen(n, pktlen);
	if (mbuf_copydata(m, 0, pktlen, mtod(n, void *)) != 0) {
		m_freem(n);
		m_freem(m);
		return NULL;
	}
	m_freem(m);
	return n;
}

static inline void
m_adj(struct mbuf *m, int len)
{
	mbuf_adj(m, len);
}

static inline struct mbuf *
m_prepend(struct mbuf *m, size_t len, int how)
{
	return mbuf_prepend(&m, len, how) == 0 ? m : NULL;
}

static inline size_t
m_leadingspace(struct mbuf *m)
{
	return mbuf_leadingspace(m);
}

static inline size_t
m_trailingspace(struct mbuf *m)
{
	return mbuf_trailingspace(m);
}

static inline struct mbuf *
m_gethdr(int how, int type)
{
	struct mbuf *m = NULL;
	mbuf_gethdr(how, type, &m);
	return m;
}

static inline struct mbuf *
MCLGETL(struct mbuf *m, int how, size_t len)
{
	u_int32_t chunks = 1;
	if (m == NULL) {
		if (mbuf_allocpacket(how, len, &chunks, &m) != 0)
			return NULL;
	} else if (mbuf_getcluster(how, mbuf_type(m), len, &m) != 0) {
		return NULL;
	}
	return m;
}

/* Firmware chunk plus MCU header headspace must fit a 4 KB cluster;
 * larger requests need 16 KB jumbo clusters, which run out quickly. */
#define MWX_FW_CHUNK	(4096 - sizeof(struct mwx_mcu_txd))

extern int mtk_compat_intr_index;
extern uint32_t mtk_aspm_lnkctl_before, mtk_aspm_lnkctl_after;
extern uint32_t mtk_aspm_l1ss_before, mtk_aspm_l1ss_after;
extern uint32_t mtk_aspm_parent_l1ss_before;
extern int mtk_compat_intr_is_msi;

/* Times an MCU send slept on a full TX ring, or retried a failed
 * firmware chunk allocation (bring-up diagnostics). */
static uint32_t mwx_ebusy_sleeps;
static uint32_t mwx_enomem_retries;

static const struct pci_matchid mwx_devices[] = {
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7921 },
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7921K },
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7922 },
	{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_RZ616 },
};

/* Keep the OpenBSD DPRINTF path available, but do not enable it by default.
 * The PCIe interrupt and MCU paths can otherwise flood the kernel log. */
/* #define MWX_DEBUG	1 */

#define	MT7921_ROM_PATCH	"mwx-mt7961_patch_mcu_1_2_hdr"
#define	MT7921_FIRMWARE_WM	"mwx-mt7961_ram_code_1"
#define	MT7922_ROM_PATCH	"mwx-mt7922_patch_mcu_1_1_hdr"
#define	MT7922_FIRMWARE_WM	"mwx-mt7922_ram_code_1"

#if NBPFILTER > 0
struct mwx_rx_radiotap_header {
	struct ieee80211_radiotap_header	wr_ihdr;
	uint64_t				wr_tsft;
	uint8_t					wr_flags;
	uint8_t					wr_rate;
	uint16_t				wr_chan_freq;
	uint16_t				wr_chan_flags;
	int8_t					wr_dbm_antsignal;
	int8_t					wr_dbm_antnoise;
} __packed;

#define	MWX_RX_RADIOTAP_PRESENT				\
	((1 << IEEE80211_RADIOTAP_TSFT) |		\
	(1 << IEEE80211_RADIOTAP_FLAGS) |		\
	(1 << IEEE80211_RADIOTAP_RATE) |		\
	(1 << IEEE80211_RADIOTAP_CHANNEL) |		\
	(1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL) |	\
	(1 << IEEE80211_RADIOTAP_DBM_ANTNOISE))

struct mwx_tx_radiotap_header {
	struct ieee80211_radiotap_header	wt_ihdr;
	uint8_t					wt_flags;
	uint8_t					wt_rate;
	uint16_t				wt_chan_freq;
	uint16_t				wt_chan_flags;
} __packed;

#define	MWX_TX_RADIOTAP_PRESENT			\
	((1 << IEEE80211_RADIOTAP_FLAGS) |	\
	(1 << IEEE80211_RADIOTAP_RATE) |	\
	(1 << IEEE80211_RADIOTAP_CHANNEL))

#endif

struct mwx_txwi {
	struct mt76_txwi		*mt_desc;
	struct mbuf			*mt_mbuf;
	bus_dmamap_t			mt_map;
	LIST_ENTRY(mwx_txwi)		mt_entry;
	u_int32_t			mt_addr;
	u_int				mt_idx;
	int				mt_busy;
	int				mt_eapol;	/* log TX status */
};

struct mwx_txwi_desc {
	struct mt76_txwi		*mt_desc;
	struct mwx_txwi			*mt_data;

	u_int				mt_count;
	u_int				mt_inuse;
	u_int				mt_peak;
	bus_dmamap_t			mt_map;
	bus_dma_segment_t		mt_seg;
	LIST_HEAD(, mwx_txwi)		mt_freelist;
};

struct mwx_queue_data {
	struct mbuf			*md_mbuf;
	struct mwx_txwi			*md_txwi;
	bus_dmamap_t			md_map;
};

struct mwx_queue {
	uint32_t			mq_regbase;
	u_int				mq_count;
	u_int				mq_prod;
	u_int				mq_cons;

	struct mwx_desc		*mq_desc;
	struct mwx_queue_data		*mq_data;

	bus_dmamap_t			mq_map;
	bus_dma_segment_t		mq_seg;
	int				mq_wakeme;
};

struct mwx_hw_capa {
	int8_t		has_2ghz;
	int8_t		has_5ghz;
	int8_t		has_6ghz;
	uint8_t		antenna_mask;
	uint8_t		num_streams;
};

struct mwx_node {
	struct ieee80211_node	ni;
	uint16_t		wcid;
	uint16_t		flags;
};
#define	MWX_NODE_FLAG_HAVE_PAIRWISE_KEY		0x1
#define	MWX_NODE_FLAG_HAVE_GROUP_KEY		0x2

struct mwx_vif {
	struct mwx_node		vif_mn;
	uint8_t			idx;
	uint8_t			omac_idx;
	uint8_t			band_idx;
	uint8_t			wmm_idx;
	uint8_t			scan_seq_num;
};

enum mwx_hw_type {
	MWX_HW_MT7921,
	MWX_HW_MT7922,
};

struct mwx_setkey_task_arg {
	struct ieee80211_node	*ni;
	struct ieee80211_key	*k;
};

struct mwx_ba_task_arg {
	struct ieee80211_node	*ni;
	uint8_t			tid;
	uint8_t			start;
	uint8_t			tx;	/* originator (TX) agreement */
};

struct mwx_softc {
    struct device sc_dev;
    struct ieee80211com sc_ic;
    struct mwx_vif sc_awdl_vif;
    struct mwx_node sc_awdl_peers[3];
    bool sc_awdl_peer_used[3];
    // A station can exist in firmware after its BSS/device is removed, or
    // after a failed reply. Keep ownership until a disconnect is acknowledged.
    bool sc_awdl_station_pending[4];
    mtkawdl::Reservation sc_awdl_roc;
    uint64_t sc_awdl_roc_events, sc_awdl_roc_grants, sc_awdl_roc_rejected;
    uint64_t sc_awdl_roc_command_results, sc_awdl_roc_command_failed, sc_awdl_roc_command_malformed;
    uint64_t sc_awdl_roc_command_result_us;
    uint32_t sc_awdl_roc_command_status;
    uint64_t sc_awdl_roc_grant_delay_us, sc_awdl_roc_max_grant_delay_us;
    uint64_t sc_awdl_roc_submitted, sc_awdl_roc_send_failed;
    uint64_t sc_awdl_roc_scan_blocked, sc_awdl_roc_busy_blocked;
    uint64_t sc_awdl_roc_timeouts;
    bool sc_awdl_roc_abort_pending;
    uint8_t sc_awdl_roc_abort_token;
    uint64_t sc_awdl_roc_abort_submitted, sc_awdl_roc_abort_failed, sc_awdl_roc_abort_retry_at;
    int sc_awdl_roc_abort_status;
    uint64_t sc_awdl_roc_queue_delay_us, sc_awdl_roc_max_queue_delay_us;
    uint64_t sc_awdl_roc_reasons[static_cast<unsigned>(mtkawdl::RocResult::Count)];
    mtkawdl::RocGrant sc_awdl_roc_last_event;
    mtkawdl::RocResult sc_awdl_roc_last_result;
    uint64_t sc_awdl_roc_last_event_us, sc_awdl_roc_last_response_us;
    uint64_t sc_awdl_tx_submitted, sc_awdl_tx_bad_frame, sc_awdl_tx_no_grant;
    uint64_t sc_awdl_tx_busy, sc_awdl_tx_no_peer, sc_awdl_tx_backpressure;
    uint64_t sc_awdl_tx_copy_failed, sc_awdl_tx_dma_failed;
    mtkawdl::TxStatusCounters sc_awdl_tx_status;
    int sc_awdl_tx_errno;
    void *sc_awdl_context;
    MtkMwx::AwdlReceive sc_awdl_receive;
    uint8_t sc_awdl_address[6];
    bool sc_awdl_enabled, sc_awdl_configured, sc_awdl_filter_active;
    bool sc_awdl_scan_suspended;
    // MtkWireless: scans that took the radio from AWDL while unassociated,
    // and ROCs refused while the STA authenticates/associates/does 4-way.
    uint64_t sc_awdl_scan_preempted, sc_awdl_roc_join_blocked;
    // ROC grant latency, <5/10/20/40/80/160/>=160 ms; [0] unassociated,
    // [1] associated with the ROC on the AP channel, [2] off it.
    uint32_t sc_awdl_roc_latency[3][7];
    // MtkWireless: AP told we sleep (PM=1) for an off-channel AWDL ROC.
    bool sc_awdl_infra_ps;
    uint64_t sc_awdl_infra_ps_at, sc_awdl_infra_ps_sleeps, sc_awdl_infra_ps_wakes;
    uint64_t sc_awdl_infra_ps_failed;
    uint32_t sc_awdl_filter;
    IOLock *sc_awdl_lock;
    uint32_t sc_awdl_epoch;
    bool sc_awdl_busy, sc_awdl_stopping;
    unsigned sc_awdl_calls;
    uint8_t sc_awdl_channel;
    uint64_t sc_awdl_token_used[256];

	enum mwx_hw_type	sc_hwtype;

	struct mwx_queue	sc_txq;
	struct mwx_queue	sc_txmcuq;
	struct mwx_queue	sc_txfwdlq;

	struct mwx_queue	sc_rxq;
	struct mwx_queue	sc_rxmcuq;
	struct mwx_queue	sc_rxfwdlq;

	struct mwx_txwi_desc	sc_txwi;

	bus_space_tag_t		sc_st;
	bus_space_handle_t	sc_memh;
	bus_size_t		sc_mems;
	bus_dma_tag_t		sc_dmat;
	pcitag_t		sc_tag;
	pci_chipset_tag_t	sc_pc;
	void			*sc_ih;

	int			(*sc_newstate)(struct ieee80211com *,
				    enum ieee80211_state, int);

	struct taskq		*sc_nswq;
	struct task		sc_newstate_task;
	struct task		sc_bgscan_done_task;
	struct task		sc_setkey_task;
	struct task		sc_scan_task;
	struct task		sc_reset_task;
	struct task		sc_power_task;
	volatile SInt32		sc_power_req;	/* bumped by every enable/disable */
	volatile SInt32		sc_power_done;	/* last request the task served */
	int			sc_power_result;
	CTimeout		*sc_reset_to;
	CTimeout		*sc_scan_to;
	CTimeout		*sc_dma_to;
	struct task		sc_fullreset_task;
	uint32_t		sc_bgscan_dwell;	/* ms per channel while associated */
#define MWX_BGSCAN_DWELL_AUTO	0xffffffffU	/* from WCL away time, else firmware */
	uint32_t		sc_knobs;
#define MWX_KNOB_LINEARIZE	0x01	/* copy TX frames into one segment */
#define MWX_KNOB_DATA_RA	0x02	/* unicast data uses firmware rate control */
#define MWX_KNOB_AUTORESET	0x04	/* full reset when WFDMA gets disabled */
#define MWX_KNOB_NO_HT		0x08	/* associate as legacy 11a/g (next join) */
#define MWX_KNOB_NO_VHT		0x10	/* do not offer 11ac (next join) */
#define MWX_KNOB_NO_40		0x20	/* 20 MHz only (next join) */
#define MWX_KNOB_NO_RXBA	0x40	/* refuse RX Block Ack agreements */
#define MWX_KNOB_TX_VO		0x80	/* old behaviour: all frames on VO */
#define MWX_KNOB_OLD_PEER	0x100	/* assoc peer: BASIC + STATE only */
#define MWX_KNOB_OLD_STA	0x200	/* legacy-only STA_REC PHY/RA/STATE */
#define MWX_KNOB_NO_QBSS	0x400	/* no QBSS TLV in BSS_INFO */
#define MWX_KNOB_NO_TXBA	0x800	/* do not set up TX Block Ack (A-MPDU) */
#define MWX_KNOB_NO_HE		0x1000	/* do not offer 11ax (next join) */
	uint8_t			sc_bss_bw;	/* CMD_CBW_* used by the BSS */
	uint8_t			sc_bss_he;	/* HE BSS info sent to firmware */
	uint8_t			sc_bss_center;	/* center channel for sc_bss_bw */
	uint16_t		sc_sta_wcid;	/* AP STA_REC in firmware, 0 = none */
	uint8_t			sc_sta_addr[IEEE80211_ADDR_LEN];
	struct task		sc_ba_task;
	struct mwx_ba_task_arg	sc_ba_arg[16];
	u_int			sc_ba_cur, sc_ba_tail;
	uint32_t		sc_rx_mode_cnt[16];	/* data frames per PHY mode */
	uint32_t		sc_rx_cur_rate;		/* RXV word 0, current frame */
	uint32_t		sc_rx_data_rate;	/* RXV word 0, last data frame */
	struct timeval		sc_txs_last;	/* last data frame sent with TX status */
	uint32_t		sc_txs_count;
	uint32_t		sc_rx_data_cnt;
	uint32_t		sc_dma_stalls;
	uint32_t		sc_full_resets;
	uint32_t		sc_mcu_cmd_irqs;
	uint32_t		sc_last_mcu_cmd;
	int			sc_dma_stall_logged;
	u_int			sc_flags;
#define MWX_FLAG_SCANNING		0x01
#define MWX_FLAG_BGSCAN			0x02
	int			sc_coredump_cnt;
	int8_t			sc_resetting;
	int8_t			sc_fw_restart;
	bool sc_detaching, sc_net_attached;
	int8_t			sc_fw_loaded;

	enum ieee80211_state	sc_ns_state;
	int			sc_ns_arg;

	struct ieee80211_node_switch_bss_arg	*sc_bgscan_arg;
	size_t			sc_bgscan_arg_size;

	struct mwx_setkey_task_arg	sc_setkey_arg[8];
	int			sc_setkey_cur;
	int			sc_setkey_tail;
	/* MtkWireless: last group CCMP key sent, for the BIP key pair. */
	int			sc_gtk_kid;

#if NBPFILTER > 0
	caddr_t			sc_drvbpf;
	union {
		struct mwx_rx_radiotap_header th;
		uint8_t pad[IEEE80211_RADIOTAP_HDRLEN];
	}			sc_rxtapu;
	int			sc_rxtap_len;
	union {
		struct mwx_tx_radiotap_header th;
		uint8_t pad[IEEE80211_RADIOTAP_HDRLEN];
	}			sc_txtapu;
	int			sc_txtap_len;
#define	sc_rxtap	sc_rxtapu.th
#define	sc_txtap	sc_txtapu.th
#endif

	struct mwx_vif		sc_vif;
	uint32_t		sc_wcid_mask;
	uint32_t		sc_intr_mask;

	/* mcu */
	uint32_t		sc_mcu_seq;
	struct {
		struct mbuf	*mcu_m;
		uint32_t	 mcu_cmd;
		uint32_t	 mcu_int;
		volatile int	 mcu_done;
	}			sc_mcu_wait[16];
	uint8_t			sc_scan_seq_num;

	/* phy / hw */
	struct mwx_hw_capa	sc_capa;
	uint8_t			sc_lladdr[ETHER_ADDR_LEN];
	char			sc_alpha2[4]; /* regulatory-domain */

	int16_t			sc_coverage_class;
	uint8_t			sc_slottime;

	/* mac specific */
	uint32_t		sc_rxfilter;

	/* diagnostic counters, all rate-limited at print sites */
	uint32_t		sc_rx_process_events;
	uint32_t		sc_rx_pkt_event;
	uint32_t		sc_rx_pkt_txrx_notify;
	uint32_t		sc_rx_pkt_normal_mcu;
	uint32_t		sc_rx_pkt_normal;
	uint32_t		sc_rx_pkt_unknown;
	uint32_t		sc_rx_fill_fail;
	uint32_t		sc_rx_delivered;
	uint32_t		sc_rx_ctl;
	uint32_t		sc_rx_bar;
	uint32_t		sc_rx_batches;
	uint32_t		sc_tx_stalls;
	uint32_t		sc_tx_nobufs;
	uint32_t		sc_tx_ring_full;
	uint32_t		sc_scan_done_count;
	uint32_t		sc_scan_last_ieee80211_nodes;
	/* Bring-up counters, read through IOCTL_80211_DEBUG. */
	uint32_t		sc_intr_count;
	uint32_t		sc_mcu_events;
	uint32_t		sc_last_mcu_eid;
	uint32_t		sc_mcu_bad_seq;
	uint32_t		sc_mcu_timeouts;
	uint32_t		sc_scan_requests;
	uint32_t		sc_scan_watchdog_fired;

};

const uint8_t mwx_channels_2ghz[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
};

#define MWX_NUM_2GHZ_CHANNELS   nitems(mwx_channels_2ghz)

const uint8_t mwx_channels_5ghz[] = {
	36, 40, 44, 48, 52, 56, 60, 64,
	100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
	149, 153, 157, 161, 165, 169, 173
};

#define MWX_NUM_5GHZ_CHANNELS   nitems(mwx_channels_5ghz)

/* MtkWireless: 5 GHz channels that need DFS (52-64, 100-144) everywhere. */
static inline int
mwx_chan_is_dfs(uint8_t chan)
{
	return (chan >= 52 && chan <= 64) || (chan >= 100 && chan <= 144);
}

/* Linux enum ieee80211_channel_flags, as MCU_CE_CMD_SET_CHAN_DOMAIN takes. */
#define MWX_LINUX_CHAN_RADAR	(1U << 3)

const uint8_t mwx_channels_6ghz[] = {
	/* UNII-5 */
	1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57,
	61, 65, 69, 73, 77, 81, 85, 89, 93,
	/* UNII-6 */
	97, 101, 105, 109, 113, 117,
	/* UNII-7 */
	121, 125, 129, 133, 137, 141, 145, 149, 153, 157, 161, 165, 169,
	173, 177, 181, 185,
	/* UNII-8 */
	189, 193, 197, 201, 205, 209, 213, 217, 221, 225, 229, 233
};

const struct mwx_rate {
	uint16_t	rate;
	uint16_t	hw_value;
} mt76_rates[] = {
	{	2,	(MT_PHY_TYPE_CCK << 8) | 0 },
	{	4,	(MT_PHY_TYPE_CCK << 8) | 1 },
	{	11,	(MT_PHY_TYPE_CCK << 8) | 2 },
	{	22,	(MT_PHY_TYPE_CCK << 8) | 3 },
	{	12,	(MT_PHY_TYPE_OFDM << 8) | 11 },
	{	18,	(MT_PHY_TYPE_OFDM << 8) | 15 },
	{	24,	(MT_PHY_TYPE_OFDM << 8) | 10 },
	{	36,	(MT_PHY_TYPE_OFDM << 8) | 14 },
	{	48,	(MT_PHY_TYPE_OFDM << 8) | 9 },
	{	72,	(MT_PHY_TYPE_OFDM << 8) | 13 },
	{	96,	(MT_PHY_TYPE_OFDM << 8) | 8 },
	{	108,	(MT_PHY_TYPE_OFDM << 8) | 12 },
};


#define MWX_NUM_6GHZ_CHANNELS   nitems(mwx_channels_6ghz)

#define MWX_RATE_HT_SISO_MCS_INV_PLCP 0x20

#define	DEVNAME(s)	((s)->sc_dev.dv_xname)
#define	DEVDEBUG(x)	((x)->sc_ic.ic_if.if_flags & IFF_DEBUG)

#define MWX_RX_DROP_LOG(sc, reason, rv, rxd0, rxd1, rxd2, rxd3, rxd4) do { \
	if ((sc)->sc_rx_fill_fail < 64) { \
		MWX_DEV_LOG((sc), \
		    "rx drop reason=%s rv=%d rxd=%08x/%08x/%08x/%08x/%08x len=%zu\n", \
		    (reason), (rv), (rxd0), (rxd1), (rxd2), (rxd3), \
		    (rxd4), mbuf_len(m)); \
	} \
} while (0)

#ifdef MWX_DEBUG
#define	DPRINTF(x...)		MWX_LOG(x)
#else
#define	DPRINTF(x...)
#endif

#ifdef MWX_PACKET_HEXDUMP
static void
pkt_hex_dump(struct mbuf *m)
{
	int len, rowsize = 16;
	int i, l, linelen;
	uint8_t *data;

	printf("Packet hex dump:\n");
	data = mtod(m, uint8_t *);
	len = mbuf_len(m);

	for (i = 0; i < len; i += linelen) {
		printf("%04x\t", i);
		linelen = len - i;
		if (len - i > rowsize)
			linelen = rowsize;
		for (l = 0; l < linelen; l++)
			printf("%02X ", (uint32_t)data[l]);
		data += linelen;
		printf("\n");
	}
}
#endif

int	mwx_init(struct ifnet *);
void	mwx_stop(struct ifnet *);
static void mwx_awdl_quiesce(struct mwx_softc *, bool drainCalls=true);
static int mwx_awdl_abort(struct mwx_softc *);
static bool mwx_awdl_current(struct mwx_softc *, uint32_t);
static int mwx_awdl_remove_context(struct mwx_softc *, uint32_t);
void	mwx_watchdog(struct ifnet *);
void	mwx_start(struct ifnet *);
int	mwx_ioctl(struct ifnet *, u_long, caddr_t);

struct ieee80211_node *mwx_node_alloc(struct ieee80211com *);
int	mwx_media_change(struct ifnet *);
#if NBPFILTER > 0
void	mwx_radiotap_attach(struct mwx_softc *);
#endif

int	mwx_newstate(struct ieee80211com *, enum ieee80211_state, int);
void	mwx_newstate_task(void *);
int	mwx_scan(struct mwx_softc *);
int	mwx_bgscan(struct ieee80211com *);
void	mwx_bgscan_done(struct ieee80211com *,
	    struct ieee80211_node_switch_bss_arg *, size_t);
void	mwx_bgscan_done_task(void *);
int	mwx_set_key(struct ieee80211com *, struct ieee80211_node *,
	    struct ieee80211_key *);
void	mwx_delete_key(struct ieee80211com *, struct ieee80211_node *,
	    struct ieee80211_key *);
void	mwx_setkey_task(void *);

int	mwx_tx(struct mwx_softc *, struct mbuf *, struct ieee80211_node *);
int	mwx_tx_full(struct mwx_softc *);
void	mwx_tx_resume(struct mwx_softc *);
void	mwx_rx(struct mwx_softc *, struct mbuf *, struct mbuf_list *);
int	mwx_count_nodes(struct ieee80211com *);
int	mwx_intr(void *);
int	mwx_preinit(struct mwx_softc *);
void	mwx_attach_hook(struct device *);
bool	mwx_attach(struct device *, struct device *, void *);
int	mwx_activate(struct device *, int);

void	mwx_reset(struct mwx_softc *);
void	mwx_reset_timeo(void *);
void	mwx_reset_task(void *);
void	mwx_power_task(void *);
int	mwx_txwi_alloc(struct mwx_softc *, int);
void	mwx_txwi_free(struct mwx_softc *);
struct mwx_txwi	*mwx_txwi_get(struct mwx_softc *);
void	mwx_txwi_put(struct mwx_softc *, struct mwx_txwi *);
int	mwx_txwi_enqueue(struct mwx_softc *, struct mwx_txwi *, struct mbuf *);
int	mwx_queue_alloc(struct mwx_softc *, struct mwx_queue *, int, uint32_t);
void	mwx_queue_free(struct mwx_softc *, struct mwx_queue *);
void	mwx_queue_reset(struct mwx_softc *, struct mwx_queue *);
int	mwx_buf_fill(struct mwx_softc *, struct mwx_queue_data *,
	    struct mwx_desc *);
int	mwx_queue_fill(struct mwx_softc *, struct mwx_queue *);
int	mwx_dma_alloc(struct mwx_softc *);
int	mwx_dma_reset(struct mwx_softc *, int);
static int mwx_dma_reinit_cond(struct mwx_softc *);
void	mwx_dma_free(struct mwx_softc *);
int	mwx_dma_tx_enqueue(struct mwx_softc *, struct mwx_queue *,
	    struct mbuf *);
int	mwx_dma_txwi_enqueue(struct mwx_softc *, struct mwx_queue *,
	    struct mwx_txwi *);
void	mwx_dma_tx_cleanup(struct mwx_softc *, struct mwx_queue *);
void	mwx_dma_tx_done(struct mwx_softc *);
void	mwx_dma_rx_process(struct mwx_softc *, struct mbuf_list *);
void	mwx_dma_rx_dequeue(struct mwx_softc *, struct mwx_queue *,
	    struct mbuf_list *);
void	mwx_dma_rx_done(struct mwx_softc *, struct mwx_queue *);

struct mbuf	*mwx_mcu_alloc_msg(size_t);
void	mwx_mcu_set_len(struct mbuf *, void *);
int	mwx_mcu_send_mbuf(struct mwx_softc *, uint32_t, struct mbuf *, int *);
int	mwx_mcu_send_msg(struct mwx_softc *, uint32_t, void *, size_t, int *);
int	mwx_mcu_send_wait(struct mwx_softc *, uint32_t, void *, size_t);
static int mwx_send_assoc_status(struct mwx_softc *, uint32_t, void *, size_t);
int	mwx_mcu_send_mbuf_wait(struct mwx_softc *, uint32_t, struct mbuf *);
void	mwx_mcu_rx_event(struct mwx_softc *, struct mbuf *);
int	mwx_mcu_wait_resp_int(struct mwx_softc *, uint32_t, int, uint32_t *);
int	mwx_mcu_wait_resp_msg(struct mwx_softc *, uint32_t, int,
	    struct mbuf **);

int		mwx_dma_disable(struct mwx_softc *sc, int force);
void		mwx_dma_enable(struct mwx_softc *sc);
int		mwx_mcu_fw_pmctrl(struct mwx_softc *);
int		mwx_mcu_drv_pmctrl(struct mwx_softc *);
int		mwx_wfsys_reset(struct mwx_softc *sc);
uint32_t	mwx_reg_addr(struct mwx_softc *, uint32_t);
int		mwx_init_hardware(struct mwx_softc *);
int		mt7921_mcu_init(struct mwx_softc *);
int		mwx_load_firmware(struct mwx_softc *);
int		mwx_mac_wtbl_update(struct mwx_softc *, int);
void		mwx_mac_init_band(struct mwx_softc *sc, uint32_t);
void		mwx_mac_init(struct mwx_softc *);
int		mwx_mcu_patch_sem_ctrl(struct mwx_softc *, int);
int		mwx_mcu_init_download(struct mwx_softc *, uint32_t,
		    uint32_t, uint32_t);
int		mwx_mcu_send_firmware(struct mwx_softc *, int,
		    u_char *, size_t, size_t);
int		mwx_mcu_start_patch(struct mwx_softc *);
int		mwx_mcu_start_firmware(struct mwx_softc *, uint32_t,
		    uint32_t);
int		mt7921_mcu_get_nic_capability(struct mwx_softc *);
int		mt7921_mcu_fw_log_2_host(struct mwx_softc *, uint8_t);
int		mt7921_mcu_set_eeprom(struct mwx_softc *);
int		mt7921_mcu_set_rts_thresh(struct mwx_softc *, uint32_t,
		    uint8_t);
int		mwx_mcu_set_deep_sleep(struct mwx_softc *, int);
void		mt7921_mcu_low_power_event(struct mwx_softc *, struct mbuf *);
void		mt7921_mcu_beacon_loss_event(struct mwx_softc *, struct mbuf *);
void		mt7921_mcu_tx_done_event(struct mwx_softc *, struct mbuf *);
void		mwx_end_scan_task(void *);
void		mwx_scan_timeo(void *);
void		mwx_dma_timeo(void *);
void		mwx_fullreset_task(void *);
void		mwx_dump_dma_state(struct mwx_softc *, const char *);
void		mt7921_mcu_scan_event(struct mwx_softc *, struct mbuf *);
int		mt7921_mcu_hw_scan(struct mwx_softc *, int);
int		mt7921_mcu_hw_scan_cancel(struct mwx_softc *);
int		mt7921_mcu_set_mac_enable(struct mwx_softc *, int, int);
int		mt7921_mcu_set_channel_domain(struct mwx_softc *);
uint8_t		mt7921_mcu_chan_bw(struct ieee80211_node *);
int		mt7921_mcu_set_chan_info(struct mwx_softc *, int);
void		mt7921_mcu_build_sku(struct mwx_softc *, int, int8_t *);
int		mt7921_mcu_rate_txpower_band(struct mwx_softc *, int,
		    const uint8_t *, int, int);
int		mt7921_mcu_set_rate_txpower(struct mwx_softc *);
void		mt7921_mac_reset_counters(struct mwx_softc *);
void		mt7921_mac_set_timing(struct mwx_softc *);
int		mt7921_mcu_uni_add_dev(struct mwx_softc *, struct mwx_vif *,
		    struct mwx_node *, int);
int		mt7921_mcu_set_sniffer(struct mwx_softc *, int);
int		mt7921_mcu_set_beacon_filter(struct mwx_softc *, int);
int		mt7921_mcu_set_rxfilter(struct mwx_softc *, uint32_t, uint8_t,
		    uint32_t);
void		mwx_awdl_restore_filter(struct mwx_softc *);
int		mt7921_mcu_set_bss_pm(struct mwx_softc *, int);
int		mt7921_mcu_set_tx(struct mwx_softc *, struct mwx_vif *);
int		mt7921_mac_fill_rx(struct mwx_softc *, struct mbuf *,
		    struct ieee80211_rxinfo *);
uint32_t	mt7921_mac_tx_rate_val(struct mwx_softc *);
void		mt7921_mac_write_txwi_80211(struct mwx_softc *, struct mbuf *,
		    struct ieee80211_node *, struct mt76_txwi *);
void		mt7921_mac_add_txs(struct mwx_softc *, const uint32_t *);
void		mt7921_mac_write_txwi(struct mwx_softc *, struct mbuf *,
		    struct ieee80211_node *, struct mt76_txwi *);
void		mwx_mac_tx_free(struct mwx_softc *, struct mbuf *);
int		mt7921_set_channel(struct mwx_softc *);
int mt7921_mcu_set_associated_bss(struct mwx_softc *, int);
int mt7921_mcu_set_associated_peer(struct mwx_softc *);
static int mwx_send_assoc_status(struct mwx_softc *, uint32_t, void *, size_t);
void		mwx_apply_phy_knobs(struct mwx_softc *);
void		mwx_setup_he_caps(struct mwx_softc *);
int		mt7921_mcu_set_bss_he(struct mwx_softc *,
		    struct ieee80211_node *, int);
void		mwx_set_bss_bw(struct mwx_softc *, struct ieee80211_node *);
int		mwx_ampdu_rx_start(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
void		mwx_ampdu_rx_stop(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
int		mwx_ampdu_tx_start(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
void		mwx_ampdu_tx_stop(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
void		mwx_ba_task(void *);
int		mt7921_mcu_sta_rx_ba(struct mwx_softc *,
		    struct ieee80211_node *, uint8_t, int);
int		mt7921_mcu_sta_tx_ba(struct mwx_softc *,
		    struct ieee80211_node *, uint8_t, int);

struct mbuf	*mwx_alloc_sta_req_tlv(int);
void		*mwx_append_len(struct mbuf *, int);
void		*mwx_append_tlv(struct mbuf *, uint16_t *, int, int);
struct mbuf	*mwx_fill_sta_req_hdr(struct mbuf *, struct mwx_vif *,
		    uint8_t, uint16_t, uint16_t);
void		 mt7921_mcu_add_basic_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *, int, int);
void		 mt7921_mcu_add_sta_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *, int);
void		 mt7921_mcu_add_sta_he_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *);
int		 mt7921_mcu_wtbl_generic_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *);
int		 mt7921_mcu_wtbl_hdr_trans_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *);
int		 mt7921_mcu_wtbl_ht_tlv(struct mbuf *, uint16_t *,
		    struct mwx_softc *, struct ieee80211_node *, int);
int		 mt7921_mac_sta_remove(struct mwx_softc *);
int		 mt7921_mac_sta_update(struct mwx_softc *,
		    struct ieee80211_node *, int, int);
void		 mt7921_mcu_add_key_tlv(struct mbuf *, uint16_t *,
		    struct ieee80211_key *, int);
int		 mt7921_mcu_sta_key_update(struct mwx_softc *,
		    struct ieee80211_node *, struct ieee80211_key *);
void		 mt7921_mcu_sta_key_delete(struct mwx_softc *,
			    struct ieee80211_node *, struct ieee80211_key *);

static inline int
mwx_dbm_to_rssi(int dbm)
{
	if (dbm <= -100)
		return 0;
	if (dbm >= 0)
		return 100;
	return dbm + 100;
}

static inline uint32_t
mwx_read(struct mwx_softc *sc, uint32_t reg)
{
	reg = mwx_reg_addr(sc, reg);
	return bus_space_read_4(sc->sc_st, sc->sc_memh, reg);
}

static inline void
mwx_write(struct mwx_softc *sc, uint32_t reg, uint32_t val)
{
	reg = mwx_reg_addr(sc, reg);
	bus_space_write_4(sc->sc_st, sc->sc_memh, reg, val);
}

static inline void
mwx_barrier(struct mwx_softc *sc)
{
	bus_space_barrier(sc->sc_st, sc->sc_memh, 0, sc->sc_mems,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
}

static inline uint32_t
mwx_rmw(struct mwx_softc *sc, uint32_t reg, uint32_t val, uint32_t mask)
{
	reg = mwx_reg_addr(sc, reg);
	val |= bus_space_read_4(sc->sc_st, sc->sc_memh, reg) & ~mask;
	bus_space_write_4(sc->sc_st, sc->sc_memh, reg, val);
	return val;
}

static inline uint32_t
mwx_set(struct mwx_softc *sc, uint32_t reg, uint32_t bits)
{
	return mwx_rmw(sc, reg, bits, 0);
}

static inline uint32_t
mwx_clear(struct mwx_softc *sc, uint32_t reg, uint32_t bits)
{
	return mwx_rmw(sc, reg, 0, bits);
}

static inline uint32_t
mt7921_map_reg_l1(struct mwx_softc *sc, uint32_t reg)
{
	uint32_t offset = MWX_HIF_REG_OFFSET(reg);
	uint32_t base = MWX_HIF_REG_BASE(reg);

	mwx_rmw(sc, MT7921_HIF_REMAP_L1,
	    base << MT7921_HIF_REMAP_L1_SHIFT, MT7921_HIF_REMAP_L1_MASK);
	mwx_barrier(sc);

	return MT7921_HIF_REMAP_BASE_L1 + offset;
}

/*
 * Poll for timeout milliseconds or until register reg read the value val
 * after applying the mask to the value read. Returns 0 on success ETIMEDOUT
 * on failure.
 */
int
mwx_poll(struct mwx_softc *sc, uint32_t reg, uint32_t val, uint32_t mask,
    int timeout)
{
	uint32_t cur;

	reg = mwx_reg_addr(sc, reg);
	timeout *= 100;
	do {
		cur = bus_space_read_4(sc->sc_st, sc->sc_memh, reg) & mask;
		if (cur == val)
			return 0;
		delay(10);
	} while (timeout-- > 0);

	DPRINTF("%s: poll timeout reg %x val %x mask %x cur %x\n",
	    DEVNAME(sc), reg, val, mask, cur);
	return ETIMEDOUT;
}

/*
 * ifp specific functions
 */
static int mwx_init_running(struct ifnet *);

int
mwx_init(struct ifnet *ifp)
{
	struct ieee80211com *ic = &((struct mwx_softc *)ifp->if_softc)->sc_ic;
	int rv;

	if (ic->ic_event_handler)
		ic->ic_event_handler(ic, IEEE80211_EVT_DRIVER_RESET_BEGIN, NULL);
	rv = mwx_init_running(ifp);
	/* WCL marks the adapter available only once the HAL is running. */
	if (rv != 0)
		ic->ic_wcl_mac_reconfig = false;
	if (ic->ic_event_handler)
		ic->ic_event_handler(ic, rv == 0 ?
		    IEEE80211_EVT_DRIVER_READY : IEEE80211_EVT_DRIVER_STOPPED,
		    NULL);
	return rv;
}

static int
mwx_init_running(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct mwx_node *mn;
	int rv;

	if (!sc->sc_fw_loaded) {
		rv = mwx_preinit(sc);
		if (rv) {
			MWX_DEV_LOG(sc, "init preinit failed error=%d\n", rv);
			return rv;
		}
	}
	/* WCL assigns a private MAC through setHardwareAddress (ac_enaddr). */
	IEEE80211_ADDR_COPY(ic->ic_myaddr, ((struct arpcom *)ifp)->ac_enaddr);
	MWX_DEV_LOG(sc, "init mac address=%s\n", ether_sprintf(ic->ic_myaddr));

	MWX_DEV_LOG(sc, "init start opmode=%d flags=0x%x ic_state=%d\n",
	    ic->ic_opmode, ifp->if_flags, ic->ic_state);
	if ((rv = mwx_mcu_drv_pmctrl(sc)) != 0) {
		MWX_DEV_LOG(sc, "init driver own failed error=%d\n", rv);
		return rv;
	}
	if ((rv = mwx_dma_reinit_cond(sc)) != 0) {
		MWX_DEV_LOG(sc, "init DMA recovery failed error=%d\n", rv);
		return rv;
	}
	rv = mwx_mcu_set_deep_sleep(sc, 0);
	if (rv)
		MWX_DEV_LOG(sc, "init disable deep sleep returned %d\n", rv);
	else
		MWX_DEV_LOG(sc, "init deep sleep disabled\n");

	rv = mt7921_mcu_set_mac_enable(sc, 0, 1);
	if (rv) {
		MWX_DEV_LOG(sc, "init mac enable failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init mac enable ok\n");

	rv = mt7921_mcu_set_channel_domain(sc);
	if (rv) {
		MWX_DEV_LOG(sc, "init channel domain failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init channel domain ok\n");

#if 0
	/* XXX no channel available yet */
	rv = mt7921_mcu_set_chan_info(sc, MCU_EXT_CMD_SET_RX_PATH);
	if (rv)
		return rv;
#endif

	rv = mt7921_mcu_set_rate_txpower(sc);
	if (rv) {
		MWX_DEV_LOG(sc, "init rate txpower failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init rate txpower ok\n");

	mt7921_mac_reset_counters(sc);
	MWX_DEV_LOG(sc, "init counters reset\n");

	mn = &sc->sc_vif.vif_mn;
	MWX_DEV_LOG(sc, "init vif node=%p wcid=%u\n", mn,
	    mn != NULL ? mn->wcid : 0);

	mwx_mac_wtbl_update(sc, mn->wcid);
	MWX_DEV_LOG(sc, "init wtbl updated wcid=%u\n", mn->wcid);

	rv = mt7921_mcu_uni_add_dev(sc, &sc->sc_vif, mn, 1);
	if (rv) {
		MWX_DEV_LOG(sc, "init add dev failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init add dev ok\n");

	rv = mt7921_mcu_set_tx(sc, &sc->sc_vif);
	if (rv) {
		MWX_DEV_LOG(sc, "init set tx failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "init set tx ok\n");

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		rv = mt7921_mcu_set_chan_info(sc, MCU_EXT_CMD_SET_RX_PATH);
		if (rv)
			return rv;
		rv = mt7921_set_channel(sc);
		if (rv)
			return rv;

		rv = mt7921_mcu_set_sniffer(sc, 1);
		MWX_DEV_LOG(sc, "init monitor sniffer enable returned %d\n", rv);
		rv = mt7921_mcu_set_beacon_filter(sc, 0);
		MWX_DEV_LOG(sc, "init monitor beacon filter disable returned %d\n", rv);
		sc->sc_rxfilter = 0;
		mwx_set(sc, MT_DMA_DCR0(0), MT_DMA_DCR0_RXD_G5_EN);
	} else {
		rv = mt7921_mcu_set_sniffer(sc, 0);
		MWX_DEV_LOG(sc, "init sniffer disable returned %d\n", rv);
		sc->sc_rxfilter |= MT_WF_RFCR_DROP_OTHER_UC;
		mwx_clear(sc, MT_DMA_DCR0(0), MT_DMA_DCR0_RXD_G5_EN);
	}
	mwx_write(sc, MT_WF_RFCR(0), sc->sc_rxfilter);
	MWX_DEV_LOG(sc, "init rxfilter set 0x%08x\n", sc->sc_rxfilter);

	ifp->if_flags |= IFF_RUNNING;
    if (sc->sc_awdl_lock) {
        IOLockLock(sc->sc_awdl_lock);
        sc->sc_awdl_stopping = false;
        IOLockUnlock(sc->sc_awdl_lock);
    }
	ifq_clr_oactive(&ifp->if_snd);
	MWX_DEV_LOG(sc, "init interface running flags=0x%x\n", ifp->if_flags);
	sc->sc_dma_stall_logged = 0;
	timeout_add_sec(&sc->sc_dma_to, 2);

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		ic->ic_bss->ni_chan = ic->ic_ibss_chan;
		ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
		MWX_DEV_LOG(sc, "init monitor state run\n");
		return 0;
	}

	MWX_DEV_LOG(sc, "init begin scan\n");
	ieee80211_begin_scan(ifp);
	MWX_DEV_LOG(sc, "init begin scan returned state=%d flags=0x%x sc_flags=0x%x\n",
	    ic->ic_state, ifp->if_flags, sc->sc_flags);

	/*
	 * ieee80211_begin_scan() ends up scheduling mwx_newstate_task().
	 * Wait until the transition to SCAN state has completed.
	 */

	return 0;
}

void
mwx_stop(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int s = splnet();
	int was_running = (ifp->if_flags & IFF_RUNNING) != 0;

    // MCU waits release the workloop gate. Invalidate and drain AWDL work
    // before removing the firmware context or reclaiming its TX entries.
    mwx_awdl_quiesce(sc);

	DPRINTF("%s: stop\n", DEVNAME(sc));

	/* Cancel scheduled tasks and let any stale tasks finish up. */
	if (sc->sc_nswq != NULL) {
		task_del(sc->sc_nswq, &sc->sc_newstate_task);
		task_del(sc->sc_nswq, &sc->sc_bgscan_done_task);
		task_del(sc->sc_nswq, &sc->sc_setkey_task);
		task_del(sc->sc_nswq, &sc->sc_reset_task);
		task_del(sc->sc_nswq, &sc->sc_ba_task);
	}
	sc->sc_ba_cur = sc->sc_ba_tail = 0;
	task_del(systq, &sc->sc_scan_task);
	timeout_del(&sc->sc_reset_to);
	timeout_del(&sc->sc_scan_to);
	timeout_del(&sc->sc_dma_to);

	ifp->if_timer = 0;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);

	if (was_running) {
		struct mwx_node *mn;
        if (sc->sc_awdl_configured) {
            mwx_awdl_restore_filter(sc);
            mwx_awdl_remove_context(sc,sc->sc_awdl_epoch);
        }
        sc->sc_awdl_filter_active=false;

		free(sc->sc_bgscan_arg, M_DEVBUF, sc->sc_bgscan_arg_size);
		sc->sc_bgscan_arg = NULL;
		sc->sc_bgscan_arg_size = 0;
		if (sc->sc_flags & (MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN)) {
			mt7921_mcu_hw_scan_cancel(sc);
			sc->sc_flags &= ~(MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN);
			/*
			 * The cancel is asynchronous. Removing the vif while
			 * the firmware is still leaving the scan channel makes
			 * it coredump (power off while associated + bgscan).
			 */
			delay(200 * 1000);
		}
		mt7921_mac_sta_remove(sc);

		mn = &sc->sc_vif.vif_mn;
		mt7921_mcu_uni_add_dev(sc, &sc->sc_vif, mn, 0);
		mwx_mcu_set_deep_sleep(sc, 1);
		mt7921_mcu_set_mac_enable(sc, 0, 0);
	}

	sc->sc_newstate(ic, IEEE80211_S_INIT, -1);
	sc->sc_ns_state = IEEE80211_S_INIT;
	sc->sc_sta_wcid = 0;
	ifp->if_timer = 0;

	if (was_running) {
		/* The firmware scan was cancelled above; no completion will
		 * clear these. Clear them before advertising the stop. */
		ic->ic_flags &= ~(IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN);
		ic->ic_wcl_scan_requested = false;
		ic->ic_wcl_scan_inflight = false;
		ic->ic_wcl_scan_active = false;
		ic->ic_wcl_scan_ssid_length = 0;
		ic->ic_wcl_scan_targets = {};
		if (ic->ic_event_handler)
			ic->ic_event_handler(ic, IEEE80211_EVT_DRIVER_STOPPED, NULL);
	}
	splx(s);
}

void
mwx_watchdog(struct ifnet *ifp)
{
	ifp->if_timer = 0;
	ieee80211_watchdog(ifp);
}

void
mwx_start(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct ether_header *eh;
	struct mbuf *m;

	if (!(ifp->if_flags & IFF_RUNNING) || ifq_is_oactive(&ifp->if_snd))
		return;

	for (;;) {
		/*
		 * Stop before a frame would be dropped for lack of a TXWI or
		 * a ring slot. mwx_tx_resume() restarts output once the
		 * firmware hands TXWIs back.
		 */
		if (mwx_tx_full(sc)) {
			ifq_set_oactive(&ifp->if_snd);
			sc->sc_tx_stalls++;
			break;
		}

		/* need to send management frames even if we're not RUNning */
		m = mq_dequeue(&ic->ic_mgtq);
		if (m) {
			ni = (struct ieee80211_node *)mbuf_pkthdr_rcvif(m);
			goto sendit;
		}

		if (ic->ic_state != IEEE80211_S_RUN ||
		    (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY))
			break;

		m = ifq_dequeue(&ifp->if_snd);
		if (!m)
			break;
		if (mbuf_len(m) < sizeof (*eh) &&
		    (m = m_pullup(m, sizeof (*eh))) == NULL) {
			ifp->if_oerrors++;
			continue;
		}
#if NBPFILTER > 0
		if (ifp->if_bpf != NULL)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
#endif
		if ((m = ieee80211_encap(ifp, m, &ni)) == NULL) {
			ifp->if_oerrors++;
			continue;
		}

 sendit:
#if NBPFILTER > 0
		if (ic->ic_rawbpf != NULL)
			bpf_mtap(ic->ic_rawbpf, m, BPF_DIRECTION_OUT);
#endif
		int rv = mwx_tx(sc, m, ni);
		/* mwx_tx copies node metadata into the descriptor; completion uses
		 * the packet/token only, so release the enqueue reference here. */
		ieee80211_release_node(ic, ni);
		if (rv != 0) {
			ifp->if_oerrors++;
			continue;
		}

		if (ifp->if_flags & IFF_UP)
			ifp->if_timer = 1;
	}
}

int
mwx_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	int s, err = 0;

	s = splnet();
	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING)) {
				mwx_stop(ifp);
				err = mwx_init(ifp);
			}
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				mwx_stop(ifp);
		}
		break;
	default:
		err = ieee80211_ioctl(ifp, cmd, data);
		break;
	}

	if (err == ENETRESET) {
		err = 0;
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
		    (IFF_UP | IFF_RUNNING)) {
			mwx_stop(ifp);
			err = mwx_init(ifp);
		}
	}
	splx(s);
	return err;
}

int
mwx_media_change(struct ifnet *ifp)
{
	struct mwx_softc *sc = (struct mwx_softc *)ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int err;

	err = ieee80211_media_change(ifp);
	if (err != ENETRESET)
		return err;

	/* TODO lot more handling here */
	if (ic->ic_fixed_mcs != -1) {
		;
	} else if (ic->ic_fixed_rate != -1) {
		;
	}
	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
	    (IFF_UP | IFF_RUNNING)) {
		/* XXX could be a bit harsh */
		mwx_stop(ifp);
		err = mwx_init(ifp);
	}
	return err;
}

/*
 * net80211 specific functions.
 */

struct ieee80211_node *
mwx_node_alloc(struct ieee80211com *ic)
{
	/* XXX this is just wrong */
	static uint32_t wcid = 0;
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_node *mn;

	mn = (struct mwx_node *)malloc(sizeof(struct mwx_node), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (mn == NULL)
		return NULL;
	mn->wcid = 1 + wcid++ % (MWX_WTBL_STA - 1);

	/* init WCID table entry */
	mwx_mac_wtbl_update(sc, mn->wcid);

	return &mn->ni;
}

void
mwx_newassoc(struct ieee80211com *ic, struct ieee80211_node *ni, int isnew)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_node *mn = (struct mwx_node *)ni;
	uint16_t wcid = 0;

	if (isnew && ni->ni_associd != 0) {
		/* only interested in true associations */
		wcid = IEEE80211_AID(ni->ni_associd);

	}
	printf("%s: new assoc isnew=%d addr=%s WCID=%d\n", DEVNAME(sc),
	    isnew, ether_sprintf(ni->ni_macaddr), mn->wcid);

	/* XXX TODO rate handling here */
}

#ifndef IEEE80211_STA_ONLY
void
mwx_node_leave(struct ieee80211com *ic, struct ieee80211_node *ni)
{
#if 0
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_node *mn = (struct mwx_node *)ni;
	uint16_t wcid = mn->wcid;

	/* TODO clear WCID */
#endif
}
#endif

int
mwx_scan(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	int rv;

	if (sc->sc_flags & MWX_FLAG_BGSCAN) {
		rv = mt7921_mcu_hw_scan_cancel(sc);
		if (rv) {
			printf("%s: could not abort background scan\n",
			    DEVNAME(sc));
			return rv;
		}
	}

	rv = mt7921_mcu_hw_scan(sc, 0);
	if (rv) {
		printf("%s: could not initiate scan\n", DEVNAME(sc));
		return rv;
	}
    if(sc->sc_awdl_scan_suspended) return 0;
	sc->sc_scan_requests++;
	/* End the scan ourselves if the firmware never reports SCAN_DONE. */
	timeout_add_sec(&sc->sc_scan_to, 12);

	/*
	 * The current mode might have been fixed during association.
	 * Ensure all channels get scanned.
	 */
	if (IFM_MODE(ic->ic_media.ifm_cur->ifm_media) != IFM_AUTO)
		ieee80211_setmode(ic, IEEE80211_MODE_AUTO);

	sc->sc_flags |= MWX_FLAG_SCANNING;
	if (ifp->if_flags & IFF_DEBUG)
		printf("%s: %s -> %s\n", ifp->if_xname,
		    ieee80211_state_name[ic->ic_state],
		    ieee80211_state_name[IEEE80211_S_SCAN]);
	if ((sc->sc_flags & MWX_FLAG_BGSCAN) == 0) {
		ieee80211_set_link_state(ic, LINK_STATE_DOWN);
		ieee80211_node_cleanup(ic, ic->ic_bss);
	}
	ic->ic_state = IEEE80211_S_SCAN;

	return 0;
}

int
mwx_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	/*
	 * Prevent attempts to transition towards the same state, unless
	 * we are scanning in which case a SCAN -> SCAN transition
	 * triggers another scan iteration. And AUTH -> AUTH is needed
	 * to support band-steering.
	 */
	if (sc->sc_ns_state == nstate && nstate != IEEE80211_S_SCAN &&
	    nstate != IEEE80211_S_AUTH)
		return 0;

	if (ic->ic_state == IEEE80211_S_RUN) {
		/* cancel other tasks here */
	}

	sc->sc_ns_state = nstate;
	sc->sc_ns_arg = arg;

	task_add(sc->sc_nswq, &sc->sc_newstate_task);
	return 0;
}

void
mwx_newstate_task(void *ptr)
{
	struct mwx_softc *sc = (struct mwx_softc *)ptr;
	struct ieee80211com *ic = &sc->sc_ic;
	enum ieee80211_state ostate = ic->ic_state;
	enum ieee80211_state nstate = sc->sc_ns_state;
	int arg = sc->sc_ns_arg;
	int s = splnet();
	int rv = 0;

	switch (ostate) {
	case IEEE80211_S_RUN:
		if (nstate != ostate)
			mwx_mcu_set_deep_sleep(sc, 1);
		break;
	case IEEE80211_S_SCAN:
		if (nstate == ostate) {
			if (sc->sc_flags & MWX_FLAG_SCANNING) {
				splx(s);
				return;
			}
		}
		break;
	default:
		break;
	}

	DPRINTF("%s: %s %d -> %d\n", DEVNAME(sc), __func__, ostate, nstate);
	MWX_DEV_LOG(sc, "newstate task %d -> %d arg=%d\n", ostate, nstate, arg);

	/*
	 * Leaving the BSS: drop the AP STA_REC (and the Block Ack sessions
	 * on it) like mac80211 does before the vif goes away.
	 */
	if (ostate >= IEEE80211_S_AUTH && nstate < IEEE80211_S_AUTH)
		mt7921_mac_sta_remove(sc);

	/* Scan, auth and assoc run on the primary 20 MHz channel. */
	if (nstate != IEEE80211_S_RUN) {
		sc->sc_bss_bw = CMD_CBW_20MHZ;
		sc->sc_bss_center = 0;
	}

	switch (nstate) {
	case IEEE80211_S_INIT:
		break;
	case IEEE80211_S_SCAN:
		rv = mwx_scan(sc);
		if (rv)
			break;
		break;
	case IEEE80211_S_AUTH:
		mwx_apply_phy_knobs(sc);
		rv = mt7921_set_channel(sc);
		if (rv)
			break;
		mwx_mcu_set_deep_sleep(sc, 0);
		if (sc->sc_sta_wcid != 0 &&
		    sc->sc_sta_wcid != ((struct mwx_node *)ic->ic_bss)->wcid)
			mt7921_mac_sta_remove(sc);
		if (mt7921_mac_sta_update(sc, ic->ic_bss, 1, 1) == 0) {
			sc->sc_sta_wcid = ((struct mwx_node *)ic->ic_bss)->wcid;
			IEEE80211_ADDR_COPY(sc->sc_sta_addr,
			    ic->ic_bss->ni_macaddr);
		}
		break;
	case IEEE80211_S_ASSOC:
		/*
		 * Stay in full power until RUN. Deep sleep here made the
		 * firmware miss Association Responses from APs that take
		 * more than a few ms to answer (Linux keeps the chip awake
		 * with a JOIN ROC for the whole auth/assoc exchange).
		 */
		break;
	case IEEE80211_S_RUN:
		if (ic->ic_opmode == IEEE80211_M_MONITOR)
			break;

		mt7921_mcu_hw_scan_cancel(sc);
		mwx_mcu_set_deep_sleep(sc, 0);
		mwx_set_bss_bw(sc, ic->ic_bss);
		if (sc->sc_bss_bw != CMD_CBW_20MHZ) {
			rv = mt7921_set_channel(sc);
			MWX_DEV_LOG(sc, "run channel switch bw=%u center=%u rv=%d\n",
			    sc->sc_bss_bw, sc->sc_bss_center, rv);
			if (rv) {
				/* Stay usable on the primary channel. */
				sc->sc_bss_bw = CMD_CBW_20MHZ;
				sc->sc_bss_center = 0;
				rv = mt7921_set_channel(sc);
				if (rv)
					break;
			}
		}
		rv = mt7921_mcu_set_associated_bss(sc, 1);
		if (!rv)
			rv = mt7921_mcu_set_associated_peer(sc);
		if (rv)
			break;
		mt7921_mcu_set_rts_thresh(sc, 0x92b, 0);
		break;
	}

	if (rv)
		mwx_reset(sc);
	else
		sc->sc_newstate(ic, nstate, arg);
	splx(s);
}

int
mwx_bgscan(struct ieee80211com *ic)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	int err;

	if (sc->sc_flags & MWX_FLAG_SCANNING)
		return 0;

	err = mt7921_mcu_hw_scan(sc, 1);
	if (err) {
		printf("%s: could not initiate scan\n", DEVNAME(sc));
		return err;
	}
    // No firmware scan was submitted. Returning success here makes
    // net80211 set F_BGSCAN forever, rejecting later WCL scans as busy.
    if(sc->sc_awdl_scan_suspended) return EBUSY;

	sc->sc_flags |= MWX_FLAG_BGSCAN;
	sc->sc_scan_requests++;
	/*
	 * A lost SCAN_DONE leaves IEEE80211_F_BGSCAN set and every later
	 * scan request from airportd is then ignored: end it ourselves.
	 */
	timeout_add_sec(&sc->sc_scan_to, 12);
	return 0;
}

void
mwx_bgscan_done(struct ieee80211com *ic,
    struct ieee80211_node_switch_bss_arg *arg, size_t arg_size)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	free(sc->sc_bgscan_arg, M_DEVBUF, sc->sc_bgscan_arg_size);
	sc->sc_bgscan_arg = arg;
	sc->sc_bgscan_arg_size = arg_size;
	task_add(sc->sc_nswq, &sc->sc_bgscan_done_task);
}

void
mwx_bgscan_done_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	int err = 0;
	int s = splnet();

	if (ic->ic_state != IEEE80211_S_RUN ||
	    (ic->ic_flags & IEEE80211_F_BGSCAN) == 0) {
		err = ENXIO;
		goto done;
	}

	/*
	 * Remove installed crypto keys while we still have access to them.
	 * Once mwx_newstate() is entered ic_bss will already contain
	 * information about our next AP.
	 */
	if (ic->ic_flags & IEEE80211_F_RSNON) {
		struct ieee80211_key *k;

		if (ni->ni_pairwise_key.k_cipher != IEEE80211_CIPHER_NONE)
			(*ic->ic_delete_key)(ic, ni, &ni->ni_pairwise_key);

		if (ic->ic_def_txkey == 1 || ic->ic_def_txkey == 2) {
			k = &ic->ic_nw_keys[ic->ic_def_txkey];
			if ((k->k_flags & IEEE80211_KEY_GROUP) &&
			    k->k_cipher == IEEE80211_CIPHER_CCMP)
				(*ic->ic_delete_key)(ic, ni, k);
		}

		if (ic->ic_igtk_kid == 4 || ic->ic_igtk_kid == 5) {
			k = &ic->ic_nw_keys[ic->ic_igtk_kid];
			if (k->k_flags & IEEE80211_KEY_IGTK)
				(*ic->ic_delete_key)(ic, ni, k);
		}

		ni->ni_port_valid = 0;
		ni->ni_flags &= ~IEEE80211_NODE_TXRXPROT;
		ni->ni_flags &= ~IEEE80211_NODE_TXMGMTPROT;
		ni->ni_flags &= ~IEEE80211_NODE_RXMGMTPROT;
		ni->ni_rsn_supp_state = RSNA_SUPP_INITIALIZE;
	}

	/*
	 * The local net80211 port switches BSS from ni_unref_cb when the
	 * current node's final reference is released. Install the same
	 * callback after the driver has removed hardware keys.
	 */
	ni->ni_unref_arg = sc->sc_bgscan_arg;
	ni->ni_unref_arg_size = sc->sc_bgscan_arg_size;
	ni->ni_unref_cb = ieee80211_node_switch_bss;
	sc->sc_bgscan_arg = NULL;
	sc->sc_bgscan_arg_size = 0;
	if (ni->ni_flags & IEEE80211_NODE_MFP)
		ieee80211_node_switch_bss(ic, ni);

 done:
	if (err) {
		free(sc->sc_bgscan_arg, M_DEVBUF, sc->sc_bgscan_arg_size);
		sc->sc_bgscan_arg = NULL;
		sc->sc_bgscan_arg_size = 0;
	}
	splx(s);
}

int
mwx_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	struct mwx_setkey_task_arg *a;

	DPRINTF("%s: set_key: ni %p, k_id %d, k_flags %x k_cipher %d\n",
	    DEVNAME(sc), ni, k->k_id, k->k_flags, k->k_cipher);

	if ((sc->sc_setkey_cur + 1) % nitems(sc->sc_setkey_arg) ==
	    sc->sc_setkey_tail)
		return ENOSPC;

	/*
	 * MtkWireless: net80211 verifies the MMIE of broadcast robust
	 * management frames itself (ieee80211_bip_decap), so the IGTK
	 * needs its software context too. The firmware copy follows.
	 */
	if (k->k_cipher == IEEE80211_CIPHER_BIP) {
		int err;

		if (k->k_flags & IEEE80211_KEY_SWCRYPTO)
			ieee80211_bip_delete_key(ic, k);
		err = ieee80211_bip_set_key(ic, k);
		if (err != 0)
			return err;
		k->k_flags |= IEEE80211_KEY_SWCRYPTO;
	}

	a = &sc->sc_setkey_arg[sc->sc_setkey_cur];
	a->ni = ni;
	a->k = k;
	sc->sc_setkey_cur = (sc->sc_setkey_cur + 1) % nitems(sc->sc_setkey_arg);
	task_add(sc->sc_nswq, &sc->sc_setkey_task);
	return EBUSY;
}

void
mwx_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	DPRINTF("%s: delete_key: ni %p, k_id %d, k_flags %x k_cipher %d\n",
	    DEVNAME(sc), ni, k->k_id, k->k_flags, k->k_cipher);

	mt7921_mcu_sta_key_delete(sc, ni, k);
	/* MtkWireless: free net80211's BIP context of the IGTK. */
	if (k->k_cipher == IEEE80211_CIPHER_BIP &&
	    (k->k_flags & IEEE80211_KEY_SWCRYPTO)) {
		ieee80211_bip_delete_key(ic, k);
		k->k_flags &= ~IEEE80211_KEY_SWCRYPTO;
	}
}

void
mwx_setkey_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct mwx_setkey_task_arg *a;
	int s = splnet();

	while (sc->sc_setkey_tail != sc->sc_setkey_cur) {
		a = &sc->sc_setkey_arg[sc->sc_setkey_tail];
		mt7921_mcu_sta_key_update(sc, a->ni, a->k);
		a->ni = NULL;
		a->k = NULL;

		sc->sc_setkey_tail = (sc->sc_setkey_tail + 1) %
		    nitems(sc->sc_setkey_arg);
	}

	splx(s);
}

#if NBPFILTER > 0
void
mwx_radiotap_attach(struct mwx_softc *sc)
{
	bpfattach(&sc->sc_drvbpf, &sc->sc_ic.ic_if, DLT_IEEE802_11_RADIO,
	    sizeof(struct ieee80211_frame) + IEEE80211_RADIOTAP_HDRLEN);

	sc->sc_rxtap_len = sizeof sc->sc_rxtapu;
	sc->sc_rxtap.wr_ihdr.it_len = htole16(sc->sc_rxtap_len);
	sc->sc_rxtap.wr_ihdr.it_present = htole32(MWX_RX_RADIOTAP_PRESENT);

	sc->sc_txtap_len = sizeof sc->sc_txtapu;
	sc->sc_txtap.wt_ihdr.it_len = htole16(sc->sc_txtap_len);
	sc->sc_txtap.wt_ihdr.it_present = htole32(MWX_TX_RADIOTAP_PRESENT);
}
#endif

/* Copy a chain into one cluster; frees m and returns the copy, or NULL. */
static struct mbuf *
mwx_mbuf_linearize(struct mbuf *m)
{
	size_t len = mbuf_pkthdr_len(m);
	struct mbuf *n = MCLGETL(NULL, M_DONTWAIT, len);

	if (n == NULL)
		return NULL;
	m_copydata(m, 0, len, mtod(n, caddr_t));
	mbuf_setlen(n, len);
	mbuf_pkthdr_setlen(n, len);
	m_freem(m);
	return n;
}

/*
 * Return 1 and the EAPOL-Key key information field if m is an unprotected
 * 802.11 data frame carrying an EAPOL-Key, for handshake debugging.
 */
static int
mwx_eapol_keyinfo(struct mbuf *m, uint16_t *keyinfo)
{
	static const uint8_t llc[] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
	struct ieee80211_frame *wh = mtod(m, struct ieee80211_frame *);
	uint8_t buf[sizeof(llc) + 7];
	u_int hdrlen;

	if (mbuf_len(m) < sizeof(*wh) ||
	    (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA ||
	    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED))
		return 0;
	hdrlen = ieee80211_get_hdrlen(wh);
	if (mbuf_pkthdr_len(m) < hdrlen + sizeof(buf))
		return 0;
	m_copydata(m, hdrlen, sizeof(buf), (caddr_t)buf);
	/* LLC/SNAP 888e, EAPOL type 3 (Key), then descriptor type + info */
	if (memcmp(buf, llc, sizeof(llc)) != 0 || buf[sizeof(llc) + 1] != 3)
		return 0;
	*keyinfo = (buf[sizeof(llc) + 5] << 8) | buf[sizeof(llc) + 6];
	return 1;
}

int
mwx_tx(struct mwx_softc *sc, struct mbuf *m, struct ieee80211_node *ni)
{
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct mwx_txwi *mt;
	struct mt76_txwi *txp;
	int rv;

	if ((sc->sc_knobs & MWX_KNOB_LINEARIZE) && mbuf_next(m) != NULL) {
		struct mbuf *n = mwx_mbuf_linearize(m);

		if (n != NULL)
			m = n;
	}

	if ((mt = mwx_txwi_get(sc)) == NULL) {
		sc->sc_tx_nobufs++;
		m_freem(m);
		return ENOBUFS;
	}
	/* XXX DMA memory access without BUS_DMASYNC_PREWRITE */
	txp = mt->mt_desc;
	memset(txp, 0, sizeof(*txp));
	mt7921_mac_write_txwi(sc, m, ni, txp);

	rv = mwx_txwi_enqueue(sc, mt, m);
	if (rv == EFBIG && mbuf_next(m) != NULL) {
		struct mbuf *n = mwx_mbuf_linearize(m);
		static uint32_t linearize_log_count;

		if (n != NULL) {
			m = n;
			rv = mwx_txwi_enqueue(sc, mt, m);
		}
		if (linearize_log_count < 8) {
			MWX_DEV_LOG(sc, "tx linearized len=%zu rv=%d\n",
			    mbuf_pkthdr_len(m), rv);
			linearize_log_count++;
		}
	}
	if (rv != 0) {
		/* Mapping failed before ownership reached the TXWI. */
		m_freem(m);
		mwx_txwi_put(sc, mt);
		return rv;
	}

	{
		uint16_t ki;

		mt->mt_eapol = mwx_eapol_keyinfo(m, &ki);
		if (mt->mt_eapol) {
			MWX_DEV_LOG(sc, "eapol tx keyinfo=0x%04x len=%zu msdu=%u "
			    "knobs=0x%x\n", ki, mbuf_pkthdr_len(m), mt->mt_idx,
			    sc->sc_knobs);
			MWX_DEV_LOG(sc, "eapol akms=0x%x nflags=0x%x myaddr=%s\n",
			    ni->ni_rsnakms, ni->ni_flags,
			    ether_sprintf(sc->sc_ic.ic_myaddr));
		}
	}

	static uint32_t tx_log_count;
	if (tx_log_count < 64) {
		MWX_DEV_LOG(sc, "tx frame wcid=%u pid=%d len=%zu\n",
		    mn->wcid, mt->mt_idx, mbuf_pkthdr_len(m));
		MWX_DEV_LOG(sc,
		    "txwi %08x %08x %08x %08x %08x %08x %08x %08x\n",
		    txp->txwi[0], txp->txwi[1], txp->txwi[2], txp->txwi[3],
		    txp->txwi[4], txp->txwi[5], txp->txwi[6], txp->txwi[7]);
		MWX_DEV_LOG(sc,
		    "tx desc msdu=%u/%u/%u/%u len=%u/%u/%u/%u buf=0x%08x/0x%08x/0x%08x/0x%08x\n",
		    txp->msdu_id[0], txp->msdu_id[1], txp->msdu_id[2],
		    txp->msdu_id[3], txp->ptr[0].len0, txp->ptr[0].len1,
		    txp->ptr[1].len0, txp->ptr[1].len1, txp->ptr[0].buf0,
		    txp->ptr[0].buf1, txp->ptr[1].buf0, txp->ptr[1].buf1);
		tx_log_count++;
	}

	rv = mwx_dma_txwi_enqueue(sc, &sc->sc_txq, mt);
	if (rv != 0) {
		sc->sc_tx_ring_full++;
		mwx_txwi_put(sc, mt);
	}
	return rv;
}

void
mwx_rx(struct mwx_softc *sc, struct mbuf *m, struct mbuf_list *ml)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_node *ni;
	struct ieee80211_frame *wh;
	struct ieee80211_rxinfo rxi = { 0 };
	int rv;

	rv = mt7921_mac_fill_rx(sc, m, &rxi);
	if (rv != 0) {
		if (sc->sc_rx_fill_fail < 32) {
			MWX_DEV_LOG(sc,
			    "rx fill failed rv=%d len=%zu fail_count=%u\n",
			    rv, mbuf_len(m), sc->sc_rx_fill_fail + 1);
		}
		sc->sc_rx_fill_fail++;
		ifp->if_ierrors++;
		m_freem(m);
		return;
	}

    if (sc->sc_awdl_enabled && sc->sc_awdl_receive &&
        mbuf_len(m) >= 24 && !memcmp(mtod(m, uint8_t *)+16, mtkawdl::bssid, 6)) {
        sc->sc_awdl_receive(sc->sc_awdl_context, mtod(m, const uint8_t *),
            mbuf_len(m), rxi.rxi_chan, rxi.rxi_rssi);
        m_freem(m);
        return;
    }
	wh = mtod(m, struct ieee80211_frame *);
	if (sc->sc_rx_delivered < 32) {
		uint8_t type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
		uint8_t subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

		MWX_DEV_LOG(sc,
		    "rx frame delivered type=0x%02x subtype=0x%02x chan=%u rssi=%d len=%zu\n",
		    type, subtype, rxi.rxi_chan, rxi.rxi_rssi, mbuf_len(m));
	}
	{
		static uint32_t rx_run_log_count;
		uint8_t type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
		uint8_t subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

		if (rx_run_log_count < 48 &&
		    (type == IEEE80211_FC0_TYPE_DATA ||
		    (ic->ic_state == IEEE80211_S_RUN &&
		    (rxi.rxi_flags & IEEE80211_RXI_HWDEC)))) {
			MWX_DEV_LOG(sc,
			    "rx run frame state=%d fc=%02x/%02x type=0x%02x subtype=0x%02x rxi_flags=0x%x chan=%u rssi=%d len=%zu\n",
			    ic->ic_state, wh->i_fc[0], wh->i_fc[1], type, subtype,
			    rxi.rxi_flags, rxi.rxi_chan, rxi.rxi_rssi, mbuf_len(m));
			rx_run_log_count++;
		}
	}
	sc->sc_rx_delivered++;
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_CTL) {
		sc->sc_rx_ctl++;
		if ((wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) ==
		    IEEE80211_FC0_SUBTYPE_BAR)
			sc->sc_rx_bar++;
	}

	{
		uint16_t ki;

		if (mwx_eapol_keyinfo(m, &ki)) {
			MWX_DEV_LOG(sc, "eapol rx keyinfo=0x%04x len=%zu rssi=%d "
			    "fc=%02x/%02x\n", ki, mbuf_pkthdr_len(m),
			    rxi.rxi_rssi, wh->i_fc[0], wh->i_fc[1]);
		}
	}

	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    sc->sc_rx_cur_rate != 0) {
		/* RXV word 0: rate 6:0, nsts 9:7, bw 14:12, sgi 16:15, mode 27:24 */
		uint32_t v = sc->sc_rx_cur_rate;

		sc->sc_rx_mode_cnt[(v >> 24) & 0xf]++;
		sc->sc_rx_data_rate = v;
		if ((sc->sc_rx_data_cnt++ & 0xfff) == 0) {
			MWX_DEV_LOG(sc,
			    "rx rate mode=%u mcs=%u nss=%u bw=%u sgi=%u frames cck=%u ofdm=%u ht=%u vht=%u\n",
			    (v >> 24) & 0xf, v & 0x7f, ((v >> 7) & 0x7) + 1,
			    (v >> 12) & 0x7, (v >> 15) & 0x3,
			    sc->sc_rx_mode_cnt[0], sc->sc_rx_mode_cnt[1],
			    sc->sc_rx_mode_cnt[2] + sc->sc_rx_mode_cnt[3],
			    sc->sc_rx_mode_cnt[4]);
		}
	}

#if NBPFILTER > 0
	if (__predict_false(sc->sc_drvbpf != NULL)) {
		struct mwx_rx_radiotap_header *tap = &sc->sc_rxtap;
		uint32_t tsf_lo, tsf_hi;
		/* get timestamp (low and high 32 bits) */
		tsf_hi = 0;
		tsf_lo = 0;
		tap->wr_tsft = htole64(((uint64_t)tsf_hi << 32) | tsf_lo);
		tap->wr_flags = 0;
		tap->wr_rate = 2;	/* XXX */
		tap->wr_chan_freq =
		    htole16(ic->ic_channels[rxi.rxi_chan].ic_freq);
		tap->wr_chan_flags =
		    ic->ic_channels[rxi.rxi_chan].ic_flags;
		tap->wr_dbm_antsignal = rxi.rxi_rssi;
		bpf_mtap_hdr(sc->sc_drvbpf, tap, sc->sc_rxtap_len, m,
		    BPF_DIRECTION_IN);
	}
#endif

	/* grab a reference to the source node */
	ni = ieee80211_find_rxnode(ic, wh);

	if (ic->ic_max_rssi && rxi.rxi_rssi < 0)
		rxi.rxi_rssi = mwx_dbm_to_rssi(rxi.rxi_rssi);

	/* send the frame to the 802.11 layer */
	ieee80211_inputm(ifp, m, ni, &rxi, ml);

	/* node is no longer needed */
	ieee80211_release_node(ic, ni);
}

int
mwx_count_nodes(struct ieee80211com *ic)
{
	struct ieee80211_node *ni;
	int count = 0;

	RB_FOREACH(ni, ieee80211_tree, &ic->ic_tree)
		count++;
	return count;
}

/*
 * Driver specific functions.
 */
int
mwx_intr(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	uint32_t intr, intr_sw;
	uint32_t mask = sc->sc_intr_mask;
	static uint32_t intr_log_count;
	static uint32_t intr_unhandled_log_count;

	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	intr = mwx_read(sc, MT_WFDMA0_HOST_INT_STA);
	/* MtkWireless: all ones means the register is unreadable (device left
	 * the bus, link down, or a transient read while asleep): nothing to
	 * service. Re-enable as for a spurious interrupt, so a transient read
	 * cannot leave interrupts off; a write to a gone device is dropped. */
	if (intr == 0xffffffff) {
		static uint32_t gone_log_count;

		if (gone_log_count++ < 4)
			MWX_DEV_LOG(sc, "interrupt status reads all ones\n");
		mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, mask);
		return 0;
	}
	if (intr == 0) {
		mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, mask);
		return 0;
	}
	sc->sc_intr_count++;

	/* TODO power management */
//	mt76_connac_pm_ref(&dev->mphy, &dev->pm);

	if (intr_log_count < 16) {
		MWX_DEV_LOG(sc, "interrupt status=0x%08x mask=0x%08x\n",
		    intr, mask);
		intr_log_count++;
	}

	if (intr & ~mask && intr_unhandled_log_count < 16) {
		printf("%s: unhandled interrupt %08x\n", DEVNAME(sc),
		    intr & ~mask);
		intr_unhandled_log_count++;
	}
	/* Re-read status after processing so events that arrived while
	 * INT_ENA was 0 are not left waiting for the next interrupt. */
	for (int loops = 0; ; loops++) {
	/* ack interrupts */
	intr &= mask;
	mwx_write(sc, MT_WFDMA0_HOST_INT_STA, intr);

	if (intr & MT_INT_MCU_CMD) {
		intr_sw = mwx_read(sc, MT_MCU_CMD);
		sc->sc_mcu_cmd_irqs++;
		sc->sc_last_mcu_cmd = intr_sw;
		if (intr_log_count < 16 || (intr_sw & ~MT_MCU_CMD_WAKE_RX_PCIE))
			MWX_DEV_LOG(sc, "mcu interrupt status=0x%08x\n", intr_sw);
		/* ack MCU2HOST_SW_INT_STA */
		mwx_write(sc, MT_MCU_CMD, intr_sw);
		if (intr_sw & MT_MCU_CMD_ERROR_MASK)
			mwx_dump_dma_state(sc, "firmware error");
		if (intr_sw & MT_MCU_CMD_WAKE_RX_PCIE)
			intr |= MT_INT_RX_DONE_DATA;
	}

	if (intr & MT_INT_TX_DONE_ALL)
		mwx_dma_tx_done(sc);

	if (intr & MT_INT_RX_DONE_WM)
		mwx_dma_rx_done(sc, &sc->sc_rxfwdlq);
	if (intr & MT_INT_RX_DONE_WM2)
		mwx_dma_rx_done(sc, &sc->sc_rxmcuq);
	if (intr & MT_INT_RX_DONE_DATA)
		mwx_dma_rx_done(sc, &sc->sc_rxq);

	intr = mwx_read(sc, MT_WFDMA0_HOST_INT_STA);
	if ((intr & mask) == 0 || loops >= 8)
		break;
	}

	/* TODO power management */
//	mt76_connac_pm_unref(&dev->mphy, &dev->pm);

	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, mask);

	return 1;
}

int
mwx_preinit(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	int rv, i;
	uint8_t chan;

	MWX_DEV_LOG(sc, "preinit start fw_loaded=%d\n", sc->sc_fw_loaded);

	if ((rv = mwx_init_hardware(sc)) != 0) {
		MWX_DEV_LOG(sc, "preinit hardware init failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "preinit hardware init ok\n");

	/* Power management is not implemented (no pm_ref before MCU
	 * commands), so keep the chip awake. */
	MWX_DEV_LOG(sc, "preinit deep sleep left disabled\n");

	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;

	/* HT MCS 0-7 and VHT MCS 0-9 for each spatial stream. */
	memset(ic->ic_sup_mcs, 0, sizeof(ic->ic_sup_mcs));
	ic->ic_vht_rx_mcs_map = 0xffff;
	for (i = 0; i < MAX(1, MIN(sc->sc_capa.num_streams, 2)); i++) {
		ic->ic_sup_mcs[i] = 0xff;
		ic->ic_vht_rx_mcs_map &= ~(0x3 << (2 * i));
		ic->ic_vht_rx_mcs_map |=
		    IEEE80211_VHT_MCS_SUPPORT_0_9 << (2 * i);
	}
	ic->ic_vht_tx_mcs_map = ic->ic_vht_rx_mcs_map;
	ic->ic_vht_rx_highest = ic->ic_vht_tx_highest = 0;
	ic->ic_tx_mcs_set = IEEE80211_TX_MCS_SET_DEFINED;
	mwx_setup_he_caps(sc);
	MWX_DEV_LOG(sc, "preinit capability 2ghz=%d 5ghz=%d 6ghz=%d streams=%u antenna_mask=0x%x\n",
	    sc->sc_capa.has_2ghz, sc->sc_capa.has_5ghz,
	    sc->sc_capa.has_6ghz, sc->sc_capa.num_streams,
	    sc->sc_capa.antenna_mask);

	if (sc->sc_capa.has_2ghz) {
		for (i = 0; i < MWX_NUM_2GHZ_CHANNELS; i++) {
			chan = mwx_channels_2ghz[i];
			ic->ic_channels[chan].ic_freq =
			    ieee80211_ieee2mhz(chan, IEEE80211_CHAN_2GHZ);
			ic->ic_channels[chan].ic_flags =
			    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
			    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
			ic->ic_channels[chan].ic_flags |= IEEE80211_CHAN_HT;
		}

	}
	if (sc->sc_capa.has_5ghz) {
		ic->ic_sup_rates[IEEE80211_MODE_11A] =
		    ieee80211_std_rateset_11a;
		/* set supported .11a channels */
		for (i = 0; i < MWX_NUM_5GHZ_CHANNELS; i++) {
			chan = mwx_channels_5ghz[i];
			ic->ic_channels[chan].ic_freq =
			    ieee80211_ieee2mhz(chan, IEEE80211_CHAN_5GHZ);
			ic->ic_channels[chan].ic_flags = IEEE80211_CHAN_A;
			ic->ic_channels[chan].ic_flags |= IEEE80211_CHAN_HT |
			    IEEE80211_CHAN_VHT20 | IEEE80211_CHAN_VHT40 |
			    IEEE80211_CHAN_VHT80;
			/* MtkWireless: UNII-2/2e need radar detection: passive
			 * scan only, reported as DFS to airportd, never AWDL. */
			if (mwx_chan_is_dfs(chan))
				ic->ic_channels[chan].ic_flags |=
				    IEEE80211_CHAN_DFS | IEEE80211_CHAN_PASSIVE;
		}
	}
#ifdef NOTYET
	/* TODO support for 6GHz */
	if (sc->sc_capa.has_6ghz) {
		for (i = 0; i < MWX_NUM_6GHZ_CHANNELS; i++) {
		}
	}
#endif

	/* Configure channel information obtained from firmware. */
	ieee80211_channel_init(ifp);
	MWX_DEV_LOG(sc, "preinit channel table ready\n");

	if (IEEE80211_ADDR_EQ(etheranyaddr, sc->sc_ic.ic_myaddr))
		IEEE80211_ADDR_COPY(ic->ic_myaddr, sc->sc_lladdr);

	/* Configure MAC address. */
	rv = if_setlladdr(ifp, ic->ic_myaddr);
	if (rv)
		printf("%s: could not set MAC address (error %d)\n",
		    DEVNAME(sc), rv);
	else
		MWX_DEV_LOG(sc, "preinit mac address=%s\n",
		    ether_sprintf(ic->ic_myaddr));

	ieee80211_media_init(ifp);

	sc->sc_fw_loaded = 1;
	MWX_DEV_LOG(sc, "preinit complete\n");
	return 0;
}

void
mwx_attach_hook(struct device *self)
{
	struct mwx_softc *sc = (struct mwx_softc *)self;

	MWX_DEV_LOG(sc, "attach hook start\n");
	mwx_preinit(sc);
	MWX_DEV_LOG(sc, "attach hook done fw_loaded=%d\n", sc->sc_fw_loaded);
}

bool
mwx_attach(struct device *parent, struct device *self, void *aux)
{
	struct mwx_softc *sc = (struct mwx_softc *)self;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct pci_attach_args *pa = (struct pci_attach_args *)aux;
	pci_intr_handle_t ih;
	pcireg_t memtype;
	uint32_t hwid, hwrev;
	int error;

	MWX_DEV_LOG(sc, "attach start pci_id=0x%08x vendor=0x%04x product=0x%04x\n",
	    pa->pa_id, PCI_VENDOR(pa->pa_id), PCI_PRODUCT(pa->pa_id));
	if (mwx_txq_lock == NULL)
		mwx_txq_lock = IOLockAlloc();
	if (mwx_txq_lock == NULL) {
		printf("%s: can't allocate TX queue lock\n", DEVNAME(sc));
		return false;
	}
    sc->sc_awdl_lock = IOLockAlloc();
    if (!sc->sc_awdl_lock) return false;
	sc->sc_pc = pa->pa_pc;
	sc->sc_tag = pa->pa_tag;
	sc->sc_dmat = pa->pa_dmat;
	sc->sc_intr_mask = MT_INT_RX_DONE_ALL | MT_INT_TX_DONE_ALL |
	    MT_INT_MCU_CMD;
	if (PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_MEDIATEK_MT7922 ||
	    PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_MEDIATEK_RZ616)
		sc->sc_hwtype = MWX_HW_MT7922;
	else
		sc->sc_hwtype = MWX_HW_MT7921;
	MWX_DEV_LOG(sc, "attach hardware type=%s\n",
	    sc->sc_hwtype == MWX_HW_MT7922 ? "MT7922" : "MT7921");

	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_MAPREG_START);
	if (pci_mapreg_map(pa, PCI_MAPREG_START, memtype, 0,
	    &sc->sc_st, &sc->sc_memh, NULL, &sc->sc_mems, 0)) {
		printf("%s: can't map mem space\n", DEVNAME(sc));
		return false;
	}
	MWX_DEV_LOG(sc, "attach BAR mapped memtype=0x%x size=%zu\n",
	    memtype, (size_t)sc->sc_mems);

	if (pci_intr_map_msix(pa, 0, &ih) &&
	    pci_intr_map_msi(pa, &ih) &&
	    pci_intr_map(pa, &ih)) {
		printf("%s: can't map interrupt\n", DEVNAME(sc));
		/* HAL detach owns the mapping on failure. */
		return false;
	}
	MWX_DEV_LOG(sc, "attach interrupt mapped source=%s\n",
	    pci_intr_string(pa->pa_pc, ih));

	if (mwx_mcu_fw_pmctrl(sc) != 0 ||
	    mwx_mcu_drv_pmctrl(sc) != 0) {
		MWX_DEV_LOG(sc, "attach MCU power control failed\n");
		goto fail;
	}
	MWX_DEV_LOG(sc, "attach MCU power control ok\n");

	hwid = mwx_read(sc, MT_HW_CHIPID) & 0xffff;
	hwrev = mwx_read(sc, MT_HW_REV) & 0xff;

	MWX_DEV_LOG(sc, "attach chip id=0x%04x hwrev=0x%02x interrupt=%s\n",
	    hwid, hwrev, pci_intr_string(pa->pa_pc, ih));

	mwx_set(sc, MT_MCU2HOST_SW_INT_ENA, MT_MCU_CMD_WAKE_RX_PCIE);
	if (mwx_wfsys_reset(sc) != 0) {
		MWX_DEV_LOG(sc, "attach WFSYS reset failed\n");
		goto fail;
	}
	MWX_DEV_LOG(sc, "attach WFSYS reset ok\n");
	/*
	 * A warm reboot keeps the card powered, so the WM firmware from the
	 * previous OS session may still run and answers stale MCU state.
	 * Drop it and download a fresh copy.
	 */
	sc->sc_fw_restart = 1;

	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	mwx_write(sc, MT_PCIE_MAC_INT_ENABLE, 0xff);

	sc->sc_ih = pci_intr_establish(pa->pa_pc, ih, IPL_NET,
	    mwx_intr, sc, DEVNAME(sc));
	if (sc->sc_ih == NULL) {
		printf("%s: can't establish interrupt\n", DEVNAME(sc));
		goto fail;
	}
	MWX_DEV_LOG(sc, "attach interrupt established handle=%p\n",
	    (void *)sc->sc_ih);

	if ((error = mwx_txwi_alloc(sc, MWX_TXWI_MAX)) != 0) {
		printf("%s: failed to allocate DMA resources %d\n",
		    DEVNAME(sc), error);
		goto fail;
	}

	if ((error = mwx_dma_alloc(sc)) != 0) {
		printf("%s: failed to allocate DMA resources %d\n",
		    DEVNAME(sc), error);
		goto fail;
	}
	MWX_DEV_LOG(sc, "attach DMA resources ready\n");

	/* set regulatory domain to '00' */
	sc->sc_alpha2[0] = '0';
	sc->sc_alpha2[1] = '0';

	ic->ic_phytype = IEEE80211_T_OFDM;	/* not only, but not used */
	ic->ic_opmode = IEEE80211_M_STA;	/* default to BSS mode */
	ic->ic_state = IEEE80211_S_INIT;

	/* Set device capabilities. */
	ic->ic_caps =
	    IEEE80211_C_QOS |		/* WMM, required for HT/VHT */
	    IEEE80211_C_TX_AMPDU |	/* A-MPDU, net80211 sends ADDBA */
#if NOTYET
	    IEEE80211_C_ADDBA_OFFLOAD | /* device sends ADDBA/DELBA frames */
#endif
	    IEEE80211_C_WEP |		/* WEP */
	    IEEE80211_C_RSN |		/* WPA/RSN */
	    IEEE80211_C_SCANALL |	/* device scans all channels at once */
	    IEEE80211_C_SCANALLBAND |	/* device scans all bands at once */
	    IEEE80211_C_MONITOR |	/* monitor mode supported */
#ifndef IEEE80211_STA_ONLY
	    IEEE80211_C_IBSS |		/* IBSS mode supported */
	    IEEE80211_C_HOSTAP |	/* HostAP mode supported */
	    IEEE80211_C_APPMGT |	/* HostAP power management */
#endif
	    IEEE80211_C_SHSLOT |	/* short slot time supported */
	    IEEE80211_C_SHPREAMBLE;	/* short preamble supported */

	/*
	 * MtkWireless: 802.11w PMF, required by WPA3-SAE. The MAC encrypts
	 * and decrypts unicast robust management frames with the pairwise
	 * key; the IGTK goes to the firmware as mt76 sends it and to
	 * net80211's software BIP, which checks broadcast Deauth/Disassoc.
	 * Boot-arg amtk_pmf=0 turns PMF (and with it SAE) off.
	 */
	{
		int pmf = 1;

		PE_parse_boot_argn("amtk_pmf", &pmf, sizeof(pmf));
		if (pmf != 0)
			ic->ic_caps |= IEEE80211_C_MFP;
	}

	/*
	 * HT/VHT receive capabilities. A-MSDU stays at the 3839/3895 byte
	 * minimum and is not offered inside A-MPDUs because the RX path does
	 * not handle hardware A-MSDU de-aggregation yet. The MCS sets are
	 * filled in by mwx_preinit() once the stream count is known.
	 */
	ic->ic_htcaps = IEEE80211_HTCAP_SGI20 | IEEE80211_HTCAP_SGI40;
	ic->ic_htcaps |= IEEE80211_HTCAP_CBW20_40;
	ic->ic_htcaps |=
	    (IEEE80211_HTCAP_SMPS_DIS << IEEE80211_HTCAP_SMPS_SHIFT);
	ic->ic_htxcaps = 0;
	ic->ic_txbfcaps = 0;
	ic->ic_aselcaps = 0;
	ic->ic_ampdu_params = (IEEE80211_AMPDU_PARAM_SS_4 | 0x3 /* 64k */);

	ic->ic_vhtcaps = IEEE80211_VHTCAP_MAX_MPDU_LENGTH_3895 |
	    IEEE80211_VHTCAP_SHORT_GI_80 |
	    (3 << IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT);

	/* IBSS channel undefined for now. */
	ic->ic_ibss_chan = &ic->ic_channels[1];

	/* HW supports up to 288 STAs in HostAP and IBSS modes */
	ic->ic_max_aid = min(IEEE80211_AID_MAX, MWX_WTBL_STA - 1);

	ic->ic_max_rssi = 100;	/* rxi_rssi is normalized for ni_rssi. */

	ifp->if_softc = sc;
	/* IFF_DEBUG enables net80211 state/EAPOL messages in the debug log. */
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST | IFF_DEBUG;
	ifp->if_ioctl = mwx_ioctl;
	ifp->if_start = mwx_start;
	ifp->if_watchdog = mwx_watchdog;
	memcpy(ifp->if_xname, DEVNAME(sc), IFNAMSIZ);

	if_attach(ifp);
	ieee80211_ifattach(ifp, gMwxHal->mwxController());
	sc->sc_net_attached = true;
	ieee80211_media_init(ifp);
	MWX_DEV_LOG(sc, "attach net80211 attached caps=0x%x max_aid=%u\n",
	    ic->ic_caps, ic->ic_max_aid);

#if NBPFILTER > 0
	mwx_radiotap_attach(sc);
#endif

	ic->ic_node_alloc = mwx_node_alloc;
	ic->ic_newassoc = mwx_newassoc;
#ifndef IEEE80211_STA_ONLY
	ic->ic_node_leave = mwx_node_leave;
#endif
	ic->ic_bgscan_start = mwx_bgscan;
	ic->ic_bgscan_done = mwx_bgscan_done;
	ic->ic_set_key = mwx_set_key;
	ic->ic_delete_key = mwx_delete_key;
	ic->ic_ampdu_rx_start = mwx_ampdu_rx_start;
	ic->ic_ampdu_rx_stop = mwx_ampdu_rx_stop;
	ic->ic_ampdu_tx_start = mwx_ampdu_tx_start;
	ic->ic_ampdu_tx_stop = mwx_ampdu_tx_stop;

	/* Override 802.11 state transition machine. */
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = mwx_newstate;

	sc->sc_nswq = taskq_create("mwxns", 1, IPL_NET, 0);
	if (sc->sc_nswq == NULL) {
		printf("%s: can't create task queue\n", DEVNAME(sc));
		goto fail;
	}

	task_set(&sc->sc_newstate_task, mwx_newstate_task, sc, "mwx_newstate");
	task_set(&sc->sc_bgscan_done_task, mwx_bgscan_done_task, sc, "mwx_bgscan");
	task_set(&sc->sc_setkey_task, mwx_setkey_task, sc, "mwx_setkey");
	task_set(&sc->sc_reset_task, mwx_reset_task, sc, "mwx_reset");
	task_set(&sc->sc_power_task, mwx_power_task, sc, "mwx_power");
	task_set(&sc->sc_scan_task, mwx_end_scan_task, sc, "mwx_scan");
	timeout_set(&sc->sc_reset_to, mwx_reset_timeo, sc);
	timeout_set(&sc->sc_scan_to, mwx_scan_timeo, sc);
	timeout_set(&sc->sc_dma_to, mwx_dma_timeo, sc);
	sc->sc_knobs = MWX_KNOB_DATA_RA;
	sc->sc_bgscan_dwell = MWX_BGSCAN_DWELL_AUTO;
	task_set(&sc->sc_fullreset_task, mwx_fullreset_task, sc, "mwx_fullreset");
	task_set(&sc->sc_ba_task, mwx_ba_task, sc, "mwx_ba");

	/*
	 * We cannot read the MAC address without loading the
	 * firmware from disk. Postpone until mountroot is done.
	 */
	config_mountroot(self, mwx_attach_hook);

	MWX_DEV_LOG(sc, "attach complete\n");
	return true;

fail:
	/* The HAL caller owns partial-attach cleanup through detach(). */
	return false;
}

int
mwx_activate(struct device *self, int act)
{
	/* XXX TODO */
	return 0;
}

#if 0
/* OpenBSD autoconf metadata is replaced by SimpleMtkWlan's IOKit matching path. */
struct cfdriver mwx_cd = {
	NULL, "mwx", DV_IFNET
};

struct cfattach mwx_ca = {
	sizeof(struct mwx_softc), mwx_match, mwx_attach,
	NULL, mwx_activate
};
#endif

void
mwx_reset(struct mwx_softc *sc)
{
	if (sc->sc_resetting || sc->sc_detaching)
		return;
	sc->sc_resetting = 1;
	task_add(sc->sc_nswq, &sc->sc_reset_task);
}

void
mwx_reset_timeo(void *arg)
{
	mwx_reset((struct mwx_softc *)arg);
}

void
mwx_reset_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	int fatal = 0;

	if (ifp->if_flags & IFF_RUNNING)
		mwx_stop(ifp);

	if (!fatal && (ifp->if_flags & (IFF_UP | IFF_RUNNING)) == IFF_UP)
		mwx_init(ifp);
	sc->sc_resetting = 0;
	sc->sc_coredump_cnt = 0;
}

/*
 * IO80211 calls enable/disable (power, MAC address changes) with the
 * workloop gate held. MCU replies are delivered by the interrupt event
 * source on that same workloop, so stopping or initialising the firmware
 * there times out every command. Bring the interface to the state
 * IFF_UP asks for from the task queue instead, like the iwx HAL does.
 */
void
mwx_power_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	SInt32 req;
	int rv;

	if (sc->sc_detaching)
		return;
	/*
	 * A disable+enable pair (private MAC change) can be split across two
	 * task runs when the task starts between them. The first run already
	 * acts on the final IFF_UP, so the second must not stop and init the
	 * firmware again right away: that hangs the MCU (cmd 0x46ed timeout).
	 */
	if (sc->sc_power_req == sc->sc_power_done) {
		MWX_DEV_LOG(sc, "power task: requests already served flags=0x%x\n",
		    ifp->if_flags);
		return;
	}
	if (ifp->if_flags & IFF_RUNNING)
		mwx_stop(ifp);
	/* Requests counted here are served by the IFF_UP read below. */
	req = sc->sc_power_req;
	OSSynchronizeIO();
	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) != IFF_UP) {
		sc->sc_power_result = 0;
		OSSynchronizeIO();
		sc->sc_power_done = req;
		return;
	}
	rv = mwx_init(ifp);
	if (rv != 0 && !sc->sc_detaching && (ifp->if_flags & IFF_UP)) {
		/* One bounded firmware reload for this enable request. A failed
		 * wake must not leave subsequent enables using dead MCU state.
		 */
		MWX_DEV_LOG(sc, "power task restart failed=%d; reload firmware\n", rv);
		task_del(sc->sc_nswq, &sc->sc_reset_task);
		timeout_del(&sc->sc_reset_to);
		mwx_fullreset_task(sc);
		rv = (ifp->if_flags & IFF_RUNNING) ? 0 : EIO;
	}
	sc->sc_power_result = rv;
	MWX_DEV_LOG(sc, "power task init=%d flags=0x%x\n", rv, ifp->if_flags);
	if (rv != 0)
		ifp->if_flags &= ~IFF_UP;
	OSSynchronizeIO();
	sc->sc_power_done = req;
}

int
mwx_txwi_alloc(struct mwx_softc *sc, int count)
{
	int error, nsegs, i;
	struct mwx_txwi_desc *q = &sc->sc_txwi;
	bus_size_t size = count * sizeof(*q->mt_desc);
	uint32_t addr;

	MWX_DEV_LOG(sc, "txwi alloc start count=%d size=%zu\n",
	    count, (size_t)size);
	LIST_INIT(&q->mt_freelist);
	q->mt_count = count;
	q->mt_inuse = 0;
	q->mt_peak = 0;

	error = bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
	    BUS_DMA_NOWAIT, &q->mt_map);
	if (error != 0) {
		printf("%s: could not create desc TWXI map\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_alloc(sc->sc_dmat, size, PAGE_SIZE, 0, &q->mt_seg,
	    1, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO);
	if (error != 0) {
		printf("%s: could not allocate TWXI memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_map(sc->sc_dmat, &q->mt_seg, nsegs, size,
	    (void **)&q->mt_desc, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: can't map desc DMA memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamap_load(sc->sc_dmat, q->mt_map, q->mt_desc,
	    size, NULL, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: could not load desc DMA map\n", DEVNAME(sc));
		goto fail;
	}

	addr = q->mt_map->dm_segs[0].ds_addr;

	q->mt_data = (struct mwx_txwi *)mallocarray(count, sizeof(*q->mt_data),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (q->mt_data == NULL) {
		printf("%s: could not allocate soft data\n", DEVNAME(sc));
		error = ENOMEM;
		goto fail;
	}

	for (i = 0; i < count; i++) {
		q->mt_data[i].mt_idx = i;
		q->mt_data[i].mt_desc = &q->mt_desc[i];
		q->mt_data[i].mt_addr = addr + i * sizeof(*q->mt_desc);
		error = bus_dmamap_create(sc->sc_dmat, MT_TXD_LEN_MASK,
		    MT_MAX_SCATTER, MT_TXD_LEN_MASK, 0, BUS_DMA_NOWAIT,
		    &q->mt_data[i].mt_map);
		if (error != 0) {
			printf("%s: could not create data DMA map\n",
			    DEVNAME(sc));
			goto fail;
		}
	}

	for (i = count - 1; i >= MT_PACKET_ID_FIRST; i--)
		LIST_INSERT_HEAD(&q->mt_freelist, &q->mt_data[i], mt_entry);

	MWX_DEV_LOG(sc, "txwi alloc ok count=%d dma=0x%08x\n", count, addr);
	return 0;

fail:
	MWX_DEV_LOG(sc, "txwi alloc failed error=%d\n", error);
	mwx_txwi_free(sc);
	return error;
}

void
mwx_txwi_free(struct mwx_softc *sc)
{
	struct mwx_txwi_desc *q = &sc->sc_txwi;

	if (q->mt_data != NULL) {
		int i;
		for (i = 0; i < q->mt_count; i++) {
			struct mwx_txwi *mt = &q->mt_data[i];
			bus_dmamap_destroy(sc->sc_dmat, mt->mt_map);
			m_freem(mt->mt_mbuf);
			if (i >= MT_PACKET_ID_FIRST)
				LIST_REMOVE(mt, mt_entry);
		}
		free(q->mt_data, M_DEVBUF, q->mt_count * sizeof(*q->mt_data));
	}

	if (q->mt_desc != NULL) {
		bus_dmamap_sync(sc->sc_dmat, q->mt_map, 0,
			q->mt_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, q->mt_map);
	}

	/*
	 * XXX TODO this is probably not correct as a check, should use
	 * some state variable bitfield to decide which steps need to be run.
	 */
	if (q->mt_seg.ds_len != 0)
		bus_dmamem_unmap(sc->sc_dmat, (caddr_t)q->mt_desc,
			q->mt_count * sizeof(*q->mt_desc));
	if (q->mt_map != NULL)
		bus_dmamem_free(sc->sc_dmat, &q->mt_seg, 1);

	memset(q, 0, sizeof(*q));
}

struct mwx_txwi *
mwx_txwi_get(struct mwx_softc *sc)
{
	struct mwx_txwi *mt;

	mt = LIST_FIRST(&sc->sc_txwi.mt_freelist);
	if (mt == NULL)
		return NULL;
	LIST_REMOVE(mt, mt_entry);
	mt->mt_busy = 1;
	if (++sc->sc_txwi.mt_inuse > sc->sc_txwi.mt_peak)
		sc->sc_txwi.mt_peak = sc->sc_txwi.mt_inuse;
	return mt;
}

void
mwx_txwi_put(struct mwx_softc *sc, struct mwx_txwi *mt)
{
	if (mt->mt_busy == 0)
		return;

	if (mt->mt_mbuf != NULL) {
		bus_dmamap_sync(sc->sc_dmat, mt->mt_map, 0,
		    mt->mt_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, mt->mt_map);
		m_freem(mt->mt_mbuf);
		mt->mt_mbuf = NULL;
	}

	memset(mt->mt_desc, 0, sizeof(*mt->mt_desc));
	mt->mt_busy = 0;

	if (mt->mt_idx < MT_PACKET_ID_FIRST)
		return;
	LIST_INSERT_HEAD(&sc->sc_txwi.mt_freelist, mt, mt_entry);
	sc->sc_txwi.mt_inuse--;
}

int
mwx_txwi_enqueue(struct mwx_softc *sc, struct mwx_txwi *mt, struct mbuf *m)
{
	struct mwx_txwi_desc *q = &sc->sc_txwi;
	struct mt76_txwi *txp = mt->mt_desc;
	struct mt76_connac_txp_ptr *ptr = &txp->ptr[0];
	uint32_t addr;
	uint16_t len;
	int i, nsegs, rv;

	rv = bus_dmamap_load_mbuf(sc->sc_dmat, mt->mt_map, m,
	    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
	if (rv != 0)
		return EFBIG;

	nsegs = mt->mt_map->dm_nsegs;
	/* The TXP only has MT_HW_TXP_MAX_BUF_NUM buffer slots. */
	if (nsegs > MT_HW_TXP_MAX_BUF_NUM) {
		bus_dmamap_unload(sc->sc_dmat, mt->mt_map);
		return EFBIG;
	}

	bus_dmamap_sync(sc->sc_dmat, mt->mt_map, 0, mt->mt_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	txp->msdu_id[0] = htole16(mt->mt_idx | MT_MSDU_ID_VALID);
	mt->mt_mbuf = m;

	bus_dmamap_sync(sc->sc_dmat, q->mt_map, 0, q->mt_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);
	for (i = 0; i < nsegs; i++) {
		KASSERT(mt->mt_map->dm_segs[i].ds_addr <= UINT32_MAX);
		KASSERT(mt->mt_map->dm_segs[i].ds_len <= MT_TXD_LEN_MASK);
		addr = mt->mt_map->dm_segs[i].ds_addr;
		len = mt->mt_map->dm_segs[i].ds_len;

		if (i == nsegs - 1)
			len |= MT_TXD_LEN_LAST;

		if ((i & 1) == 0) {
			ptr->buf0 = htole32(addr);
			ptr->len0 = htole16(len);
		} else {
			ptr->buf1 = htole32(addr);
			ptr->len1 = htole16(len);
			ptr++;
		}
	}
	bus_dmamap_sync(sc->sc_dmat, q->mt_map, 0, q->mt_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	return 0;
}

int
mwx_queue_alloc(struct mwx_softc *sc, struct mwx_queue *q, int count,
    uint32_t regbase)
{
	int error, nsegs, i;
	bus_size_t size = count * sizeof(*q->mq_desc);

	MWX_DEV_LOG(sc, "queue alloc start regbase=0x%08x count=%d size=%zu\n",
	    regbase, count, (size_t)size);
	q->mq_regbase = regbase;
	q->mq_count = count;

	error = bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
	    BUS_DMA_NOWAIT, &q->mq_map);
	if (error != 0) {
		printf("%s: could not create desc DMA map\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_alloc(sc->sc_dmat, size, PAGE_SIZE, 0, &q->mq_seg,
	    1, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO);
	if (error != 0) {
		printf("%s: could not allocate DMA memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamem_map(sc->sc_dmat, &q->mq_seg, nsegs, size,
	    (void **)&q->mq_desc, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: can't map desc DMA memory\n", DEVNAME(sc));
		goto fail;
	}

	error = bus_dmamap_load(sc->sc_dmat, q->mq_map, q->mq_desc,
	    size, NULL, BUS_DMA_NOWAIT);
	if (error != 0) {
		printf("%s: could not load desc DMA map\n", DEVNAME(sc));
		goto fail;
	}

	q->mq_data = (struct mwx_queue_data *)mallocarray(count, sizeof(*q->mq_data),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (q->mq_data == NULL) {
		printf("%s: could not allocate soft data\n", DEVNAME(sc));
		error = ENOMEM;
		goto fail;
	}

	for (i = 0; i < count; i++) {
		error = bus_dmamap_create(sc->sc_dmat, MT_MAX_SIZE,
		    MT_MAX_SCATTER, MT_MAX_SIZE, 0, BUS_DMA_NOWAIT,
		    &q->mq_data[i].md_map);
		if (error != 0) {
			printf("%s: could not create data DMA map\n",
			    DEVNAME(sc));
			goto fail;
		}
	}

	mwx_queue_reset(sc, q);
	MWX_DEV_LOG(sc, "queue alloc ok regbase=0x%08x count=%d dma=0x%llx\n",
	    regbase, count, (unsigned long long)q->mq_map->dm_segs[0].ds_addr);
	return 0;

fail:
	MWX_DEV_LOG(sc, "queue alloc failed regbase=0x%08x error=%d\n",
	    regbase, error);
	mwx_queue_free(sc, q);
	return error;
}

void
mwx_queue_free(struct mwx_softc *sc, struct mwx_queue *q)
{
	if (q->mq_data != NULL) {
		int i;
		for (i = 0; i < q->mq_count; i++) {
			struct mwx_queue_data  *md = &q->mq_data[i];
			bus_dmamap_destroy(sc->sc_dmat, md->md_map);
			m_freem(md->md_mbuf);
		}
		free(q->mq_data, M_DEVBUF, q->mq_count * sizeof(*q->mq_data));
	}

	if (q->mq_desc != NULL) {
		bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0,
			q->mq_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, q->mq_map);
	}

	/*
	 * XXX TODO this is probably not correct as a check, should use
	 * some state variable bitfield to decide which steps need to be run.
	 */
	if (q->mq_seg.ds_len != 0)
		bus_dmamem_unmap(sc->sc_dmat, (caddr_t)q->mq_desc,
			q->mq_count * sizeof(*q->mq_desc));
	if (q->mq_map != NULL)
		bus_dmamem_free(sc->sc_dmat, &q->mq_seg, 1);

	memset(q, 0, sizeof(*q));
}

void
mwx_queue_reset(struct mwx_softc *sc, struct mwx_queue *q)
{
	int i;
	uint32_t dmaaddr;
	struct mwx_queue_data *md;

	IOLockLock(mwx_txq_lock);

	/* clear descriptors */
	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	for (i = 0; i < q->mq_count; i++) {
		q->mq_desc[i].buf0 = 0;
		q->mq_desc[i].buf1 = 0;
		q->mq_desc[i].info = 0;
		q->mq_desc[i].ctrl = htole32(MT_DMA_CTL_DMA_DONE);
	}

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	/* reset DMA registers */
	KASSERT(q->mq_map->dm_nsegs == 1);
	KASSERT(q->mq_map->dm_segs[0].ds_addr <= UINT32_MAX);
	dmaaddr	= q->mq_map->dm_segs[0].ds_addr;
	mwx_write(sc, q->mq_regbase + MT_DMA_DESC_BASE, dmaaddr);
	mwx_write(sc, q->mq_regbase + MT_DMA_RING_SIZE, q->mq_count);
	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, 0);
	mwx_write(sc, q->mq_regbase + MT_DMA_DMA_IDX, 0);
	q->mq_cons = 0;
	q->mq_prod = 0;

	/* free buffers */
	for (i = 0; i < q->mq_count; i++) {
		md = &q->mq_data[i];
		md->md_txwi = NULL;
		if (md->md_mbuf != NULL) {
			bus_dmamap_sync(sc->sc_dmat, md->md_map, 0,
			    md->md_map->dm_mapsize,
			    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, md->md_map);
			m_freem(md->md_mbuf);
			md->md_mbuf = NULL;
		}
	}

	IOLockUnlock(mwx_txq_lock);
}

int
mwx_buf_fill(struct mwx_softc *sc, struct mwx_queue_data *md,
    struct mwx_desc *desc)
{
	struct mbuf *m;
	uint32_t buf0, len0, ctrl;
	int rv;

	m = MCLGETL(NULL, M_DONTWAIT, MT_RX_BUF_SIZE);
	if (m == NULL)
		return (ENOMEM);

	mbuf_setlen(m, MT_RX_BUF_SIZE);
	mbuf_pkthdr_setlen(m, MT_RX_BUF_SIZE);

	rv = bus_dmamap_load_mbuf(sc->sc_dmat, md->md_map, m,
	     BUS_DMA_READ | BUS_DMA_NOWAIT);
	if (rv != 0) {
		printf("%s: could not load data, %d\n", DEVNAME(sc), rv);
		m_freem(m);
		return (rv);
	}

	bus_dmamap_sync(sc->sc_dmat, md->md_map, 0,
	    md->md_map->dm_mapsize, BUS_DMASYNC_PREREAD);

	md->md_mbuf = m;

	KASSERT(md->md_map->dm_nsegs == 1);
	KASSERT(md->md_map->dm_segs[0].ds_addr <= UINT32_MAX);
	buf0 = md->md_map->dm_segs[0].ds_addr;
	len0 = md->md_map->dm_segs[0].ds_len;
	ctrl = MT_DMA_CTL_SD_LEN0(len0);
	ctrl |= MT_DMA_CTL_LAST_SEC0;

	desc->buf0 = htole32(buf0);
	desc->buf1 = 0;
	desc->info = 0;
	desc->ctrl = htole32(ctrl);

	return 0;
}

int
mwx_queue_fill(struct mwx_softc *sc, struct mwx_queue *q)
{
	u_int idx, last;
	int rv;

	last = (q->mq_count + q->mq_cons - 1) % q->mq_count;
	idx = q->mq_prod;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	while (idx != last) {
		rv = mwx_buf_fill(sc, &q->mq_data[idx], &q->mq_desc[idx]);
		if (rv != 0) {
			printf("%s: could not fill data, slot %d err %d\n",
			    DEVNAME(sc), idx, rv);
			return rv;
		}
		idx = (idx + 1) % q->mq_count;
	}

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_prod = idx;
	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, q->mq_prod);

	return 0;
}

int
mwx_dma_alloc(struct mwx_softc *sc)
{
	int rv;

	MWX_DEV_LOG(sc, "dma alloc start\n");
	/* TX queues */
	if ((rv = mwx_queue_alloc(sc, &sc->sc_txq, 256,
	    MT_TX_DATA_RING_BASE)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_txmcuq, 256,
	    MT_TX_MCU_RING_BASE)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_txfwdlq, 16 /* XXX */,
	    MT_TX_FWDL_RING_BASE)) != 0)
		return rv;

	/* RX queues */
	if ((rv = mwx_queue_alloc(sc, &sc->sc_rxq, 256,
	    MT_RX_DATA_RING_BASE)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxq)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_rxmcuq, 256,
	    MT_RX_MCU_RING_BASE)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxmcuq)) != 0)
		return rv;
	if ((rv = mwx_queue_alloc(sc, &sc->sc_rxfwdlq, 16 /* XXX */,
	    MT_RX_FWDL_RING_BASE)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxfwdlq)) != 0)
		return rv;

	/* enable DMA engine */
	mwx_dma_enable(sc);

	MWX_DEV_LOG(sc, "dma alloc complete\n");
	return 0;
}

int
mwx_dma_reset(struct mwx_softc *sc, int fullreset)
{
	int rv;

	DPRINTF("%s: DMA reset\n", DEVNAME(sc));

	if ((rv = mwx_dma_disable(sc, fullreset)) != 0)
		return rv;
	if (fullreset)
		if ((rv = mwx_wfsys_reset(sc)) != 0)
			return rv;

	/* TX queues */
	mwx_queue_reset(sc, &sc->sc_txq);
	mwx_queue_reset(sc, &sc->sc_txmcuq);
	mwx_queue_reset(sc, &sc->sc_txfwdlq);

	/* RX queues */
	mwx_queue_reset(sc, &sc->sc_rxq);
	mwx_queue_reset(sc, &sc->sc_rxmcuq);
	mwx_queue_reset(sc, &sc->sc_rxfwdlq);

	/* Reset rings no longer refer to old TXP tokens. Reclaim them for
	 * both conditional wake recovery and a complete firmware reload.
	 */
	IOLockLock(mwx_txq_lock);
	for (u_int i = 0; i < sc->sc_txwi.mt_count; i++)
		mwx_txwi_put(sc, &sc->sc_txwi.mt_data[i]);
	IOLockUnlock(mwx_txq_lock);

	/* refill RX queues */
	if ((rv = mwx_queue_fill(sc, &sc->sc_rxq)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxmcuq)) != 0 ||
	    (rv = mwx_queue_fill(sc, &sc->sc_rxfwdlq)) != 0)
		return rv;

	/* enable DMA engine */
	mwx_dma_enable(sc);

	return 0;
}

/* Firmware clears NEED_REINIT when deep sleep loses the WFDMA context.
 * Recover the host rings before sending any wake-up MCU command, as in
 * mt792x_wpdma_reinit_cond. This is only called for a stopped interface;
 * initial attach has not allocated the rings and must not enter here.
 */
static IOReturn
mwx_dma_reinit_gated(OSObject *, void *arg, void *, void *, void *)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	int rv = 0;

	/* Masking hardware alone does not drain an already queued interrupt.
	 * The gate excludes its RX callback while buffers are reclaimed. No
	 * MCU command or response wait is performed with this gate held.
	 */
	if (!(mwx_read(sc, MT_WFDMA_DUMMY_CR) & MT_WFDMA_NEED_REINIT)) {
		MWX_DEV_LOG(sc, "DMA wake recovery start dummy=0x%08x\n",
		    mwx_read(sc, MT_WFDMA_DUMMY_CR));
		mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, 0);
		mwx_write(sc, MT_PCIE_MAC_INT_ENABLE, 0);
		rv = mwx_dma_reset(sc, 0);
		MWX_DEV_LOG(sc, "DMA wake recovery done error=%d\n", rv);
	}
	return rv;
}

static int
mwx_dma_reinit_cond(struct mwx_softc *sc)
{
	pci_intr_handle_t ih = (pci_intr_handle_t)sc->sc_ih;
	if (mwx_read(sc, MT_WFDMA_DUMMY_CR) & MT_WFDMA_NEED_REINIT)
		return 0;
	if (sc->sc_ic.ic_if.if_flags & IFF_RUNNING)
		return EBUSY;
	if (!ih || !ih->workloop || !sc->sc_txq.mq_map ||
	    !sc->sc_txmcuq.mq_map || !sc->sc_txfwdlq.mq_map ||
	    !sc->sc_rxq.mq_map || !sc->sc_rxmcuq.mq_map ||
	    !sc->sc_rxfwdlq.mq_map)
		return ENXIO;
	return ih->workloop->runAction(mwx_dma_reinit_gated, ih, sc);
}

void
mwx_dma_free(struct mwx_softc *sc)
{
	/* TX queues */
	mwx_queue_free(sc, &sc->sc_txq);
	mwx_queue_free(sc, &sc->sc_txmcuq);
	mwx_queue_free(sc, &sc->sc_txfwdlq);

	/* RX queues */
	mwx_queue_free(sc, &sc->sc_rxq);
	mwx_queue_free(sc, &sc->sc_rxmcuq);
	mwx_queue_free(sc, &sc->sc_rxfwdlq);
}

static inline int
mwx_dma_free_slots(struct mwx_queue *q)
{
	int free = q->mq_count - 1;
	free += q->mq_cons;
	free -= q->mq_prod;
	free %= q->mq_count;
	return free;
}

int
mwx_tx_full(struct mwx_softc *sc)
{
	return LIST_EMPTY(&sc->sc_txwi.mt_freelist) ||
	    mwx_dma_free_slots(&sc->sc_txq) < 1;
}

void
mwx_tx_resume(struct mwx_softc *sc)
{
	struct ifnet *ifp = &sc->sc_ic.ic_if;

	if (!ifq_is_oactive(&ifp->if_snd) || mwx_tx_full(sc))
		return;
	ifq_clr_oactive(&ifp->if_snd);
	(*ifp->if_start)(ifp);
#ifdef __PRIVATE_SPI__
	/* outputStart() stopped pulling when it saw oactive. */
	if (ifp->iface != NULL)
		ifp->iface->signalOutputThread();
#endif
}

static int
mwx_dma_tx_enqueue_locked(struct mwx_softc *sc, struct mwx_queue *q,
    struct mbuf *m)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	int i, nsegs, idx, rv;

	idx = q->mq_prod;
	md = &q->mq_data[idx];
	desc = &q->mq_desc[idx];

	rv = bus_dmamap_load_mbuf(sc->sc_dmat, md->md_map, m,
	    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
	if (rv == EFBIG && m_defrag(m, M_DONTWAIT) == 0)
		rv = bus_dmamap_load_mbuf(sc->sc_dmat, md->md_map, m,
		    BUS_DMA_WRITE | BUS_DMA_NOWAIT);
	if (rv != 0)
		return rv;

	nsegs = md->md_map->dm_nsegs;

	/* check if there is enough space */
	if ((nsegs + 1)/2 > mwx_dma_free_slots(q)) {
		bus_dmamap_unload(sc->sc_dmat, md->md_map);
		return EBUSY;
	}

	bus_dmamap_sync(sc->sc_dmat, md->md_map, 0, md->md_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);
	md->md_mbuf = m;
	md->md_txwi = NULL;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);
	for (i = 0; i < nsegs; i += 2) {
		uint32_t	buf0, buf1 = 0;
		uint32_t	len0, len1 = 0, ctrl;

		KASSERT(md->md_map->dm_segs[i].ds_addr <= UINT32_MAX);
		buf0 = md->md_map->dm_segs[i].ds_addr;
		len0 = md->md_map->dm_segs[i].ds_len;
		ctrl = MT_DMA_CTL_SD_LEN0(len0);

		if (i < nsegs - 1) {
			KASSERT(md->md_map->dm_segs[i + 1].ds_addr <=
			    UINT32_MAX);
			buf1 = md->md_map->dm_segs[i + 1].ds_addr;
			len1 = md->md_map->dm_segs[i + 1].ds_len;
			ctrl |= MT_DMA_CTL_SD_LEN1(len1);
		}

		if (i == nsegs - 1)
			ctrl |= MT_DMA_CTL_LAST_SEC0;
		else if (i == nsegs - 2)
			ctrl |= MT_DMA_CTL_LAST_SEC1;

		desc->buf0 = htole32(buf0);
		desc->buf1 = htole32(buf1);
		desc->info = 0;
		desc->ctrl = htole32(ctrl);

		idx = (idx + 1) % q->mq_count;
		KASSERT(idx != q->mq_cons);
		md = &q->mq_data[idx];
		desc = &q->mq_desc[idx];
	}
	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_prod = idx;

	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, q->mq_prod);

	return 0;
}

static int
mwx_dma_txwi_enqueue_locked(struct mwx_softc *sc, struct mwx_queue *q,
    struct mwx_txwi *mt)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	uint32_t buf0, len0, ctrl;
	int idx;

	idx = q->mq_prod;
	md = &q->mq_data[idx];
	desc = &q->mq_desc[idx];

	/* check if there is enough space */
	if (1 > mwx_dma_free_slots(q)) {
		bus_dmamap_unload(sc->sc_dmat, md->md_map);
		return EBUSY;
	}

	md->md_txwi = mt;
	md->md_mbuf = NULL;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	buf0 = mt->mt_addr;
	len0 = sizeof(*mt->mt_desc);
	ctrl = MT_DMA_CTL_SD_LEN0(len0);
	ctrl |= MT_DMA_CTL_LAST_SEC0;

	static uint32_t txwi_dma_log_count;
	if (txwi_dma_log_count < 16) {
		MWX_DEV_LOG(sc, "txwi dma idx=%d addr=0x%08x len=%u\n",
		    idx, buf0, len0);
		txwi_dma_log_count++;
	}

	desc->buf0 = htole32(buf0);
	desc->buf1 = 0;
	desc->info = 0;
	desc->ctrl = htole32(ctrl);

	idx = (idx + 1) % q->mq_count;
	KASSERT(idx != q->mq_cons);

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_prod = idx;

	mwx_write(sc, q->mq_regbase + MT_DMA_CPU_IDX, q->mq_prod);

	return 0;
}

int
mwx_dma_tx_enqueue(struct mwx_softc *sc, struct mwx_queue *q, struct mbuf *m)
{
	int rv;

	IOLockLock(mwx_txq_lock);
	rv = mwx_dma_tx_enqueue_locked(sc, q, m);
	IOLockUnlock(mwx_txq_lock);
	return rv;
}

int
mwx_dma_txwi_enqueue(struct mwx_softc *sc, struct mwx_queue *q,
    struct mwx_txwi *mt)
{
	int rv;

	IOLockLock(mwx_txq_lock);
	rv = mwx_dma_txwi_enqueue_locked(sc, q, mt);
	IOLockUnlock(mwx_txq_lock);
	return rv;
}

static int
mwx_dma_tx_cleanup_locked(struct mwx_softc *sc, struct mwx_queue *q)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	int idx, last;

	uint32_t hw;

	idx = q->mq_cons;
	if (!mtk_wlan_dma_hw_index(mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX),
	    q->mq_count, &hw))
		return 0;
	last = hw;

	if (idx == last)
		return 0;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	     BUS_DMASYNC_PREWRITE);

	while (idx != last) {
		md = &q->mq_data[idx];
		desc = &q->mq_desc[idx];

		if (md->md_mbuf != NULL) {
			bus_dmamap_sync(sc->sc_dmat, md->md_map, 0,
			    md->md_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmat, md->md_map);
			m_freem(md->md_mbuf);
			md->md_mbuf = NULL;
		}
		if (md->md_txwi != NULL) {
			static uint32_t txwi_ack_log_count;

			/* TXWI cleanup is driven by the firmware TX free event. */
			md->md_txwi = NULL;
			if (txwi_ack_log_count < 32) {
				MWX_DEV_LOG(sc, "txwi acked idx=%d\n", idx);
				txwi_ack_log_count++;
			}
		}

		/* clear DMA desc just to be sure */
		desc->buf0 = 0;
		desc->buf1 = 0;
		desc->info = 0;
		desc->ctrl = htole32(MT_DMA_CTL_DMA_DONE);

		idx = (idx + 1) % q->mq_count;

		/* check if more data made it in */
		/* XXX should we actually do that? */
		if (idx == last &&
		    mtk_wlan_dma_hw_index(mwx_read(sc,
		    q->mq_regbase + MT_DMA_DMA_IDX), q->mq_count, &hw))
			last = hw;
	}

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTWRITE);

	q->mq_cons = idx;
	if (q->mq_wakeme) {
		q->mq_wakeme = 0;
		return 1;
	}
	return 0;
}

void
mwx_dma_tx_cleanup(struct mwx_softc *sc, struct mwx_queue *q)
{
	int wake;

	IOLockLock(mwx_txq_lock);
	wake = mwx_dma_tx_cleanup_locked(sc, q);
	IOLockUnlock(mwx_txq_lock);
	if (wake) {
        if(gMwxHal) gMwxHal->mwxWakeup(q); else wakeup(q);
    }
}

void
mwx_dma_tx_done(struct mwx_softc *sc)
{
	mwx_dma_tx_cleanup(sc, &sc->sc_txq);
	mwx_dma_tx_cleanup(sc, &sc->sc_txmcuq);
	mwx_dma_tx_cleanup(sc, &sc->sc_txfwdlq);
	mwx_tx_resume(sc);
}

/* XXX wrong place */
void
mwx_dma_rx_process(struct mwx_softc *sc, struct mbuf_list *ml)
{
	struct mbuf_list mlout = MBUF_LIST_INITIALIZER();
	struct mbuf *m;
	uint32_t *data, rxd, type, flag;
	uint32_t event_count = 0, notify_count = 0, normal_mcu_count = 0;
	uint32_t normal_count = 0, unknown_count = 0;
	uint32_t first_unknown_type = 0, first_unknown_flag = 0, first_unknown_rxd = 0;

	while ((m = ml_dequeue(ml)) != NULL) {
		data = mtod(m, uint32_t *);
		/* MtkWireless: classified in Rust, which also rejects packets
		 * too short to hold the first dword. */
		if (!mtk_wlan_rx_classify(mtod(m, const uint8_t *), mbuf_len(m),
		    &type, &flag, &rxd)) {
			unknown_count++;
			m_freem(m);
			continue;
		}

		switch (type) {
		case PKT_TYPE_RX_EVENT:
			event_count++;
			mwx_mcu_rx_event(sc, m);
			break;
		case PKT_TYPE_TXRX_NOTIFY:
			notify_count++;
			mwx_mac_tx_free(sc, m);
			break;
		case PKT_TYPE_TXS: {
			size_t records = mtk_wlan_txs_records(mbuf_len(m));
			size_t i;

			for (i = 0; i < records; i++)
				mt7921_mac_add_txs(sc, &data[2 + 8 * i]);
			m_freem(m);
			break;
		}
		case PKT_TYPE_NORMAL_MCU:
			normal_mcu_count++;
			mwx_rx(sc, m, &mlout);
			break;
		case PKT_TYPE_NORMAL:
			normal_count++;
			mwx_rx(sc, m, &mlout);
			break;
		default:
			if (unknown_count == 0) {
				first_unknown_type = type;
				first_unknown_flag = flag;
				first_unknown_rxd = rxd;
			}
			unknown_count++;
			m_freem(m);
			break;
		}
	}

	if (event_count || notify_count || normal_mcu_count ||
	    normal_count || unknown_count) {
		sc->sc_rx_pkt_event += event_count;
		sc->sc_rx_pkt_txrx_notify += notify_count;
		sc->sc_rx_pkt_normal_mcu += normal_mcu_count;
		sc->sc_rx_pkt_normal += normal_count;
		sc->sc_rx_pkt_unknown += unknown_count;
		/*
		 * Normal data arrives in nearly every batch (~2000/s under
		 * load), so only log the first batches, the first batches with
		 * unknown packets, and a periodic summary.
		 */
		if (sc->sc_rx_process_events < 64 ||
		    (unknown_count != 0 &&
		    sc->sc_rx_pkt_unknown - unknown_count < 16) ||
		    (sc->sc_rx_process_events & 0x3ffff) == 0) {
			MWX_DEV_LOG(sc,
			    "rx process event=%u notify=%u normal_mcu=%u normal=%u unknown=%u totals e=%u n=%u nmcu=%u norm=%u unk=%u\n",
			    event_count, notify_count, normal_mcu_count,
			    normal_count, unknown_count, sc->sc_rx_pkt_event,
			    sc->sc_rx_pkt_txrx_notify,
			    sc->sc_rx_pkt_normal_mcu, sc->sc_rx_pkt_normal,
			    sc->sc_rx_pkt_unknown);
			if (unknown_count != 0) {
				MWX_DEV_LOG(sc,
				    "rx unknown first type=%u flag=0x%x rxd0=0x%08x\n",
				    first_unknown_type, first_unknown_flag,
				    first_unknown_rxd);
			}
		}
		sc->sc_rx_process_events++;
	}

	if (!ml_empty(&mlout))
		sc->sc_rx_batches++;
	if_input(&sc->sc_ic.ic_if, &mlout);
}

void
mwx_dma_rx_dequeue(struct mwx_softc *sc, struct mwx_queue *q,
    struct mbuf_list *ml)
{
	struct mwx_queue_data *md;
	struct mwx_desc *desc;
	struct mbuf *m, *m0 = NULL, *mtail = NULL;
	int idx, last;
	uint32_t hw;
	bool bad = false;

	idx = q->mq_cons;
	/* MtkWireless: hardware indices and lengths are validated in Rust
	 * (rust/mtk-wlan/src/connac2/dma.rs); 0xffffffff means the device
	 * is gone and must not drive the loop below. */
	if (!mtk_wlan_dma_hw_index(mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX),
	    q->mq_count, &hw)) {
		sc->sc_rx_fill_fail++;
		return;
	}
	last = hw;

	if (idx == last)
		return;

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	     BUS_DMASYNC_PREREAD);

	while (idx != last) {
		uint32_t ctrl, seglen;
		bool seglast;

		md = &q->mq_data[idx];
		desc = &q->mq_desc[idx];

		/* A slot that could not be refilled holds no buffer. */
		if (md->md_mbuf == NULL)
			break;

		/* only buf0 is used on RX rings */
		ctrl = le32toh(desc->ctrl);
		if (!mtk_wlan_dma_rx_segment(ctrl, (uint32_t)md->md_map->dm_mapsize,
		    &seglen, &seglast)) {
			/* longer than the posted buffer: drop the frame */
			bad = true;
			seglen = 0;
			seglast = (ctrl & MT_DMA_CTL_LAST_SEC0) != 0;
		}

		/* only copy back what the device wrote */
		bus_dmamap_sync(sc->sc_dmat, md->md_map, 0, seglen,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->sc_dmat, md->md_map);

		/* dequeue mbuf */
		m = md->md_mbuf;
		md->md_mbuf = NULL;

		mbuf_setlen(m, seglen);

		if (m0 == NULL) {
			m0 = mtail = m;
			mbuf_pkthdr_setlen(m0, mbuf_len(m));
		} else {
			mbuf_setnext(mtail, m);
			mtail = m;
			mbuf_pkthdr_setlen(m0, mbuf_pkthdr_len(m0) + mbuf_len(m));
		}

		/* TODO handle desc->info */

		/* check if this was the last mbuf of the chain */
		if (seglast) {
			if (bad) {
				m_freem(m0);
				sc->sc_rx_fill_fail++;
			} else {
				ml_enqueue(ml, m0);
			}
			m0 = NULL;
			mtail = NULL;
			bad = false;
		}

		idx = (idx + 1) % q->mq_count;

		/* check if more data made it in */
		/* XXX should we actually do that? */
		if (idx == last &&
		    mtk_wlan_dma_hw_index(mwx_read(sc,
		    q->mq_regbase + MT_DMA_DMA_IDX), q->mq_count, &hw))
			last = hw;
	}

	/* A chain without its last segment (the C version asserted here,
	 * letting the hardware panic the kernel): drop it. */
	if (m0 != NULL) {
		m_freem(m0);
		sc->sc_rx_fill_fail++;
	}

	bus_dmamap_sync(sc->sc_dmat, q->mq_map, 0, q->mq_map->dm_mapsize,
	    BUS_DMASYNC_POSTREAD);

	q->mq_cons = idx;
}

void
mwx_dma_rx_done(struct mwx_softc *sc, struct mwx_queue *q)
{
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();

	mwx_dma_rx_dequeue(sc, q, &ml);

	if (ml_empty(&ml))
		return;

	mwx_queue_fill(sc, q);	/* TODO what if it fails, run timer? */

	mwx_dma_rx_process(sc, &ml);
}

struct mbuf *
mwx_mcu_alloc_msg(size_t len)
{
	const int headspace = sizeof(struct mwx_mcu_txd);
	struct mbuf *m;

	/* Allocate mbuf with enough space */
	m = MCLGETL(NULL, M_DONTWAIT, len + headspace);
	if (m == NULL)
		return NULL;
	mbuf_setlen(m, len + headspace);
	mbuf_pkthdr_setlen(m, len + headspace);
	m_adj(m, headspace);

	return m;
}

void
mwx_mcu_set_len(struct mbuf *m, void *end)
{
	uint8_t *start = mtod(m, uint8_t *);
	uint8_t *finish = (uint8_t *)end;
	int len = mbuf_pkthdr_len(m), adj;

	KASSERT(start <= finish && finish - start <= len);
	adj = len - (finish - start);
	m_adj(m, -adj);
}

int
mwx_mcu_send_mbuf(struct mwx_softc *sc, uint32_t cmd, struct mbuf *m, int *seqp)
{
	struct mwx_uni_txd *uni_txd;
	struct mwx_mcu_txd *mcu_txd;
	struct mwx_queue *q;
	uint32_t *txd, val;
	int s, rv, txd_len = 0, mcu_cmd = cmd & MCU_CMD_FIELD_ID_MASK;
	int tot_len, len = mbuf_pkthdr_len(m);
	size_t lead;
	uint8_t seq = 0;

	if (cmd == MCU_CMD_FW_SCATTER) {
		q = &sc->sc_txfwdlq;
		goto enqueue;
	}

	seq = ++sc->sc_mcu_seq & 0x0f;
	if (seq == 0)
		seq = ++sc->sc_mcu_seq & 0x0f;

	KASSERT(seq < nitems(sc->sc_mcu_wait));
	txd_len = cmd & MCU_CMD_FIELD_UNI ? sizeof(*uni_txd) : sizeof(*mcu_txd);
	tot_len = txd_len + len;
	lead = m_leadingspace(m);
	if (lead < (size_t)txd_len) {
		static uint32_t rebuild_log_count;
		struct mbuf *n;

		if (rebuild_log_count < 32) {
			MWX_DEV_LOG(sc,
			    "MCU tx mbuf rebuild cmd=0x%08x len=%d lead=%zu txd_len=%d\n",
			    cmd, len, lead, txd_len);
			rebuild_log_count++;
		}
		n = MCLGETL(NULL, M_DONTWAIT, len + txd_len);
		if (n == NULL) {
			m_freem(m);
			return ENOMEM;
		}
		mbuf_setlen(n, len + txd_len);
		mbuf_pkthdr_setlen(n, len + txd_len);
		if (len != 0 &&
		    mbuf_copydata(m, 0, len, mtod(n, uint8_t *) + txd_len) != 0) {
			m_freem(n);
			m_freem(m);
			return EINVAL;
		}
		m_freem(m);
		m = n;
	} else {
		m = m_prepend(m, txd_len, M_DONTWAIT);
		if (m == NULL)
			return ENOMEM;
	}
	txd = mtod(m, uint32_t *);
	memset(txd, 0, txd_len);

	val = (tot_len & MT_TXD0_TX_BYTES_MASK) |
	    MT_TX_TYPE_CMD | MT_TXD0_Q_IDX(MT_TX_MCU_PORT_RX_Q0);
	txd[0] = htole32(val);

	val = MT_TXD1_LONG_FORMAT | MT_HDR_FORMAT_CMD;
	txd[1] = htole32(val);

	if (cmd & MCU_CMD_FIELD_UNI) {
		uni_txd = (struct mwx_uni_txd *)txd;
		uni_txd->len = htole16(tot_len - sizeof(uni_txd->txd));
		// Linux connac2 requests an ACK for ROC acquire/abort even when
		// remain-on-channel waits only for the separate unsolicited grant.
		// Request that same wire contract without adding a synchronous wait.
		uni_txd->option = (seqp || mcu_cmd == 0x27) ? MCU_CMD_UNI_EXT_ACK :
		    (MCU_CMD_UNI | MCU_CMD_QUERY);
		uni_txd->cid = htole16(mcu_cmd);
		uni_txd->s2d_index = CMD_S2D_IDX_H2N;
		uni_txd->pkt_type = MCU_PKT_ID;
		uni_txd->seq = seq;
	} else {
		mcu_txd = (struct mwx_mcu_txd *)txd;
		mcu_txd->len = htole16(tot_len - sizeof(uni_txd->txd));
		mcu_txd->pq_id = htole16(MCU_PQ_ID(MT_TX_PORT_IDX_MCU,
			MT_TX_MCU_PORT_RX_Q0));
		mcu_txd->pkt_type = MCU_PKT_ID;
		mcu_txd->seq = seq;
		mcu_txd->cid = mcu_cmd;
		mcu_txd->s2d_index = CMD_S2D_IDX_H2N;
		mcu_txd->ext_cid = MCU_GET_EXT_CMD(cmd);

		if (mcu_txd->ext_cid || (cmd & MCU_CMD_FIELD_CE)) {
			if (cmd & MCU_CMD_FIELD_QUERY)
				mcu_txd->set_query = MCU_Q_QUERY;
			else
				mcu_txd->set_query = MCU_Q_SET;
			mcu_txd->ext_cid_ack = !!mcu_txd->ext_cid;
		} else {
			mcu_txd->set_query = MCU_Q_NA;
		}
	}

	if (seqp != NULL) {
		memset(&sc->sc_mcu_wait[seq], 0, sizeof(sc->sc_mcu_wait[0]));
		sc->sc_mcu_wait[seq].mcu_cmd = cmd;
		*seqp = seq;
	}
	q = &sc->sc_txmcuq;
enqueue:

	if (cmd != MCU_CMD_FW_SCATTER) {
		static uint32_t mcu_tx_log_count;

		if (mcu_tx_log_count < 96) {
			MWX_DEV_LOG(sc,
			    "mcu tx cmd=0x%08x seq=%u len=%d txd_len=%d q_reg=0x%08x\n",
			    cmd, seq, len, txd_len, q->mq_regbase);
			mcu_tx_log_count++;
		}
#ifdef MWX_PACKET_HEXDUMP
		pkt_hex_dump(m);
#endif
	}

	s = splnet();
	unsigned ringWaits=0;
	while (1) {
		rv = mwx_dma_tx_enqueue(sc, q, m);
		if (rv != EBUSY)
			break;
        // Teardown must be able to drain an AWDL MCU operation even when
        // firmware has stopped advancing the command ring.
        if(++ringWaits>=30) { rv=EWOULDBLOCK; break; }
		mwx_ebusy_sleeps++;
		q->mq_wakeme = 1;
		tsleep_nsec(q, 0, "mwxq", MSEC_TO_NSEC(100));
	}
	splx(s);
	if (rv != 0) {
		if (seqp != NULL)
			memset(&sc->sc_mcu_wait[seq], 0,
			    sizeof(sc->sc_mcu_wait[0]));
		m_freem(m);
	}
	return rv;
}

int
mwx_mcu_send_msg(struct mwx_softc *sc, uint32_t cmd, void *data, size_t len,
    int *seqp)
{
	struct mbuf *m;

	m = mwx_mcu_alloc_msg(len);
	if (m == NULL)
		return ENOMEM;

	if (len != 0)
		memcpy(mtod(m, caddr_t), data, len);

	return mwx_mcu_send_mbuf(sc, cmd, m, seqp);
}

int
mwx_mcu_send_wait(struct mwx_softc *sc, uint32_t cmd, void *data, size_t len)
{
	int rv, seq;

	rv = mwx_mcu_send_msg(sc, cmd, data, len, &seq);
	if (rv != 0)
		return rv;
	return mwx_mcu_wait_resp_int(sc, cmd, seq, NULL);
}

int
mwx_mcu_send_mbuf_wait(struct mwx_softc *sc, uint32_t cmd, struct mbuf *m)
{
	int rv, seq;

	rv = mwx_mcu_send_mbuf(sc, cmd, m, &seq);
	if (rv != 0)
		return rv;
	return mwx_mcu_wait_resp_int(sc, cmd, seq, NULL);
}

static void mwx_awdl_roc_event(mwx_softc *sc,const uint8_t *p,size_t n,uint64_t now)
{
    if(!sc->sc_awdl_lock) return;
    IOLockLock(sc->sc_awdl_lock);
    ++sc->sc_awdl_roc_events;
    sc->sc_awdl_roc_last_event_us=now;
    sc->sc_awdl_roc_last_response_us=sc->sc_awdl_roc.pending && now>=sc->sc_awdl_roc.requestedAt ?
        now-sc->sc_awdl_roc.requestedAt : 0;
    bool enabled=sc->sc_awdl_enabled && !sc->sc_awdl_stopping && !sc->sc_resetting && !sc->sc_detaching;
    mtkawdl::RocResult result=mtkawdl::receiveRocGrant(sc->sc_awdl_roc,p,n,
        sc->sc_awdl_vif.idx,enabled,now,sc->sc_awdl_roc_last_event);
    sc->sc_awdl_roc_last_result=result;
    ++sc->sc_awdl_roc_reasons[static_cast<unsigned>(result)];
    if(result==mtkawdl::RocResult::Accepted) {
        ++sc->sc_awdl_roc_grants;
        sc->sc_awdl_roc_grant_delay_us=sc->sc_awdl_roc.responseDelayUS;
        {
            uint32_t ms=sc->sc_awdl_roc.responseDelayUS/1000; unsigned b=0;
            while(b<6 && ms>=(5u<<b)) ++b;
            unsigned kind=0;
            if(sc->sc_ic.ic_opmode==IEEE80211_M_STA && sc->sc_ic.ic_state==IEEE80211_S_RUN &&
               sc->sc_ic.ic_bss && sc->sc_ic.ic_bss->ni_chan)
                kind=ieee80211_chan2ieee(&sc->sc_ic,sc->sc_ic.ic_bss->ni_chan)==sc->sc_awdl_roc.channel ? 1 : 2;
            ++sc->sc_awdl_roc_latency[kind][b];
        }
        if(sc->sc_awdl_roc_grant_delay_us>sc->sc_awdl_roc_max_grant_delay_us)
            sc->sc_awdl_roc_max_grant_delay_us=sc->sc_awdl_roc_grant_delay_us;
    } else ++sc->sc_awdl_roc_rejected;
    IOLockUnlock(sc->sc_awdl_lock);
}

// Asynchronous ROC command acknowledgements are not radio grants. Consume
// them by CID before the generic sequence-indexed waiter path: a delayed ROC
// ACK must not complete a later BSS/STA command using the same four-bit seq.
static bool mwx_awdl_roc_command_result(mwx_softc *sc,const uint8_t *p,size_t n,uint64_t now)
{
    if(!n || p[0]!=0x27) return false;
    if(sc->sc_awdl_lock) {
        IOLockLock(sc->sc_awdl_lock);
        if(n<sizeof(mt7921_mcu_uni_event)) ++sc->sc_awdl_roc_command_malformed;
        else {
            ++sc->sc_awdl_roc_command_results;
            sc->sc_awdl_roc_command_status=mtkawdl::le32(p+4);
            sc->sc_awdl_roc_command_result_us=now;
            if(sc->sc_awdl_roc_command_status) ++sc->sc_awdl_roc_command_failed;
        }
        IOLockUnlock(sc->sc_awdl_lock);
    }
    return true;
}

void
mwx_mcu_rx_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct mwx_mcu_rxd *rxd;
	uint32_t cmd, mcu_int = 0;
	uint8_t rxd_eid, rxd_ext_eid, rxd_seq, rxd_option;
	int len, rxd_size;

	rxd_size = MT7921_MCU_RXD_SIZE;
	if ((m = m_pullup(m, sizeof(*rxd) + rxd_size)) == NULL)
		return;
	m_adj(m, rxd_size);
	rxd = mtod(m, struct mwx_mcu_rxd *);

	if (rxd->ext_eid == MCU_EXT_EVENT_RATE_REPORT) {
		printf("%s: MCU_EXT_EVENT_RATE_REPORT COMMAND\n", DEVNAME(sc));
		m_freem(m);
		return;
	}

	len = le16toh(rxd->len);
	/* make sure all the data is in one mbuf */
	if ((m = m_pullup(m, len)) == NULL) {
		printf("%s: mwx_mcu_rx_event m_pullup failed\n", DEVNAME(sc));
		return;
	}
	/* refetch after pullup */
	rxd = mtod(m, struct mwx_mcu_rxd *);
	rxd_eid = rxd->eid;
	rxd_ext_eid = rxd->ext_eid;
	rxd_seq = rxd->seq;
    rxd_option = rxd->option;
	m_adj(m, sizeof(*rxd));
	sc->sc_mcu_events++;
	sc->sc_last_mcu_eid = rxd_eid;
    if ((rxd_option & MCU_UNI_CMD_UNSOLICITED_EVENT) && rxd_eid == MCU_UNI_EVENT_ROC) {
        uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        mwx_awdl_roc_event(sc,mtod(m,const uint8_t *),mbuf_len(m),ns/1000);
        m_freem(m); return;
    }
    if (rxd_eid == MCU_UNI_EVENT_RESULT && (rxd_option & MCU_UNI_CMD_EVENT) &&
        !(rxd_option & MCU_UNI_CMD_UNSOLICITED_EVENT)) {
        uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        if(mwx_awdl_roc_command_result(sc,mtod(m,const uint8_t *),mbuf_len(m),ns/1000)) {
            m_freem(m); return;
        }
    }

	switch (rxd_eid) {
	case MCU_EVENT_SCHED_SCAN_DONE:
	case MCU_EVENT_SCAN_DONE:
		mt7921_mcu_scan_event(sc, m);
		break;
	case MCU_EVENT_BSS_BEACON_LOSS:
		mt7921_mcu_beacon_loss_event(sc, m);
		break;
	case MCU_EVENT_BSS_ABSENCE:
		/* AP absence (NoA) notices; only matter for P2P power save. */
		break;
#if 0
	case MCU_EVENT_DBG_MSG:
		mt7921_mcu_debug_msg_event(dev, skb);
		break;
#endif
	case MCU_EVENT_COREDUMP:
		/* it makes little sense to write the coredump down */
		if (!sc->sc_resetting)
			printf("%s: coredump event\n", DEVNAME(sc));
		mwx_reset(sc);
		break;
	case MCU_EVENT_LP_INFO:
		mt7921_mcu_low_power_event(sc, m);
		break;
	case MCU_EVENT_TX_DONE:
		mt7921_mcu_tx_done_event(sc, m);
		break;
	case 0x6:
		printf("%s: MAGIC COMMAND\n", DEVNAME(sc));
	default:
		if (rxd_seq == 0 || rxd_seq >= nitems(sc->sc_mcu_wait)) {
			printf("%s: mcu rx bad seq %x eid 0x%x ext 0x%x\n",
			    DEVNAME(sc), rxd_seq, rxd_eid, rxd_ext_eid);
			sc->sc_mcu_bad_seq++;
			break;
		}

		cmd = sc->sc_mcu_wait[rxd_seq].mcu_cmd;

		if (cmd == MCU_CMD_PATCH_SEM_CONTROL ||
		    cmd == MCU_CMD_PATCH_FINISH_REQ) {
			mcu_int = rxd_ext_eid;
			MWX_DEV_LOG(sc, "MCU patch response cmd=0x%08x seq=%u status=0x%x\n",
			    cmd, rxd_seq, mcu_int);
		} else if (cmd == MCU_EXT_CMD_THERMAL_CTRL) {
			if (mbuf_len(m) < sizeof(uint32_t) * 2)
				break;
			mcu_int = le32toh(mtod(m, uint32_t *)[1]);
		} else if (cmd == MCU_EXT_CMD_EFUSE_ACCESS) {
			//ret = mt7921_mcu_parse_eeprom(sc, m);
			printf("%s: mcu resp no handled yet\n", DEVNAME(sc));
		} else if (cmd == MCU_UNI_CMD_DEV_INFO_UPDATE ||
		    cmd == MCU_UNI_CMD_BSS_INFO_UPDATE ||
		    cmd == MCU_UNI_CMD_STA_REC_UPDATE ||
		    cmd == MCU_UNI_CMD_HIF_CTRL ||
		    cmd == MCU_UNI_CMD_OFFLOAD ||
		    cmd == MCU_UNI_CMD_SUSPEND) {
			struct mt7921_mcu_uni_event *event;

			if (mbuf_len(m) < sizeof(*event))
				break;
			event = mtod(m, struct mt7921_mcu_uni_event *);
			mcu_int = le32toh(event->status);
		} else if (cmd == MCU_CE_QUERY_REG_READ) {
			struct mt7921_mcu_reg_event *event;

			if (mbuf_len(m) < sizeof(*event))
				break;
			event = mtod(m, struct mt7921_mcu_reg_event *);
			mcu_int = le32toh(event->val);
		}

		sc->sc_mcu_wait[rxd_seq].mcu_int = mcu_int;
		sc->sc_mcu_wait[rxd_seq].mcu_m = m;
		sc->sc_mcu_wait[rxd_seq].mcu_done = 1;
        if(gMwxHal) gMwxHal->mwxWakeup(&sc->sc_mcu_wait[rxd_seq]);
        else wakeup(&sc->sc_mcu_wait[rxd_seq]);
		return;
	}

	m_freem(m);
}

/* Log WFDMA state when an MCU command gets no response. */
static void
mwx_dump_mcu_state(struct mwx_softc *sc, uint32_t cmd, int seq)
{
	MWX_DEV_LOG(sc, "timeout diag cmd=0x%08x seq=%d lpctl=0x%08x int_sta=0x%08x int_ena=0x%08x\n",
	    cmd, seq, mwx_read(sc, MT_CONN_ON_LPCTL),
	    mwx_read(sc, MT_WFDMA0_HOST_INT_STA),
	    mwx_read(sc, MT_WFDMA0_HOST_INT_ENA));
	MWX_DEV_LOG(sc, "timeout diag rxmcu cpu=%u dma=%u cons=%u rxfwdl cpu=%u dma=%u cons=%u txmcu cpu=%u dma=%u cons=%u\n",
	    mwx_read(sc, sc->sc_rxmcuq.mq_regbase + MT_DMA_CPU_IDX),
	    mwx_read(sc, sc->sc_rxmcuq.mq_regbase + MT_DMA_DMA_IDX),
	    sc->sc_rxmcuq.mq_cons,
	    mwx_read(sc, sc->sc_rxfwdlq.mq_regbase + MT_DMA_CPU_IDX),
	    mwx_read(sc, sc->sc_rxfwdlq.mq_regbase + MT_DMA_DMA_IDX),
	    sc->sc_rxfwdlq.mq_cons,
	    mwx_read(sc, sc->sc_txmcuq.mq_regbase + MT_DMA_CPU_IDX),
	    mwx_read(sc, sc->sc_txmcuq.mq_regbase + MT_DMA_DMA_IDX),
	    sc->sc_txmcuq.mq_cons);
}

/* The response may land before we sleep, so poll the done flag
 * instead of trusting a single wakeup. */
static int
mwx_mcu_sleep_resp(struct mwx_softc *sc, uint32_t cmd, int seq)
{
	int i;

	for (i = 0; i < 300; i++) {
		if (sc->sc_mcu_wait[seq].mcu_done)
			return 0;
		tsleep_nsec(&sc->sc_mcu_wait[seq], 0, "mwxwait",
		    MSEC_TO_NSEC(10));
	}
	if (sc->sc_mcu_wait[seq].mcu_done)
		return 0;
	sc->sc_mcu_timeouts++;
	mwx_dump_mcu_state(sc, cmd, seq);
	return EWOULDBLOCK;
}

int
mwx_mcu_wait_resp_int(struct mwx_softc *sc, uint32_t cmd, int seq,
    uint32_t *val)
{
	int rv;

	KASSERT(seq < nitems(sc->sc_mcu_wait));

	rv = mwx_mcu_sleep_resp(sc, cmd, seq);
	if (rv != 0) {
		printf("%s: command %x timeout\n", DEVNAME(sc), cmd);
		mwx_reset(sc);
		return rv;
	}

	if (sc->sc_mcu_wait[seq].mcu_m != NULL) {
		m_freem(sc->sc_mcu_wait[seq].mcu_m);
		sc->sc_mcu_wait[seq].mcu_m = NULL;
	}
	if (val != NULL)
		*val = sc->sc_mcu_wait[seq].mcu_int;
	return 0;
}

int
mwx_mcu_wait_resp_msg(struct mwx_softc *sc, uint32_t cmd, int seq,
    struct mbuf **mp)
{
	int rv;

	KASSERT(seq < nitems(sc->sc_mcu_wait));

	rv = mwx_mcu_sleep_resp(sc, cmd, seq);
	if (rv != 0) {
		printf("%s: command %x timeout\n", DEVNAME(sc), cmd);
		mwx_reset(sc);
		return rv;
	}
	if (sc->sc_mcu_wait[seq].mcu_m == NULL) {
		printf("%s: command response missing\n", DEVNAME(sc));
		return ENOENT;
	}
	if (mp != NULL)
		*mp = sc->sc_mcu_wait[seq].mcu_m;
	else
		m_freem(sc->sc_mcu_wait[seq].mcu_m);
	sc->sc_mcu_wait[seq].mcu_m = NULL;
	return 0;
}

int
mwx_dma_disable(struct mwx_softc *sc, int force)
{
	int rv;

	/* disable WFDMA0 */
	mwx_clear(sc, MT_WFDMA0_GLO_CFG,
	    MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN |
	    MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
	    MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
	    MT_WFDMA0_GLO_CFG_OMIT_RX_INFO |
	    MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);

	rv = mwx_poll(sc, MT_WFDMA0_GLO_CFG, 0,
	    MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY, 100);
	if (!force && rv != 0)
		return rv;

	/* disable DMASHDL and set bypass */
	mwx_clear(sc, MT_WFDMA0_GLO_CFG_EXT0, MT_WFDMA0_CSR_TX_DMASHDL_ENABLE);
	mwx_set(sc, MT_DMASHDL_SW_CONTROL, MT_DMASHDL_DMASHDL_BYPASS);

	if (force) {
		/* reset */
		mwx_clear(sc, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST |
		    MT_WFDMA0_RST_LOGIC_RST);
		mwx_set(sc, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST |
		    MT_WFDMA0_RST_LOGIC_RST);
		delay(20);
	}

	return 0;
}

void
mwx_dma_enable(struct mwx_softc *sc)
{
#define PREFETCH(base, depth)   ((base) << 16 | (depth))
	/* configure perfetch settings */
	mwx_write(sc, MT_WFDMA0_RX_RING0_EXT_CTRL, PREFETCH(0x0, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING2_EXT_CTRL, PREFETCH(0x40, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING3_EXT_CTRL, PREFETCH(0x80, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING4_EXT_CTRL, PREFETCH(0xc0, 0x4));
	mwx_write(sc, MT_WFDMA0_RX_RING5_EXT_CTRL, PREFETCH(0x100, 0x4));

	mwx_write(sc, MT_WFDMA0_TX_RING0_EXT_CTRL, PREFETCH(0x140, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING1_EXT_CTRL, PREFETCH(0x180, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING2_EXT_CTRL, PREFETCH(0x1c0, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING3_EXT_CTRL, PREFETCH(0x200, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING4_EXT_CTRL, PREFETCH(0x240, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING5_EXT_CTRL, PREFETCH(0x280, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING6_EXT_CTRL, PREFETCH(0x2c0, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING16_EXT_CTRL, PREFETCH(0x340, 0x4));
	mwx_write(sc, MT_WFDMA0_TX_RING17_EXT_CTRL, PREFETCH(0x380, 0x4));

	/* reset dma idx */
	mwx_write(sc, MT_WFDMA0_RST_DTX_PTR, ~0);

	/* configure delay interrupt */
	mwx_write(sc, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	mwx_set(sc, MT_WFDMA0_GLO_CFG,
	    MT_WFDMA0_GLO_CFG_TX_WB_DDONE |
	    MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN |
	    MT_WFDMA0_GLO_CFG_CLK_GAT_DIS |
	    MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
	    MT_WFDMA0_GLO_CFG_DMA_SIZE(3) |
	    MT_WFDMA0_GLO_CFG_FIFO_DIS_CHECK |
	    MT_WFDMA0_GLO_CFG_RX_WB_DDONE |
	    MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN |
	    MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);

	mwx_barrier(sc);
	mwx_set(sc, MT_WFDMA0_GLO_CFG,
	    MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	mwx_set(sc, MT_WFDMA_DUMMY_CR, MT_WFDMA_NEED_REINIT);

	/* enable interrupts for TX/RX rings */
	mwx_write(sc, MT_WFDMA0_HOST_INT_ENA, sc->sc_intr_mask);
	mwx_set(sc, MT_MCU2HOST_SW_INT_ENA, MT_MCU_CMD_WAKE_RX_PCIE);
	mwx_write(sc, MT_PCIE_MAC_INT_ENABLE, 0xff);
}

int
mwx_mcu_fw_pmctrl(struct mwx_softc *sc)
{
	int i;

	for (i = 0; i < MWX_MCU_INIT_RETRY_COUNT; i++) {
		mwx_write(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_SET_OWN);
		if (mwx_poll(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC,
		    4, 50) == 0)
			break;
	}

	if (i == MWX_MCU_INIT_RETRY_COUNT) {
		printf("%s: firmware own failed\n", DEVNAME(sc));
		return EIO;
	}

	return 0;
}

int
mwx_mcu_drv_pmctrl(struct mwx_softc *sc)
{
	int i;

	for (i = 0; i < MWX_MCU_INIT_RETRY_COUNT; i++) {
		mwx_write(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
		delay(3000);
		if (mwx_poll(sc, MT_CONN_ON_LPCTL, 0,
		    PCIE_LPCR_HOST_OWN_SYNC, 50) == 0)
			break;
	}

	if (i == MWX_MCU_INIT_RETRY_COUNT) {
		printf("%s: driver own failed\n", DEVNAME(sc));
		return EIO;
	}

	return 0;
}

int
mwx_wfsys_reset(struct mwx_softc *sc)
{
	DPRINTF("%s: WFSYS reset\n", DEVNAME(sc));

	mwx_clear(sc, MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);
	delay(50 * 1000);
	mwx_set(sc, MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);

	return mwx_poll(sc, MT_WFSYS_SW_RST_B, WFSYS_SW_INIT_DONE,
	    WFSYS_SW_INIT_DONE, 500);
}

/*
 * To be honest this is ridiculous.
 */
uint32_t
mwx_reg_addr(struct mwx_softc *sc, uint32_t reg)
{
	static const struct {
		uint32_t phys;
		uint32_t mapped;
		uint32_t size;
	} fixed_map[] = {
	{ 0x820d0000, 0x30000, 0x10000 }, /* WF_LMAC_TOP (WF_WTBLON) */
	{ 0x820ed000, 0x24800, 0x00800 }, /* WF_LMAC_TOP BN0 (WF_MIB) */
	{ 0x820e4000, 0x21000, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_TMAC) */
	{ 0x820e7000, 0x21e00, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_DMA) */
	{ 0x820eb000, 0x24200, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_LPON) */
	{ 0x820e2000, 0x20800, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_AGG) */
	{ 0x820e3000, 0x20c00, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_ARB) */
	{ 0x820e5000, 0x21400, 0x00800 }, /* WF_LMAC_TOP BN0 (WF_RMAC) */
	{ 0x00400000, 0x80000, 0x10000 }, /* WF_MCU_SYSRAM */
	{ 0x00410000, 0x90000, 0x10000 }, /* WF_MCU_SYSRAM (conf register) */
	{ 0x40000000, 0x70000, 0x10000 }, /* WF_UMAC_SYSRAM */
	{ 0x54000000, 0x02000, 0x01000 }, /* WFDMA PCIE0 MCU DMA0 */
	{ 0x55000000, 0x03000, 0x01000 }, /* WFDMA PCIE0 MCU DMA1 */
	{ 0x58000000, 0x06000, 0x01000 }, /* WFDMA PCIE1 MCU DMA0 (MEM_DMA) */
	{ 0x59000000, 0x07000, 0x01000 }, /* WFDMA PCIE1 MCU DMA1 */
	{ 0x7c000000, 0xf0000, 0x10000 }, /* CONN_INFRA */
	{ 0x7c020000, 0xd0000, 0x10000 }, /* CONN_INFRA, WFDMA */
	{ 0x7c060000, 0xe0000, 0x10000 }, /* CONN_INFRA, conn_host_csr_top */
	{ 0x80020000, 0xb0000, 0x10000 }, /* WF_TOP_MISC_OFF */
	{ 0x81020000, 0xc0000, 0x10000 }, /* WF_TOP_MISC_ON */
	{ 0x820c0000, 0x08000, 0x04000 }, /* WF_UMAC_TOP (PLE) */
	{ 0x820c8000, 0x0c000, 0x02000 }, /* WF_UMAC_TOP (PSE) */
	{ 0x820cc000, 0x0e000, 0x01000 }, /* WF_UMAC_TOP (PP) */
	{ 0x820cd000, 0x0f000, 0x01000 }, /* WF_MDP_TOP */
	{ 0x820ce000, 0x21c00, 0x00200 }, /* WF_LMAC_TOP (WF_SEC) */
	{ 0x820cf000, 0x22000, 0x01000 }, /* WF_LMAC_TOP (WF_PF) */
	{ 0x820e0000, 0x20000, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_CFG) */
	{ 0x820e1000, 0x20400, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_TRB) */
	{ 0x820e9000, 0x23400, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
	{ 0x820ea000, 0x24000, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_ETBF) */
	{ 0x820ec000, 0x24600, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_INT) */
	{ 0x820f0000, 0xa0000, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_CFG) */
	{ 0x820f1000, 0xa0600, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_TRB) */
	{ 0x820f2000, 0xa0800, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_AGG) */
	{ 0x820f3000, 0xa0c00, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_ARB) */
	{ 0x820f4000, 0xa1000, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_TMAC) */
	{ 0x820f5000, 0xa1400, 0x00800 }, /* WF_LMAC_TOP BN1 (WF_RMAC) */
	{ 0x820f7000, 0xa1e00, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_DMA) */
	{ 0x820f9000, 0xa3400, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
	{ 0x820fa000, 0xa4000, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_ETBF) */
	{ 0x820fb000, 0xa4200, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_LPON) */
	{ 0x820fc000, 0xa4600, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_INT) */
	{ 0x820fd000, 0xa4800, 0x00800 }, /* WF_LMAC_TOP BN1 (WF_MIB) */
	};
	int i;

	if (reg < 0x100000)
		return reg;

	for (i = 0; i < nitems(fixed_map); i++) {
		uint32_t ofs;

		if (reg < fixed_map[i].phys)
			continue;

		ofs = reg - fixed_map[i].phys;
		if (ofs > fixed_map[i].size)
			continue;

		return fixed_map[i].mapped + ofs;
	}

	if ((reg >= 0x18000000 && reg < 0x18c00000) ||
	    (reg >= 0x70000000 && reg < 0x78000000) ||
	    (reg >= 0x7c000000 && reg < 0x7c400000))
		return mt7921_map_reg_l1(sc, reg);

	panic("%s: Access to currently unsupported address %08x\n",
	    DEVNAME(sc), reg);
}

int
mwx_init_hardware(struct mwx_softc *sc)
{
	int rv;

	MWX_DEV_LOG(sc, "hardware init start\n");
	/* reset dma */
	rv = mwx_dma_reset(sc, 1);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init dma reset failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "hardware init dma reset ok\n");

	/*
	 * force firmware operation mode into normal state,
	 * which should be set before firmware download stage.
	 */
	mwx_write(sc, MT_SWDEF_MODE, MT_SWDEF_NORMAL_MODE);
	mwx_barrier(sc);
	MWX_DEV_LOG(sc, "hardware init normal firmware mode set\n");

	rv = mt7921_mcu_init(sc);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init MCU init failed error=%d\n", rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "hardware init MCU init ok\n");
	/* TODO override eeprom for systems with FDT */
	rv = mt7921_mcu_set_eeprom(sc);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init EEPROM setup failed error=%d\n", rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "hardware init EEPROM setup ok\n");
	mwx_mac_init(sc);

	sc->sc_vif.vif_mn.wcid = MWX_WTBL_RESERVED;

	/* MAYBE alloc beacon and mgmt frame wcid 0 here */

	MWX_DEV_LOG(sc, "hardware init complete\n");
	return 0;

 fail:
	/* reset dma */
	MWX_DEV_LOG(sc, "hardware init recovery dma reset start\n");
	rv = mwx_dma_reset(sc, 1);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "hardware init recovery dma reset failed error=%d\n",
		    rv);
		return rv;
	}
	return EAGAIN;
}

int
mt7921_mcu_init(struct mwx_softc *sc)
{
	int rv;

	MWX_DEV_LOG(sc, "MCU init start\n");
	/* this read is needed to make interrupts work */
	(void) mwx_read(sc, MT_TOP_LPCR_HOST_BAND0);
	mwx_write(sc, MT_TOP_LPCR_HOST_BAND0, MT_TOP_LPCR_HOST_DRV_OWN);
	if (mwx_poll(sc, MT_TOP_LPCR_HOST_BAND0, 0, MT_TOP_LPCR_HOST_FW_OWN,
	    5000) != 0) {
	    MWX_DEV_LOG(sc, "MCU init timeout for driver own\n");
	    return EIO;
	}
	MWX_DEV_LOG(sc, "MCU init driver own ok\n");

	mwx_set(sc, MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS);

	if ((rv = mwx_load_firmware(sc)) != 0) {
		MWX_DEV_LOG(sc, "MCU init firmware load failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "MCU init firmware load ok\n");

	if ((rv = mt7921_mcu_get_nic_capability(sc)) != 0) {
		MWX_DEV_LOG(sc, "MCU init NIC capability failed error=%d\n", rv);
		return rv;
	}
	MWX_DEV_LOG(sc, "MCU init NIC capability ok\n");
	if ((rv = mt7921_mcu_fw_log_2_host(sc, 1)) != 0) {
		MWX_DEV_LOG(sc, "MCU init firmware log enable failed error=%d\n",
		    rv);
		return rv;
	}

	/* TODO mark MCU running */

	MWX_DEV_LOG(sc, "MCU init complete\n");
	return 0;
}

static inline uint32_t
mwx_get_data_mode(struct mwx_softc *sc, uint32_t info)
{
	uint32_t mode = DL_MODE_NEED_RSP;

	if (info == PATCH_SEC_NOT_SUPPORT)
		return mode;
	switch (info & PATCH_SEC_ENC_TYPE_MASK) {
	case PATCH_SEC_ENC_TYPE_PLAIN:
		break;
	case PATCH_SEC_ENC_TYPE_AES:
		mode |= DL_MODE_ENCRYPT;
		mode |= (info << DL_MODE_KEY_IDX_SHIFT) & DL_MODE_KEY_IDX_MASK;
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	case PATCH_SEC_ENC_TYPE_SCRAMBLE:
		mode |= DL_MODE_ENCRYPT;
		mode |= DL_CONFIG_ENCRY_MODE_SEL;
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	default:
		printf("%s: encryption type not supported\n", DEVNAME(sc));
	}
	return mode;
}

static inline uint32_t
mwx_mcu_gen_dl_mode(uint8_t feature_set)
{
	uint32_t ret = DL_MODE_NEED_RSP;

	if (feature_set & FW_FEATURE_SET_ENCRYPT)
		ret |= (DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV);
	if (feature_set & FW_FEATURE_ENCRY_MODE)
		ret |= DL_CONFIG_ENCRY_MODE_SEL;

	/* FW_FEATURE_SET_KEY_IDX_MASK == DL_MODE_KEY_IDX_MASK */
	ret |= feature_set & FW_FEATURE_SET_KEY_IDX_MASK;

	return ret;
}


int
mwx_load_firmware(struct mwx_softc *sc)
{
	const char *rompatch, *fw;
	u_char *buf = NULL, *fwbuf = NULL;
	size_t buflen = 0, fwlen = 0;
	uint32_t reg, n_region, override = 0, option = 0;
	int i, rv, sem;

	MWX_DEV_LOG(sc, "firmware load start hwtype=%s\n",
	    sc->sc_hwtype == MWX_HW_MT7922 ? "MT7922" : "MT7921");
	reg = mwx_read(sc, MT_CONN_ON_MISC) & MT_TOP_MISC2_FW_N9_RDY;
	if (reg != 0 && sc->sc_fw_restart) {
		/* Like mt76_connac_mcu_restart(): drop the running firmware. */
		uint8_t req[4] = { 1, 0, 0, 0 };

		sc->sc_fw_restart = 0;
		rv = mwx_mcu_send_msg(sc, MCU_CMD_NIC_POWER_CTRL, req,
		    sizeof(req), NULL);
		rv = mwx_poll(sc, MT_CONN_ON_MISC, MT_TOP_MISC2_FW_PWR_ON,
		    MT_TOP_MISC_FW_STATE, 1000);
		reg = mwx_read(sc, MT_CONN_ON_MISC);
		MWX_DEV_LOG(sc, "firmware restart rv=%d misc=0x%08x\n", rv, reg);
		reg &= MT_TOP_MISC2_FW_N9_RDY;
		if (reg == MT_TOP_MISC2_FW_N9_RDY)
			return EIO;
		reg = 0;
	}
	if (reg != 0) {
		MWX_DEV_LOG(sc, "firmware already downloaded status=0x%08x\n", reg);
		return 0;
	}
	sc->sc_fw_restart = 0;

	switch (sc->sc_hwtype) {
	case MWX_HW_MT7921:
		rompatch = MT7921_ROM_PATCH;
		fw = MT7921_FIRMWARE_WM;
		break;
	case MWX_HW_MT7922:
		rompatch = MT7922_ROM_PATCH;
		fw = MT7922_FIRMWARE_WM;
		break;
	}
	MWX_DEV_LOG(sc, "firmware selected patch=%s wm=%s\n", rompatch, fw);

	if ((rv = loadfirmware(rompatch, &buf, &buflen)) != 0) {
		MWX_DEV_LOG(sc, "patch firmware load failed name=%s error=%d\n",
		    rompatch, rv);
		return rv;
	}
	if ((rv = loadfirmware(fw, &fwbuf, &fwlen)) != 0) {
		MWX_DEV_LOG(sc, "WM firmware load failed name=%s error=%d\n",
		    fw, rv);
		free(buf, M_DEVBUF, buflen);
		return rv;
	}
	MWX_DEV_LOG(sc, "firmware blobs loaded patch_size=%zu wm_size=%zu\n",
	    buflen, fwlen);

	rv = mwx_mcu_patch_sem_ctrl(sc, 1);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "patch semaphore acquire failed error=%d\n", rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "patch semaphore acquired\n");

	/* MtkWireless: image layout is parsed and bounds-checked in Rust
	 * (rust/mtk-wlan/src/connac2/fw.rs). */
	mtk_wlan_patch_info pinfo;
	if ((rv = mtk_wlan_patch_parse(buf, buflen, &pinfo)) != 0) {
		MWX_DEV_LOG(sc, "invalid patch firmware size=%zu\n", buflen);
		goto out;
	}
	n_region = pinfo.regions;
	MWX_DEV_LOG(sc, "patch header hw_sw=0x%x patch_ver=0x%x regions=%u build=%.15s\n",
	    pinfo.hw_sw_ver, pinfo.patch_ver, n_region, pinfo.build_date);

	for (i = 0; i < n_region; i++) {
		mtk_wlan_fw_region region;

		if ((rv = mtk_wlan_patch_region(buf, buflen, i, &region)) != 0) {
			MWX_DEV_LOG(sc, "invalid patch region index=%d\n", i);
			goto out;
		}
		MWX_DEV_LOG(sc, "patch region index=%d addr=0x%08x len=%u mode=0x%08x\n",
		    i, region.addr, region.len, region.mode);

		rv = mwx_mcu_init_download(sc, region.addr, region.len, region.mode);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "patch download request failed index=%d error=%d\n",
			    i, rv);
			goto out;
		}
		rv = mwx_mcu_send_firmware(sc, MCU_CMD_FW_SCATTER,
		    buf + region.offset, region.len, MWX_FW_CHUNK);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "patch send failed index=%d error=%d\n",
			    i, rv);
			goto out;
		}
	}

	delay(5000);
	rv = mwx_mcu_start_patch(sc);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "patch start failed error=%d\n", rv);
		goto out;
	}
	MWX_DEV_LOG(sc, "patch start ok\n");

out:
	sem = mwx_mcu_patch_sem_ctrl(sc, 0);
	if (sem != 0)
		rv = sem;
	if (rv != 0)
		goto fail;

	mtk_wlan_ram_info rinfo;
	if ((rv = mtk_wlan_ram_parse(fwbuf, fwlen, &rinfo)) != 0) {
		MWX_DEV_LOG(sc, "invalid WM firmware size=%zu\n", fwlen);
		goto fail;
	}
	MWX_DEV_LOG(sc, "WM header version=%.10s build=%.15s chip=0x%02x eco=0x%02x regions=%u\n",
	    rinfo.fw_ver, rinfo.build_date, rinfo.chip_id,
	    rinfo.eco_code, rinfo.regions);

	for (i = 0; i < rinfo.regions; i++) {
		mtk_wlan_fw_region region;

		if ((rv = mtk_wlan_ram_region(fwbuf, fwlen, i, &region)) != 0) {
			MWX_DEV_LOG(sc, "invalid WM region index=%d\n", i);
			goto fail;
		}
		MWX_DEV_LOG(sc, "WM region index=%d addr=0x%08x len=%u mode=0x%08x feature=0x%02x offset=%zu ebusy_sleeps=%u enomem_retries=%u\n",
		    i, region.addr, region.len, region.mode, region.feature,
		    region.offset, mwx_ebusy_sleeps, mwx_enomem_retries);
		if (!region.download)
			continue;

		rv = mwx_mcu_init_download(sc, region.addr, region.len, region.mode);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "WM download request failed index=%d error=%d\n",
			    i, rv);
			goto fail;
		}

		rv = mwx_mcu_send_firmware(sc, MCU_CMD_FW_SCATTER,
		    fwbuf + region.offset, region.len, MWX_FW_CHUNK);
		if (rv != 0) {
			MWX_DEV_LOG(sc, "WM send failed index=%d error=%d\n",
			    i, rv);
			goto fail;
		}
	}

	override = rinfo.override_addr;
	option = rinfo.start_option;
	rv = mwx_mcu_start_firmware(sc, override, option);
	if (rv != 0) {
		MWX_DEV_LOG(sc, "firmware start failed override=0x%08x option=0x%08x error=%d\n",
		    override, option, rv);
		goto fail;
	}
	MWX_DEV_LOG(sc, "firmware start ok override=0x%08x option=0x%08x\n",
	    override, option);

	/* XXX should not busy poll here */
	if (mwx_poll(sc, MT_CONN_ON_MISC, MT_TOP_MISC2_FW_N9_RDY,
	    MT_TOP_MISC2_FW_N9_RDY, 1500) != 0) {
		MWX_DEV_LOG(sc, "firmware ready poll timed out\n");
		rv = EIO;
		goto fail;
	}

	MWX_DEV_LOG(sc, "firmware loaded and ready\n");
	rv = 0;

fail:
	if (buf != NULL)
		free(buf, M_DEVBUF, buflen);
	if (fwbuf != NULL)
		free(fwbuf, M_DEVBUF, fwlen);
	return rv;
}

int
mwx_mac_wtbl_update(struct mwx_softc *sc, int idx)
{
	mwx_rmw(sc, MT_WTBL_UPDATE,
	    (idx & MT_WTBL_UPDATE_WLAN_IDX_MASK) | MT_WTBL_UPDATE_ADM_COUNT_CLEAR,
	    MT_WTBL_UPDATE_WLAN_IDX_MASK);

	return mwx_poll(sc, MT_WTBL_UPDATE, 0, MT_WTBL_UPDATE_BUSY, 5000);
}

void
mwx_mac_init_band(struct mwx_softc *sc, uint32_t band)
{
	mwx_rmw(sc, MT_TMAC_CTCR0(band), 0x3f, MT_TMAC_CTCR0_INS_DDLMT_REFTIME);
	mwx_set(sc, MT_TMAC_CTCR0(band),
	    MT_TMAC_CTCR0_INS_DDLMT_VHT_SMPDU_EN |
	    MT_TMAC_CTCR0_INS_DDLMT_EN);

	mwx_set(sc, MT_WF_RMAC_MIB_TIME0(band), MT_WF_RMAC_MIB_RXTIME_EN);
	mwx_set(sc, MT_WF_RMAC_MIB_AIRTIME0(band), MT_WF_RMAC_MIB_RXTIME_EN);

	/* enable MIB tx-rx time reporting */
	mwx_set(sc, MT_MIB_SCR1(band), MT_MIB_TXDUR_EN);
	mwx_set(sc, MT_MIB_SCR1(band), MT_MIB_RXDUR_EN);

	mwx_rmw(sc, MT_DMA_DCR0(band),
	    1536 << MT_DMA_DCR0_MAX_RX_LEN_SHIFT, MT_DMA_DCR0_MAX_RX_LEN_MASK);
	/* disable rx rate report by default due to hw issues */
	mwx_clear(sc, MT_DMA_DCR0(band), MT_DMA_DCR0_RXD_G5_EN);

	/* filter out non-resp frames and get instantaneous signal reporting */
	mwx_rmw(sc, MT_WTBLOFF_TOP_RSCR(band),
	    0 << MT_WTBLOFF_TOP_RSCR_RCPI_MODE_SHIFT |
	    0x3 << MT_WTBLOFF_TOP_RSCR_RCPI_PARAM_SHIFT,
	    MT_WTBLOFF_TOP_RSCR_RCPI_MODE_MASK |
	    MT_WTBLOFF_TOP_RSCR_RCPI_PARAM_MASK);
}

void
mwx_mac_init(struct mwx_softc *sc)
{
	int i;

	mwx_rmw(sc, MT_MDP_DCR1, 1536 << MT_MDP_DCR1_MAX_RX_LEN_SHIFT,
	    MT_MDP_DCR1_MAX_RX_LEN_MASK);

	/* enable hardware de-agg */
	mwx_set(sc, MT_MDP_DCR0, MT_MDP_DCR0_DAMSDU_EN);
#if 0
	/* not enabled since our stack does not handle 802.3 frames */
	/* enable hardware rx header translation */
	mwx_set(sc, MT_MDP_DCR0, MT_MDP_DCR0_RX_HDR_TRANS_EN);
#endif

	for (i = 0; i < MWX_WTBL_SIZE; i++)
		mwx_mac_wtbl_update(sc, i);

	mwx_mac_init_band(sc, 0);
	mwx_mac_init_band(sc, 1);

	sc->sc_rxfilter = mwx_read(sc, MT_WF_RFCR(0));
}

int
mwx_mcu_patch_sem_ctrl(struct mwx_softc *sc, int semget)
{
#define	PATCH_SEM_RELEASE		0
#define PATCH_SEM_GET			1
#define	PATCH_NOT_DL_SEM_FAIL		0
#define	PATCH_IS_DL			1
#define	PATCH_NOT_DL_SEM_SUCCESS	2
#define	PATCH_REL_SEM_SUCCESS		3

	uint32_t op = semget ? PATCH_SEM_GET : PATCH_SEM_RELEASE;
	struct {
		uint32_t op;
	} req = {
		.op = htole32(op),
	};
	int rv, seq;
	uint32_t sem;

	rv = mwx_mcu_send_msg(sc, MCU_CMD_PATCH_SEM_CONTROL,
	    &req, sizeof(req), &seq);
	if (rv != 0)
		return rv;

	rv = mwx_mcu_wait_resp_int(sc, MCU_CMD_PATCH_SEM_CONTROL, seq, &sem);
	if (rv != 0)
		return rv;

	if (semget) {
		switch (sem) {
		case PATCH_IS_DL:
			return -1;
		case PATCH_NOT_DL_SEM_SUCCESS:
			return 0;
		default:
			DPRINTF("%s: failed to %s patch semaphore\n",
			    DEVNAME(sc), "get");
			return EAGAIN;
		}
	} else {
		switch (sem) {
		case PATCH_REL_SEM_SUCCESS:
			return 0;
		default:
			DPRINTF("%s: failed to %s patch semaphore\n",
			    DEVNAME(sc), "release");
			return EAGAIN;
		}
	}
}

int
mwx_mcu_init_download(struct mwx_softc *sc, uint32_t addr,
    uint32_t len, uint32_t mode)
{
	struct {
		uint32_t addr;
		uint32_t len;
		uint32_t mode;
	} req = {
		.addr = htole32(addr),
		.len = htole32(len),
		.mode = htole32(mode),
	};
	int cmd;

	if (addr == 0x200000 || addr == 0x900000)
		cmd = MCU_CMD_PATCH_START_REQ;
	else
		cmd = MCU_CMD_TARGET_ADDRESS_LEN_REQ;

	return mwx_mcu_send_wait(sc, cmd, &req, sizeof(req));
}

int
mwx_mcu_send_firmware(struct mwx_softc *sc, int cmd, u_char *data,
    size_t len, size_t max_len)
{
	size_t cur_len;
	int rv, retry;

	while (len > 0) {
		cur_len = len;
		if (cur_len > max_len)
			cur_len = max_len;

		/* mbuf allocation is M_DONTWAIT; let the pool refill. */
		for (retry = 0; ; retry++) {
			rv = mwx_mcu_send_msg(sc, cmd, data, cur_len, NULL);
			if (rv != ENOMEM || retry >= 50)
				break;
			mwx_enomem_retries++;
			mwx_dma_tx_cleanup(sc, &sc->sc_txfwdlq);
			IOSleep(2);
		}
		if (rv != 0)
			return rv;

		data += cur_len;
		len -= cur_len;

		mwx_dma_tx_cleanup(sc, &sc->sc_txfwdlq);
	}

	return 0;
}

int
mwx_mcu_start_patch(struct mwx_softc *sc)
{
	struct {
		uint8_t check_crc;
		uint8_t reserved[3];
	} req = {
		.check_crc = 0,
	};

	return mwx_mcu_send_wait(sc, MCU_CMD_PATCH_FINISH_REQ, &req,
	    sizeof(req));
}

int
mwx_mcu_start_firmware(struct mwx_softc *sc, uint32_t addr, uint32_t option)
{
	struct {
		uint32_t option;
		uint32_t addr;
	} req = {
		.option = htole32(option),
		.addr = htole32(addr),
	};

	return mwx_mcu_send_wait(sc, MCU_CMD_FW_START_REQ, &req, sizeof(req));
}

int
mt7921_mcu_get_nic_capability(struct mwx_softc *sc)
{
	struct mt76_connac_cap_hdr {
		uint16_t	n_elements;
		uint16_t	pad;
	} __packed *hdr;
	struct tlv_hdr {
		uint32_t	type;
		uint32_t	len;
	} __packed *tlv;
	struct mt76_connac_phy_cap {
		uint8_t		ht;
		uint8_t		vht;
		uint8_t		_5g;
		uint8_t		max_bw;
		uint8_t		nss;
		uint8_t		dbdc;
		uint8_t		tx_ldpc;
		uint8_t		rx_ldpc;
		uint8_t		tx_stbc;
		uint8_t		rx_stbc;
		uint8_t		hw_path;
		uint8_t		he;
	} __packed *cap;
	struct mbuf *m;
	int rv, seq, count, i;

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_GET_NIC_CAPAB, NULL, 0, &seq);
	if (rv != 0)
		return rv;

	rv = mwx_mcu_wait_resp_msg(sc, MCU_CE_CMD_GET_NIC_CAPAB, seq, &m);
	if (rv != 0)
		return rv;

	/* the message was already pulled up by mwx_mcu_rx_event() */
	if (mbuf_len(m) < sizeof(*hdr)) {
		printf("%s: GET_NIC_CAPAB response size error\n", DEVNAME(sc));
		m_freem(m);
		return EINVAL;
	}
	hdr = mtod(m, struct mt76_connac_cap_hdr *);
	count = le16toh(hdr->n_elements);
	m_adj(m, sizeof(*hdr));

	for (i = 0; i < count; i++) {
		uint32_t type, len;

		if (mbuf_len(m) < sizeof(*tlv)) {
			printf("%s: GET_NIC_CAPAB tlv size error\n",
			    DEVNAME(sc));
			m_freem(m);
			return EINVAL;
		}

		tlv = mtod(m, struct tlv_hdr *);
		type = le32toh(tlv->type);
		len = le32toh(tlv->len);
		m_adj(m, sizeof(*tlv));

		if (mbuf_len(m) < len)
			break;
		switch (type) {
		case MT_NIC_CAP_6G:
			/* TODO 6GHZ SUPPORT */
			sc->sc_capa.has_6ghz = /* XXX 1 */ 0;
			break;
		case MT_NIC_CAP_MAC_ADDR:
			if (len < ETHER_ADDR_LEN)
				break;
			memcpy(sc->sc_lladdr, mtod(m, caddr_t), ETHER_ADDR_LEN);
			break;
		case MT_NIC_CAP_PHY:
			if (len < sizeof(*cap))
				break;
			cap = mtod(m, struct mt76_connac_phy_cap *);

			/* MtkWireless: the chip has at most 4 chains; a larger
			 * value from firmware would overflow the shift. */
			sc->sc_capa.num_streams = MIN(cap->nss, 4);
			sc->sc_capa.antenna_mask =
			    (1U << sc->sc_capa.num_streams) - 1;
			sc->sc_capa.has_2ghz = cap->hw_path & 0x01;
			sc->sc_capa.has_5ghz = cap->hw_path & 0x02;
			break;
		case MT_NIC_CAP_TX_RESOURCE:
			/* unused on PCIe devices */
			break;
		}
		m_adj(m, len);
	}

	printf("%s: address %s\n", DEVNAME(sc), ether_sprintf(sc->sc_lladdr));

	m_freem(m);
	return 0;
}

int
mt7921_mcu_fw_log_2_host(struct mwx_softc *sc, uint8_t ctrl)
{
	struct {
		uint8_t	ctrl;
		uint8_t	pad[3];
	} req = {
		.ctrl = ctrl,
	};

	return mwx_mcu_send_msg(sc, MCU_CE_CMD_FWLOG_2_HOST, &req,
	    sizeof(req), NULL);
}

int
mt7921_mcu_set_eeprom(struct mwx_softc *sc)
{
	struct req_hdr {
		uint8_t	buffer_mode;
		uint8_t	format;
		uint8_t pad[2];
	} req = {
		.buffer_mode = EE_MODE_EFUSE,
		.format = EE_FORMAT_WHOLE,
	};

	return mwx_mcu_send_wait(sc, MCU_EXT_CMD_EFUSE_BUFFER_MODE, &req,
	    sizeof(req));
}

int
mt7921_mcu_set_rts_thresh(struct mwx_softc *sc, uint32_t val, uint8_t band)
{
	struct {
		uint8_t		prot_idx;
		uint8_t		band;
		uint8_t		rsv[2];
		uint32_t	len_thresh;
		uint32_t	pkt_thresh;
	} __packed req = {
		.prot_idx = 1,
		.band = band,
		.len_thresh = htole32(val),
		.pkt_thresh = htole32(0x2),
	};

	return mwx_mcu_send_wait(sc, MCU_EXT_CMD_PROTECT_CTRL, &req,
	    sizeof(req));
}

int
mwx_mcu_set_deep_sleep(struct mwx_softc *sc, int ena)
{
	struct mwx_connac_config req = {
		.resp_type = 0,
	};

	DPRINTF("%s: %s deep sleep\n", DEVNAME(sc), ena ? "enable" : "disable");
	snprintf((char *)req.data, sizeof(req.data), "KeepFullPwr %d", !ena);
	return mwx_mcu_send_msg(sc, MCU_CE_CMD_CHIP_CONFIG, &req,
	     sizeof(req), NULL);
}

void
mt7921_mcu_low_power_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct mt7921_mcu_lp_event {
		uint8_t state;
		uint8_t reserved[3];
	} __packed *event;

	if (mbuf_len(m) < sizeof(*event))
		return;
	event = mtod(m, struct mt7921_mcu_lp_event *);
	DPRINTF("%s: low power event state %d\n", DEVNAME(sc), event->state);
}

/*
 * The firmware reports that beacons from the AP stopped arriving. Like
 * mac80211 and iwm(4), probe the AP before giving up: a probe response or
 * beacon clears ic_mgt_timer, otherwise ieee80211_watchdog() moves us to
 * SCAN and the link goes down so macOS can rejoin.
 */
void
mt7921_mcu_beacon_loss_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mt7921_mcu_beacon_loss_event {
		uint8_t bss_idx;
		uint8_t reason;
		uint8_t pad[2];
	} __packed *event;

	if (mbuf_len(m) < sizeof(*event))
		return;
	event = mtod(m, struct mt7921_mcu_beacon_loss_event *);
	MWX_DEV_LOG(sc, "beacon loss bss_idx=%u reason=%u state=%d mgt_timer=%d\n",
	    event->bss_idx, event->reason, ic->ic_state, ic->ic_mgt_timer);

	if (sc->sc_resetting || ic->ic_opmode != IEEE80211_M_STA ||
	    ic->ic_state != IEEE80211_S_RUN ||
	    (ic->ic_flags & IEEE80211_F_BGSCAN) || ic->ic_mgt_timer != 0)
		return;
	IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_PROBE_REQ, 0);
}

void
mt7921_mcu_tx_done_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct mt7921_mcu_tx_done_event {
		uint8_t		pid;
		uint8_t		status;
		uint16_t	seq;
		uint8_t		wlan_idx;
		uint8_t		tx_cnt;
		uint16_t	tx_rate;
		uint8_t		flag;
		uint8_t		tid;
		uint8_t		rsp_rate;
		uint8_t		mcs;
		uint8_t		bw;
		uint8_t		tx_pwr;
		uint8_t		reason;
		uint8_t		rsv0[1];
		uint32_t	delay;
		uint32_t	timestamp;
		uint32_t	applied_flag;
		uint8_t		txs[28];
		uint8_t		rsv1[32];
	} __packed *event;

	if (mbuf_len(m) < sizeof(*event))
		return;
	event = mtod(m, struct mt7921_mcu_tx_done_event *);
	uint32_t txs[8]={};
	memcpy(txs,event->txs,sizeof(event->txs));
	mt7921_mac_add_txs(sc, txs);
}

/*
 * TX status of a data frame: record the rate the firmware sent it at
 * (mt76_connac2_mac_fill_txs) so getRATE/getMCS report the real link.
 * TXS does not carry the guard interval.
 */
void
mt7921_mac_add_txs(struct mwx_softc *sc, const uint32_t *txs_data)
{
	if (sc->sc_awdl_tx_status.observe(reinterpret_cast<const uint8_t *>(txs_data),32))
		return;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	uint32_t txs = le32toh(txs_data[0]);
	uint32_t pid = le32toh(txs_data[3]) >> 24;
	uint32_t rate, mode, mcs, nss;

	if (((txs >> 23) & 3) > 1 || pid < MT_PACKET_ID_FIRST || ni == NULL ||
	    ((le32toh(txs_data[2]) >> 16) & 0x3ff) != ((struct mwx_node *)ni)->wcid ||
	    ic->ic_state != IEEE80211_S_RUN ||
	    (txs & 0x00070000) != 0)		/* MT_TXS0_ACK_ERROR_MASK */
		return;
	rate = txs & 0x3fff;			/* MT_TXS0_TX_RATE */
	mode = (rate & MT_TX_RATE_MODE_MASK) >> MT_TX_RATE_MODE_SHIFT;
	mcs = rate & MT_TX_RATE_IDX_MASK;
	nss = ((rate & MT_TX_RATE_NSS_MASK) >> MT_TX_RATE_NSS_SHIFT) + 1;
	if ((rate & MT_TX_RATE_STBC) && nss > 1)
		nss >>= 1;
	switch (mode) {
	case MT_PHY_TYPE_HT:
	case MT_PHY_TYPE_HT_GF:
		ni->ni_txmcs = (mcs & 7) + (nss - 1) * 8;
		break;
	case MT_PHY_TYPE_VHT:
		ni->ni_txmcs = mcs;
		ni->ni_vht_ss = nss;
		break;
	case MT_PHY_TYPE_HE_SU:
	case MT_PHY_TYPE_HE_EXT_SU:
	case MT_PHY_TYPE_HE_TB:
	case MT_PHY_TYPE_HE_MU:
		/* HE uses bits 3:0; bit 4 is DCM, bit 5 SU extended tone. */
		mcs &= 0xf;
		if (mcs > 11)
			return;
		ni->ni_txmcs = mcs;
		ni->ni_vht_ss = nss;
		break;
	default:
		return;
	}
	if ((sc->sc_txs_count++ & 0x3ff) == 0)
		MWX_DEV_LOG(sc, "tx rate mode=%u mcs=%u nss=%u bw=%u\n",
		    mode, mcs, nss, (txs >> 29) & 0x3);
}

int
mt7921_mcu_hw_scan(struct mwx_softc *sc, int bgscan)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c;
	struct mt76_connac_hw_scan_req	*req;
	struct mbuf *m;
	int n_ssids = 0;
	int rv;
	uint8_t nchan, n_2g = 0, n_5g = 0;
	uint16_t dwell;

    // One radio cannot scan while honoring an AWDL reservation. Keep the
    // logical scan pending and finish it when AWDL releases the radio. Do
    // not repeatedly cancel/end/restart a foreground scan each timer tick.
    // MtkWireless: unsolicited scans only while associated. A WCL request
    // must finish even with AWDL enabled so airportd gets fresh results.
    // Unassociated, the scan is how
    // airportd finds the AP to join; suspending it until AWDL went idle
    // failed every join (-3905) while AirDrop was open. Release the AWDL
    // ROC instead; awdlReserve waits for the scan.
    bool requestedScan=false;
    requestedScan=ic->ic_wcl_scan_requested ||
        (ic->ic_wcl_join_requested && sc->sc_ns_state==IEEE80211_S_SCAN);
    if(sc->sc_awdl_enabled && sc->sc_ic.ic_state==IEEE80211_S_RUN && !requestedScan) {
        sc->sc_awdl_scan_suspended=true;
        return 0;
    }
    sc->sc_awdl_scan_suspended=false;
    if(sc->sc_awdl_enabled) {
        mwx_awdl_abort(sc);
        ++sc->sc_awdl_scan_preempted;
    }

	m = mwx_mcu_alloc_msg(sizeof(*req));
	if (m == NULL)
		return ENOMEM;
	req = mtod(m, struct mt76_connac_hw_scan_req *);
	/* The mbuf payload is not initialized by allocation. */
	memset(req, 0, sizeof(*req));

	sc->sc_flags |= MWX_FLAG_SCANNING;
	ic->ic_wcl_scan_inflight = true;
	sc->sc_scan_seq_num = (sc->sc_scan_seq_num + 1) & 0x7f;

	req->seq_num = sc->sc_scan_seq_num /* | sc->sc_band_idx << 7 */;
	req->bss_idx = /* mvif->idx */ 0;
	req->scan_type = /* sreq->n_ssids ? 1 : */ 0;
	req->probe_req_num = /* sreq->n_ssids ? 2 : */ 0;
	req->version = 1;

#ifdef NOTYET
	for (i = 0; i < sreq->n_ssids; i++) {
		if (!sreq->ssids[i].ssid_len)
			continue;
		req->ssids[i].ssid_len = htole32(sreq->ssids[i].ssid_len);
		memcpy(req->ssids[i].ssid, sreq->ssids[i].ssid,
		    sreq->ssids[i].ssid_len);
		n_ssids++;
	}
#endif
	/* MtkWireless: WCL lists carry Instant Hotspot/hidden-network targets.
	 * Firmware v1 has four primary and six extended SSID slots. */
	if (ic->ic_wcl_scan_requested && ic->ic_wcl_scan_active &&
	    ic->ic_wcl_scan_targets.count > 0 &&
	    ic->ic_wcl_scan_targets.count <= MTK_WLAN_SCAN_MAX_SSIDS) {
		for (unsigned i = 0; i < ic->ic_wcl_scan_targets.count; ++i) {
			uint8_t len = ic->ic_wcl_scan_targets.lengths[i];
			if (!len || len > IEEE80211_NWID_LEN)
				continue;
			auto target = n_ssids < 4 ? &req->ssids[n_ssids] :
			    &req->ext_ssids[n_ssids - 4];
			target->ssid_len = htole32(len);
			memcpy(target->ssid, ic->ic_wcl_scan_targets.ssids[i], len);
			++n_ssids;
		}
	}
	/* Keep legacy single-SSID callers working. */
	if (ic->ic_wcl_scan_requested && ic->ic_wcl_scan_active &&
	    n_ssids == 0 &&
	    ic->ic_wcl_scan_ssid_length > 0 &&
	    ic->ic_wcl_scan_ssid_length <= IEEE80211_NWID_LEN) {
		req->ssids[0].ssid_len = htole32(ic->ic_wcl_scan_ssid_length);
		memcpy(req->ssids[0].ssid, ic->ic_wcl_scan_ssid,
		    ic->ic_wcl_scan_ssid_length);
		n_ssids = 1;
	}
	if (n_ssids) {
		req->scan_type = 1;
		req->probe_req_num = 2;
	}

	req->ssid_type = n_ssids ? 0x4 : 0x1;
	req->ssid_type_ext = n_ssids && !ic->ic_wcl_scan_targets.wildcard ? 1 : 0;
	req->ssids_num = MIN(n_ssids, 4);
	req->ext_ssids_num = n_ssids > 4 ? n_ssids - 4 : 0;

	for (nchan = 0, c = &ic->ic_channels[1];
	    c <= &ic->ic_channels[IEEE80211_CHAN_MAX] &&
	    nchan < 64; c++) {
		struct mt76_connac_mcu_scan_channel *chan;
		uint8_t channel_num;

		if (c->ic_flags == 0)
			continue;
		/* Restrict to the channels WCL asked for, if any. */
		if (ic->ic_wcl_scan_requested && ic->ic_wcl_scan_restricted) {
			unsigned int chn = ieee80211_chan2ieee(ic, c);
			if (chn >= 256 || !(ic->ic_wcl_scan_channels[chn / 8] &
			    (1U << (chn % 8))))
				continue;
		}

		if (nchan < 32)
			chan = &req->channels[nchan];
		else
			chan = &req->ext_channels[nchan - 32];

		channel_num = ieee80211_mhz2ieee(c->ic_freq, 0);
		/* TODO IEEE80211_IS_CHAN_6GHZ -> chan->band = 3*/
		if (IEEE80211_IS_CHAN_2GHZ(c)) {
			chan->band = 1;
			n_2g++;
		} else {
			chan->band = 2;
			n_5g++;
		}
		chan->channel_num = channel_num;
		nchan++;
	}

	if (nchan <= 32) {
		req->channels_num = nchan;
	} else {
		req->channels_num = 32;
		req->ext_channels_num = nchan - 32;
	}

	req->channel_type = nchan ? 4 : 0;
	/*
	 * Off-channel time is dead time for the link we are on, so background
	 * scans leave the dwell to the firmware (0) as Linux mt7921 does for
	 * every scan; split scan then returns to the operating channel between
	 * channel groups.  When airportd sets a home/away time, stay within it:
	 * the firmware visits about three channels per trip off channel
	 * (measured: 110 ms dwell gave ~300 ms gaps, 40 ms gave ~135 ms).
	 * Unassociated scans keep the longer fixed dwell.
	 */
	if (!bgscan)
		dwell = 120;
	else if (sc->sc_bgscan_dwell != MWX_BGSCAN_DWELL_AUTO)
		dwell = sc->sc_bgscan_dwell;
	else {
		dwell = 0;
		if (ic->ic_wcl_scan_away_time_ms != 0)
			dwell = MIN(MAX(ic->ic_wcl_scan_away_time_ms / 3, 20), 120);
	}
	req->timeout_value = htole16(nchan * dwell);
	req->channel_min_dwell_time = htole16(dwell);
	req->channel_dwell_time = htole16(dwell);


#ifdef NOTYET
	if (sreq->ie_len > 0) {
		memcpy(req->ies, sreq->ie, sreq->ie_len);
		req->ies_len = htole16(sreq->ie_len);
	}
#endif

	req->scan_func |= SCAN_FUNC_SPLIT_SCAN;

	/* wildcard BSSID */
	memset(req->bssid, 0xff, ETHER_ADDR_LEN);
#ifdef NOTYET
	if (sreq->flags & NL80211_SCAN_FLAG_RANDOM_ADDR) {
		get_random_mask_addr(req->random_mac, sreq->mac_addr,
		    sreq->mac_addr_mask);
		req->scan_func |= SCAN_FUNC_RANDOM_MAC;
	}
#endif

	MWX_DEV_LOG(sc,
	    "scan request seq=%u channels=%u 2g=%u 5g=%u primary=%u ext=%u timeout=%u dwell=%u min_dwell=%u scan_func=0x%08x ssid_type=0x%x ssids=%u ext_ssids=%u ssid_type_ext=%u\n",
	    sc->sc_scan_seq_num, nchan, n_2g, n_5g, req->channels_num,
	    req->ext_channels_num, le16toh(req->timeout_value),
	    le16toh(req->channel_dwell_time),
	    le16toh(req->channel_min_dwell_time),
	    le32toh(req->scan_func), req->ssid_type, req->ssids_num,
	    req->ext_ssids_num, req->ssid_type_ext);

	rv = mwx_mcu_send_mbuf(sc, MCU_CE_CMD_START_HW_SCAN, m, NULL);
	if (rv != 0) {
		sc->sc_flags &= ~(MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN);
		ic->ic_wcl_scan_inflight = false;
	}

	return rv;
}

int
mt7921_mcu_hw_scan_cancel(struct mwx_softc *sc)
{
	struct {
		uint8_t		seq_num;
		uint8_t		is_ext_channel;
		uint8_t		rsv[2];
	} __packed req = {
		.seq_num = sc->sc_scan_seq_num,
	};
	int rv;

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_CANCEL_HW_SCAN, &req,
	    sizeof(req), NULL);
	if (rv == 0) {
		sc->sc_flags &= ~(MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN);
		sc->sc_ic.ic_wcl_scan_inflight = false;
	}
	return rv;
}

void
mwx_scan_timeo(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;

	if ((sc->sc_flags & MWX_FLAG_SCANNING) == 0)
		return;
	sc->sc_scan_watchdog_fired++;
	MWX_DEV_LOG(sc, "scan watchdog: no SCAN_DONE, ending scan (fired=%u)\n",
	    sc->sc_scan_watchdog_fired);
	mt7921_mcu_hw_scan_cancel(sc);
	task_add(systq, &sc->sc_scan_task);
}

void
mwx_dump_dma_state(struct mwx_softc *sc, const char *why)
{
	struct mwx_queue *q = &sc->sc_txq;

	MWX_DEV_LOG(sc, "dma state (%s): glo_cfg=0x%08x dummy_cr=0x%08x mcu_cmd=0x%08x "
	    "int_sta=0x%08x int_ena=0x%08x lpctl=0x%08x\n", why,
	    mwx_read(sc, MT_WFDMA0_GLO_CFG), mwx_read(sc, MT_WFDMA_DUMMY_CR),
	    mwx_read(sc, MT_MCU_CMD), mwx_read(sc, MT_WFDMA0_HOST_INT_STA),
	    mwx_read(sc, MT_WFDMA0_HOST_INT_ENA), mwx_read(sc, MT_CONN_ON_LPCTL));
	MWX_DEV_LOG(sc, "dma state (%s): txq base=0x%08x cnt=%u cpu=%u dma=%u cons=%u "
	    "txmcu cpu=%u dma=%u mcu_cmd_irqs=%u last_mcu_cmd=0x%08x\n", why,
	    mwx_read(sc, q->mq_regbase), mwx_read(sc, q->mq_regbase + 4),
	    mwx_read(sc, q->mq_regbase + MT_DMA_CPU_IDX),
	    mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX), q->mq_cons,
	    mwx_read(sc, sc->sc_txmcuq.mq_regbase + MT_DMA_CPU_IDX),
	    mwx_read(sc, sc->sc_txmcuq.mq_regbase + MT_DMA_DMA_IDX),
	    sc->sc_mcu_cmd_irqs, sc->sc_last_mcu_cmd);

	/*
	 * The firmware stops TX DMA on a descriptor it does not accept; log
	 * the one it stopped at and the one before it so the frame is known.
	 */
	if (q->mq_desc == NULL || q->mq_data == NULL || q->mq_count == 0)
		return;
	uint32_t dma = mwx_read(sc, q->mq_regbase + MT_DMA_DMA_IDX) % q->mq_count;
	for (int back = 1; back >= 0; back--) {
		uint32_t idx = (dma + q->mq_count - back) % q->mq_count;
		struct mwx_desc *desc = &q->mq_desc[idx];
		struct mwx_txwi *mt = q->mq_data[idx].md_txwi;
		struct mt76_txwi *w;

		MWX_DEV_LOG(sc, "dma state (%s): desc[%u] buf0=0x%08x ctrl=0x%08x "
		    "buf1=0x%08x info=0x%08x\n", why, idx, le32toh(desc->buf0),
		    le32toh(desc->ctrl), le32toh(desc->buf1), le32toh(desc->info));
		if (mt == NULL || (w = mt->mt_desc) == NULL)
			continue;
		MWX_DEV_LOG(sc, "dma state (%s): txwi[%u] %08x %08x %08x %08x %08x "
		    "%08x %08x %08x txp %08x/%u/%u/%08x %08x/%u/%u/%08x mbuf=%p\n",
		    why, mt->mt_idx, le32toh(w->txwi[0]), le32toh(w->txwi[1]),
		    le32toh(w->txwi[2]), le32toh(w->txwi[3]), le32toh(w->txwi[4]),
		    le32toh(w->txwi[5]), le32toh(w->txwi[6]), le32toh(w->txwi[7]),
		    le32toh(w->ptr[0].buf0), le16toh(w->ptr[0].len0),
		    le16toh(w->ptr[0].len1), le32toh(w->ptr[0].buf1),
		    le32toh(w->ptr[1].buf0), le16toh(w->ptr[1].len0),
		    le16toh(w->ptr[1].len1), le32toh(w->ptr[1].buf1),
		    (void *)mt->mt_mbuf);
	}
}

void
mwx_dma_timeo(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;

	if ((ifp->if_flags & IFF_RUNNING) == 0 || sc->sc_resetting ||
	    sc->sc_detaching)
		return;

	if ((mwx_read(sc, MT_WFDMA0_GLO_CFG) & MT_WFDMA0_GLO_CFG_TX_DMA_EN) == 0 &&
	    !sc->sc_dma_stall_logged) {
		sc->sc_dma_stall_logged = 1;
		sc->sc_dma_stalls++;
		mwx_dump_dma_state(sc, "TX DMA disabled");
		if (sc->sc_knobs & MWX_KNOB_AUTORESET) {
			sc->sc_resetting = 1;
			task_add(systq, &sc->sc_fullreset_task);
			return;
		}
	}
	timeout_add_sec(&sc->sc_dma_to, 2);
}

/* Reset WFSYS and reload firmware; the only way out of a WFDMA stop. */
void
mwx_fullreset_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	int rv, s = splnet();

	sc->sc_full_resets++;
	MWX_DEV_LOG(sc, "full reset %u start\n", sc->sc_full_resets);
	mwx_dump_dma_state(sc, "before reset");
	sc->sc_resetting = 1;
	if (ifp->if_flags & IFF_RUNNING)
		mwx_stop(ifp);
	rv = mwx_wfsys_reset(sc);
	MWX_DEV_LOG(sc, "full reset wfsys rv=%d\n", rv);
	sc->sc_fw_restart = 1;
	/* Channel/media setup from preinit stays valid; only redo hardware. */
	rv = mwx_init_hardware(sc);
	MWX_DEV_LOG(sc, "full reset hardware init rv=%d\n", rv);
	if(rv==0) {
        // Reloading firmware destroys every old station, including entries
        // whose disconnect response failed before the hardware reset.
        memset(sc->sc_awdl_station_pending,0,sizeof(sc->sc_awdl_station_pending));
        memset(sc->sc_awdl_peer_used,0,sizeof(sc->sc_awdl_peer_used));
        sc->sc_awdl_configured=false;
    }
	sc->sc_resetting = 0;
	if (rv == 0 && (ifp->if_flags & IFF_UP))
		rv = mwx_init(ifp);
	MWX_DEV_LOG(sc, "full reset done init rv=%d\n", rv);
	splx(s);
}

void
mwx_end_scan_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ieee80211com *ic = &sc->sc_ic;
	int s;

	s = splnet();
	ieee80211_end_scan(&ic->ic_if);
	splx(s);
}

void
mt7921_mcu_scan_event(struct mwx_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mt76_connac_hw_scan_done *event;
	uint32_t probe_sum = 0, mdrdy_sum = 0;
	int i, nodes_before;

    // A canceled scan's delayed completion must not cancel a newer scan or
    // restart scanning while AWDL owns the radio.
    if(sc->sc_awdl_scan_suspended || !(sc->sc_flags & MWX_FLAG_SCANNING) ||
       mbuf_len(m)<sizeof(*event)) return;
    event=mtod(m,struct mt76_connac_hw_scan_done *);
    if(event->seq_num!=sc->sc_scan_seq_num) return;

	sc->sc_scan_done_count++;
	timeout_del(&sc->sc_scan_to);
	nodes_before = mwx_count_nodes(ic);
	sc->sc_scan_last_ieee80211_nodes = nodes_before;

	if (mbuf_len(m) >= sizeof(*event)) {
		event = mtod(m, struct mt76_connac_hw_scan_done *);
		for (i = 0; i < MT76_HW_SCAN_DONE_MAX_CHANNEL_NUM; i++) {
			probe_sum += event->beacon_probe_num[i];
			mdrdy_sum += event->mdrdy_count[i];
		}
		MWX_DEV_LOG(sc,
		    "scan done #%u seq=%u complete_ch=%u state=%u version=%u beacon_scan=%u probe_sum=%u mdrdy_sum=%u beacon_2g=%u beacon_5g=%u nodes_before=%d rx_norm=%u rx_fail=%u\n",
		    sc->sc_scan_done_count, event->seq_num,
		    event->complete_channel_num, event->current_state,
		    event->version, le32toh(event->beacon_scan_num),
		    probe_sum, mdrdy_sum, le32toh(event->beacon_2g_num),
		    le32toh(event->beacon_5g_num), nodes_before,
		    sc->sc_rx_pkt_normal, sc->sc_rx_fill_fail);
	} else {
		MWX_DEV_LOG(sc,
		    "scan done #%u short event len=%zu nodes_before=%d rx_norm=%u rx_fail=%u\n",
		    sc->sc_scan_done_count, mbuf_len(m), nodes_before,
		    sc->sc_rx_pkt_normal, sc->sc_rx_fill_fail);
	}

	if (mt7921_mcu_hw_scan_cancel(sc) != 0)
		return;
	task_add(systq, &sc->sc_scan_task);
}

int
mt7921_mcu_set_mac_enable(struct mwx_softc *sc, int band, int enable)
{
	struct {
		uint8_t		enable;
		uint8_t		band;
		uint8_t		rsv[2];
	} __packed req = {
		.enable = (uint8_t)enable,
		.band = (uint8_t)band,
	};

	return mwx_mcu_send_wait(sc, MCU_EXT_CMD_MAC_INIT_CTRL, &req,
	    sizeof(req));
}

int
mt7921_mcu_set_channel_domain(struct mwx_softc *sc)
{
	struct {
		uint8_t		alpha2[4]; /* regulatory_request.alpha2 */
		uint8_t		bw_2g;	/* BW_20_40M		0
					 * BW_20M		1
					 * BW_20_40_80M		2
					 * BW_20_40_80_160M	3
					 * BW_20_40_80_8080M	4
					 */
		uint8_t		bw_5g;
		uint8_t		bw_6g;
		uint8_t		pad;
		uint8_t		n_2ch;
		uint8_t		n_5ch;
		uint8_t		n_6ch;
		uint8_t		pad2;
	} __packed *hdr;
	struct {
		uint16_t	hw_value;
		uint16_t	pad;
		uint32_t	flags;
	} __packed *channel;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *chan;
	struct mbuf *m;
	int i, len, rv;
	int n_2ch = 0, n_5ch = 0, n_6ch = 0;

	len = sizeof(*hdr) + IEEE80211_CHAN_MAX * sizeof(channel);
	m = mwx_mcu_alloc_msg(len);
	if (m == NULL)
		return ENOMEM;
	hdr = (decltype(hdr))mbuf_data(m);

	hdr->alpha2[0] = '0';
	hdr->alpha2[1] = '0';

	channel = (decltype(channel))(hdr + 1);

	hdr->bw_2g = 0;	/* BW_20_40M */
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		chan = &ic->ic_channels[i];
		if (!IEEE80211_IS_CHAN_2GHZ(chan))
			continue;

		channel->hw_value = htole16(ieee80211_chan2ieee(ic, chan));
		channel->flags = htole32(0);	/* XXX */

		channel++;
		n_2ch++;
	}
	hdr->bw_5g = 3; /* BW_20_40_80_160M */
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		chan = &ic->ic_channels[i];
		if (!IEEE80211_IS_CHAN_5GHZ(chan))
			continue;

		channel->hw_value = htole16(ieee80211_chan2ieee(ic, chan));
		/* MtkWireless: Linux IEEE80211_CHAN_RADAR, as mt76 passes it;
		 * the firmware scans these channels passively. */
		channel->flags = htole32((chan->ic_flags & IEEE80211_CHAN_DFS) ?
		    MWX_LINUX_CHAN_RADAR : 0);

		channel++;
		n_5ch++;
	}
#ifdef NOTYET
	/* 6GHz handling */
	hdr->bw_6g = 3; /* BW_20_40_80_160M */
	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		chan = &ic->ic_channels[i];
		if (!IEEE80211_IS_CHAN_6GHZ(chan))
			continue;

		channel->hw_value = htole16(ieee80211_chan2ieee(ic, chan));
		channel->flags = htole32(0);	/* XXX */

		channel++;
		n_6ch++;
	}
#endif

	memcpy(hdr->alpha2, sc->sc_alpha2, sizeof(sc->sc_alpha2));
	hdr->n_2ch = n_2ch;
	hdr->n_5ch = n_5ch;
	hdr->n_6ch = n_6ch;

	mwx_mcu_set_len(m, channel);
	rv = mwx_mcu_send_mbuf(sc, MCU_CE_CMD_SET_CHAN_DOMAIN, m, NULL);
	return rv;
}

uint8_t
mt7921_mcu_chan_bw(struct ieee80211_node *ni)
{
	/*
	 * Use the width negotiated with the AP, not the channel's
	 * capability flags. CMD_CBW_5MHZ and CMD_CBW_10MHZ are not used.
	 */
	switch (ni->ni_chw) {
	case IEEE80211_CHAN_WIDTH_40:
		return CMD_CBW_40MHZ;
	case IEEE80211_CHAN_WIDTH_80:
		return CMD_CBW_80MHZ;
	case IEEE80211_CHAN_WIDTH_80P80:
		return CMD_CBW_8080MHZ;
	case IEEE80211_CHAN_WIDTH_160:
		return CMD_CBW_160MHZ;
	default:
		return CMD_CBW_20MHZ;
	}
}

/*
 * Record the BSS width negotiated in the (re)association response.
 * Until then (scan, auth, assoc) the radio stays on the primary 20 MHz.
 */
void
mwx_set_bss_bw(struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct ieee80211_channel *c;

	sc->sc_bss_bw = CMD_CBW_20MHZ;
	sc->sc_bss_center = 0;
	if (ni == NULL || ni->ni_chan == NULL ||
	    ni->ni_chan == IEEE80211_CHAN_ANYC)
		return;
	if ((ni->ni_flags & (IEEE80211_NODE_HT | IEEE80211_NODE_VHT)) == 0)
		return;
	c = ni->ni_chan;
	sc->sc_bss_bw = mt7921_mcu_chan_bw(ni);
	if (sc->sc_bss_bw == CMD_CBW_20MHZ || c->ic_center_freq1 == 0) {
		sc->sc_bss_bw = CMD_CBW_20MHZ;
		return;
	}
	sc->sc_bss_center = ieee80211_mhz2ieee(c->ic_center_freq1, c->ic_flags);
}

int
mt7921_mcu_set_chan_info(struct mwx_softc *sc, int cmd)
{
	struct ieee80211_channel *channel;
	struct {
		uint8_t		control_ch;
		uint8_t		center_ch;
		uint8_t		bw;
		uint8_t		tx_streams_num;
		uint8_t		rx_streams;	/* mask or num */
		uint8_t		switch_reason;
		uint8_t		band_idx;
		uint8_t		center_ch2;	/* for 80+80 only */
		uint16_t	cac_case;
		uint8_t		channel_band;
		uint8_t		rsv0;
		uint32_t	outband_freq;
		uint8_t		txpower_drop;
		uint8_t		ap_bw;
		uint8_t		ap_center_ch;
		uint8_t		rsv1[57];
	} __packed req = {
		.tx_streams_num = sc->sc_capa.num_streams,
		.rx_streams = sc->sc_capa.antenna_mask,
		.band_idx = 0,	/* XXX 0 or 1 */
	};

	if (sc->sc_ic.ic_opmode == IEEE80211_M_STA && sc->sc_ic.ic_bss != NULL)
		channel = sc->sc_ic.ic_bss->ni_chan;
	else
		channel = sc->sc_ic.ic_ibss_chan;

	req.control_ch = ieee80211_mhz2ieee(channel->ic_freq,
		    channel->ic_flags);
	req.center_ch = req.control_ch;
	req.bw = CMD_CBW_20MHZ;
	if (sc->sc_ic.ic_opmode == IEEE80211_M_STA &&
	    sc->sc_ic.ic_bss != NULL && sc->sc_bss_bw != CMD_CBW_20MHZ) {
		req.bw = sc->sc_bss_bw;
		req.center_ch = sc->sc_bss_center;
	}

#ifdef NOTYET
	if (channel->ic_flags & IEEE80211_CHAN_6GHZ)
		req.channel_band = 2;
	else
#endif
	if (channel->ic_flags & IEEE80211_CHAN_5GHZ)
		req.channel_band = 1;
	else
		req.channel_band = 0;

#ifdef NOTYET
	if (dev->mt76.hw->conf.flags & IEEE80211_CONF_OFFCHANNEL)
		req.switch_reason = CH_SWITCH_SCAN_BYPASS_DPD;
	else if ((chandef->chan->flags & IEEE80211_CHAN_RADAR) &&
		 chandef->chan->dfs_state != NL80211_DFS_AVAILABLE)
		req.switch_reason = CH_SWITCH_DFS;
	else
#endif
		req.switch_reason = CH_SWITCH_NORMAL;

	if (cmd == MCU_EXT_CMD_CHANNEL_SWITCH)
		req.rx_streams = sc->sc_capa.num_streams;

#ifdef NOTYET
	if (chandef->width == NL80211_CHAN_WIDTH_80P80) {
		int freq2 = chandef->center_freq2;
		req.center_ch2 = ieee80211_frequency_to_channel(freq2);
	}
#endif

	return mwx_mcu_send_wait(sc, cmd, &req, sizeof(req));
}

/* hardcoded version of what linux does */
void
mt7921_mcu_build_sku(struct mwx_softc *sc, int band, int8_t *sku)
{
	int max_power = 127;
	int i, offset = 4;

	memset(sku, max_power, MT_SKU_POWER_LIMIT);

	if (band == MT_TX_PWR_BAND_2GHZ) {
		/* cck */
		memset(sku, 0x28, 4);
	}

	/* ofdm */
	memset(sku + offset, 0x28, 8);
	offset += 8;

	/* ht */
	for (i = 0; i < 2; i++) {
		memset(sku + offset, 0x28, 8);
		offset += 8;
	}
	sku[offset++] = 0x28;

	/* vht */
	for (i = 0; i < 4; i++) {
		/* this only sets 10 out of 12 bytes on purpose */
		memset(sku + offset, 0x28, 10);
		offset += 12;
	}

	/* he */
	for (i = 0; i < 7; i++) {
		memset(sku + offset, 0x28, 12);
		offset += 12;
	}
}

int
mt7921_mcu_rate_txpower_band(struct mwx_softc *sc, int band,
    const uint8_t *chans, int n_chans, int is_last)
{
	struct mt76_connac_sku_tlv *sku_tlv;
	struct mt76_connac_tx_power_limit_tlv *tx_power_tlv;
	struct mbuf *m;
	int batch_size = 8;
	const int len = sizeof(*tx_power_tlv) + batch_size * sizeof(*sku_tlv);
	int rv = 0, idx, j;

	for (idx = 0; idx < n_chans; ) {
		int num_ch = batch_size;

		m = mwx_mcu_alloc_msg(len);
		if (m == NULL)
			return ENOMEM;
		tx_power_tlv = mtod(m, struct mt76_connac_tx_power_limit_tlv *);
		tx_power_tlv->n_chan = num_ch;
		tx_power_tlv->band = band;
		memcpy(tx_power_tlv->alpha2, sc->sc_alpha2,
		    sizeof(sc->sc_alpha2));

		if (n_chans - idx < batch_size) {
			num_ch = n_chans - idx;
			if (is_last)
				tx_power_tlv->last_msg = 1;
		}

		sku_tlv = (struct mt76_connac_sku_tlv *)(tx_power_tlv + 1);

		for (j = 0; j < num_ch; j++, idx++) {
			sku_tlv->channel = chans[idx];
			mt7921_mcu_build_sku(sc, band, sku_tlv->pwr_limit);
			sku_tlv++;
		}

		mwx_mcu_set_len(m, sku_tlv);
		rv = mwx_mcu_send_mbuf(sc, MCU_CE_CMD_SET_RATE_TX_POWER, m,
		    NULL);
		if (rv != 0)
			break;
	}

	return rv;
}

int
mt7921_mcu_set_rate_txpower(struct mwx_softc *sc)
{
	static const uint8_t chan_list_2ghz[] = {
	    1, 2,  3,  4,  5,  6,  7,
	    8, 9, 10, 11, 12, 13, 14
	};
	static const uint8_t chan_list_5ghz[] = {
	    36,  38,  40,  42,  44,  46,  48,
	    50,  52,  54,  56,  58,  60,  62,
	    64, 100, 102, 104, 106, 108, 110,
	    112, 114, 116, 118, 120, 122, 124,
	    126, 128, 132, 134, 136, 138, 140,
	    142, 144, 149, 151, 153, 155, 157,
	    159, 161, 165
	};
	static const uint8_t chan_list_6ghz[] = {
	    1,   3,   5,   7,   9,  11,  13,
	    15,  17,  19,  21,  23,  25,  27,
	    29,  33,  35,  37,  39,  41,  43,
	    45,  47,  49,  51,  53,  55,  57,
	    59,  61,  65,  67,  69,  71,  73,
	    75,  77,  79,  81,  83,  85,  87,
	    89,  91,  93,  97,  99, 101, 103,
	    105, 107, 109, 111, 113, 115, 117,
	    119, 121, 123, 125, 129, 131, 133,
	    135, 137, 139, 141, 143, 145, 147,
	    149, 151, 153, 155, 157, 161, 163,
	    165, 167, 169, 171, 173, 175, 177,
	    179, 181, 183, 185, 187, 189, 193,
	    195, 197, 199, 201, 203, 205, 207,
	    209, 211, 213, 215, 217, 219, 221,
	    225, 227, 229, 233
	};
	int rv = 0;

	if (sc->sc_capa.has_2ghz)
		rv = mt7921_mcu_rate_txpower_band(sc, MT_TX_PWR_BAND_2GHZ,
		    chan_list_2ghz, nitems(chan_list_2ghz),
		    !(sc->sc_capa.has_5ghz || sc->sc_capa.has_6ghz));
	if (rv == 0 && sc->sc_capa.has_5ghz)
		rv = mt7921_mcu_rate_txpower_band(sc, MT_TX_PWR_BAND_5GHZ,
		    chan_list_5ghz, nitems(chan_list_5ghz),
		    !sc->sc_capa.has_6ghz);
	if (rv == 0 && sc->sc_capa.has_6ghz)
		rv = mt7921_mcu_rate_txpower_band(sc, MT_TX_PWR_BAND_6GHZ,
		    chan_list_6ghz, nitems(chan_list_6ghz), 1);
	return rv;
}

void
mt7921_mac_reset_counters(struct mwx_softc *sc)
{
	int i;

	for (i = 0; i < 4; i++) {
		mwx_read(sc, MT_TX_AGG_CNT(0, i));
		mwx_read(sc, MT_TX_AGG_CNT2(0, i));
	}

	/* XXX TODO stats in softc */

	/* reset airtime counters */
	mwx_read(sc, MT_MIB_SDR9(0));
	mwx_read(sc, MT_MIB_SDR36(0));
	mwx_read(sc, MT_MIB_SDR37(0));
	mwx_set(sc, MT_WF_RMAC_MIB_TIME0(0), MT_WF_RMAC_MIB_RXTIME_CLR);
	mwx_set(sc, MT_WF_RMAC_MIB_AIRTIME0(0), MT_WF_RMAC_MIB_RXTIME_CLR);
}

void
mt7921_mac_set_timing(struct mwx_softc *sc)
{
	uint16_t coverage_class = 0;		/* XXX */
	uint32_t val, reg_offset;
	uint32_t cck = MT_TIMEOUT_CCK_DEF_VAL;
	uint32_t ofdm = MT_TIMEOUT_OFDM_DEF_VAL;
	uint32_t offset;
	int is_2ghz = 1; /* XXX get from ic_bss node */
	uint32_t sifs = is_2ghz ? 10 : 16;
	uint32_t slottime = IEEE80211_DUR_DS_SHSLOT;	/* XXX get from stack */

#ifdef NOTYET
	if (!test_bit(MT76_STATE_RUNNING, &phy->mt76->state))
		return;
#endif

	mwx_set(sc, MT_ARB_SCR(0),
	    MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
	delay(1);

	offset = 3 * coverage_class;
	reg_offset = offset | (offset << 16);

	mwx_write(sc, MT_TMAC_CDTR(0), cck + reg_offset);
	mwx_write(sc, MT_TMAC_ODTR(0), ofdm + reg_offset);
	mwx_write(sc, MT_TMAC_ICR0(0),
	    MT_IFS_EIFS_DEF | MT_IFS_RIFS_DEF |
	    (sifs << MT_IFS_SIFS_SHIFT) |
	    (slottime << MT_IFS_SLOT_SHIFT));

	if (slottime < 20 || !is_2ghz)
		val = MT7921_CFEND_RATE_DEFAULT;
	else
		val = MT7921_CFEND_RATE_11B;

	mwx_rmw(sc, MT_AGG_ACR0(0), val, MT_AGG_ACR_CFEND_RATE_MASK);
	mwx_clear(sc, MT_ARB_SCR(0),
	    MT_ARB_SCR_TX_DISABLE | MT_ARB_SCR_RX_DISABLE);
}

int
mt7921_mcu_uni_add_dev(struct mwx_softc *sc, struct mwx_vif *mvif,
    struct mwx_node *mn, int enable)
{
	struct {
		struct {
			uint8_t		omac_idx;
			uint8_t		band_idx;
			uint16_t	pad;
		} __packed hdr;
		struct req_tlv {
			uint16_t	tag;
			uint16_t	len;
			uint8_t		active;
			uint8_t		pad;
			uint8_t		omac_addr[ETHER_ADDR_LEN];
		} __packed tlv;
	} dev_req = {
		.hdr = {
			.omac_idx = mvif->omac_idx,
			.band_idx = mvif->band_idx,
		},
		.tlv = {
			.tag = htole16(DEV_INFO_ACTIVE),
			.len = htole16(sizeof(dev_req.tlv)),
			.active = (uint8_t)enable,
		},
	};
	struct {
		struct {
			uint8_t		bss_idx;
			uint8_t		pad[3];
		} __packed hdr;
		struct mt76_connac_bss_basic_tlv basic;
	} basic_req = {
		.hdr = {
			.bss_idx = mvif->idx,
		},
		.basic = {
			.tag = htole16(UNI_BSS_INFO_BASIC),
			.len = htole16(
			    sizeof(struct mt76_connac_bss_basic_tlv)),
			.omac_idx = mvif->omac_idx,
			.band_idx = mvif->band_idx,
			.wmm_idx = mvif->wmm_idx,
			.active = (uint8_t)enable,
			.bmc_tx_wlan_idx = htole16(mn->wcid),
			.sta_idx = htole16(mn->wcid),
			.conn_state = 1,
		},
	};
	int rv, idx, cmd, len;
	void *data;
    const uint32_t awdlEpoch = sc->sc_awdl_epoch;

	if (mvif == &sc->sc_awdl_vif) {
        // MT7921 firmware rejects the IBSS BSS (0xc00000bb on this card).
        // Match MT76's NL80211_IFTYPE_P2P_DEVICE constructor: AP|P2P
        // identifies a discovery device while conn_state=1 leaves it
        // disconnected. Setting conn_state=0 plus a BSSID/PHY configuration
        // is the separate uni_add_bss/start_ap path for a connected GO.
        // AWDL has no GO beacon or persistent channel; its radio ownership
        // comes exclusively from the self-contained ROC request/grant.
        basic_req.basic.conn_type = htole32(STA_TYPE_AP | NETWORK_P2P);
    } else switch (sc->sc_ic.ic_opmode) {
	case IEEE80211_M_MONITOR:
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_HOSTAP:
#endif
		basic_req.basic.conn_type =
		    htole32(STA_TYPE_AP | NETWORK_INFRA);
		break;
	case IEEE80211_M_STA:
		basic_req.basic.conn_type =
		    htole32(STA_TYPE_STA | NETWORK_INFRA);
		break;
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_IBSS:
		basic_req.basic.conn_type =
		    htole32(STA_TYPE_ADHOC | NETWORK_IBSS);
		break;
#endif
	default:
		panic("%s: unknown operation mode", DEVNAME(sc));
	}

	idx = mvif->omac_idx > EXT_BSSID_START ? HW_BSSID_0 : mvif->omac_idx;
	basic_req.basic.hw_bss_idx = idx;

	memcpy(dev_req.tlv.omac_addr, mvif == &sc->sc_awdl_vif ?
        sc->sc_awdl_address : sc->sc_ic.ic_myaddr, ETHER_ADDR_LEN);

	if (enable) {
		cmd = MCU_UNI_CMD_DEV_INFO_UPDATE;
		data = &dev_req;
		len = sizeof(dev_req);
	} else {
		cmd = MCU_UNI_CMD_BSS_INFO_UPDATE;
		data = &basic_req;
		len = sizeof(basic_req);
	}

printf("%s: %s cmd %x mvif idx %d omac %d band %d wmm %d\n", DEVNAME(sc), __func__, cmd, mvif->idx, mvif->omac_idx, mvif->band_idx, mvif->wmm_idx);
	rv = mvif == &sc->sc_awdl_vif ? mwx_send_assoc_status(sc,cmd,data,len) :
        mwx_mcu_send_wait(sc, cmd, data, len);
	if (rv != 0)
		return rv;
    if (mvif == &sc->sc_awdl_vif && sc->sc_awdl_epoch != awdlEpoch)
        return ECANCELED;

	if (enable) {
		cmd = MCU_UNI_CMD_BSS_INFO_UPDATE;
		data = &basic_req;
		len = sizeof(basic_req);
	} else {
		cmd = MCU_UNI_CMD_DEV_INFO_UPDATE;
		data = &dev_req;
		len = sizeof(dev_req);
	}

printf("%s: %s cmd %x wcid %d\n", DEVNAME(sc), __func__, cmd, mn->wcid);
return mvif == &sc->sc_awdl_vif ? mwx_send_assoc_status(sc,cmd,data,len) :
        mwx_mcu_send_wait(sc, cmd, data, len);
}

int
mt7921_mcu_set_sniffer(struct mwx_softc *sc, int enable)
{
	struct {
		uint8_t		band_idx;
		uint8_t		pad[3];
		struct sniffer_enable_tlv {
			uint16_t	tag;
			uint16_t	len;
			uint8_t		enable;
			uint8_t		pad[3];
		}		enable;
	} req = {
		.band_idx = 0,
		.enable = {
			.tag = htole16(0),
			.len = htole16(sizeof(req.enable)),
			.enable = (uint8_t)enable,
		},
	};

	return mwx_mcu_send_wait(sc, MCU_UNI_CMD_SNIFFER, &req, sizeof(req));
}

int
mt7921_mcu_set_beacon_filter(struct mwx_softc *sc, int enable)
{
	int rv;

	if (enable) {
#ifdef NOTYET
		rv = mt7921_mcu_uni_bss_bcnft(dev, vif, true);
		if (rv)
			return rv;
#endif
		mwx_set(sc, MT_WF_RFCR(0), MT_WF_RFCR_DROP_OTHER_BEACON);
	} else {
		rv = mt7921_mcu_set_bss_pm(sc, 0);
		if (rv)
			return rv;
		mwx_clear(sc, MT_WF_RFCR(0), MT_WF_RFCR_DROP_OTHER_BEACON);
	}
	return 0;
}

/*
 * MT7921 firmware owns RFCR; MT76 changes it only through this command.
 * A nonzero fif selects FIF mode, otherwise bit_op sets/clears bit_map.
 */
int
mt7921_mcu_set_rxfilter(struct mwx_softc *sc, uint32_t fif, uint8_t bit_op,
    uint32_t bit_map)
{
	struct {
		uint8_t		rsv[4];
		uint8_t		mode;
		uint8_t		rsv2[3];
		uint32_t	fif;
		uint32_t	bit_map;
		uint8_t		bit_op;
		uint8_t		pad[51];
	} __packed req = {
		.mode = (uint8_t)(fif ? 1 : 2),
		.fif = htole32(fif),
		.bit_map = htole32(bit_map),
		.bit_op = bit_op,
	};

	return mwx_mcu_send_msg(sc, MCU_CE_CMD_SET_RX_FILTER, &req,
	    sizeof(req), NULL);
}

int
mt7921_mcu_set_bss_pm(struct mwx_softc *sc, int enable)
{
#ifdef NOTYET
	struct {
		uint8_t		bss_idx;
		uint8_t		dtim_period;
		uint16_t	aid;
		uint16_t	bcn_interval;
		uint16_t	atim_window;
		uint8_t		uapsd;
		uint8_t		bmc_delivered_ac;
		uint8_t		bmc_triggered_ac;
		uint8_t		pad;
	} req = {
		.bss_idx = mvif->mt76.idx,
		.aid = htole16(vif->cfg.aid),
		.dtim_period = vif->bss_conf.dtim_period,
		.bcn_interval = htole16(vif->bss_conf.beacon_int),
	};
#endif
	struct {
		uint8_t		bss_idx;
		uint8_t		pad[3];
	} req_hdr = {
		.bss_idx = /* mvif->mt76.idx XXX */ 0,
	};
	int rv;

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_SET_BSS_ABORT,
		&req_hdr, sizeof(req_hdr), NULL);
#ifdef NOTYET
	if (rv != 0 || !enable)
		return rv;
	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_SET_BSS_CONNECTED,
		&req, sizeof(req), NULL);
#endif
	return rv;
}

#define IEEE80211_NUM_ACS	4
int
mt7921_mcu_set_tx(struct mwx_softc *sc, struct mwx_vif *mvif)
{
	struct edca {
		uint16_t	cw_min;
		uint16_t	cw_max;
		uint16_t	txop;
		uint16_t	aifs;
		uint8_t		guardtime;
		uint8_t		acm;
	} __packed;
	struct mt7921_mcu_tx {
		struct edca	edca[IEEE80211_NUM_ACS];
		uint8_t		bss_idx;
		uint8_t		qos;
		uint8_t		wmm_idx;
		uint8_t		pad;
	} __packed req = {
		.bss_idx = mvif->idx,
		.qos = /* vif->bss_conf.qos */ 0,
		.wmm_idx = mvif->wmm_idx,
	};
#ifdef NOTYET
	struct mu_edca {
		uint8_t		cw_min;
		uint8_t		cw_max;
		uint8_t		aifsn;
		uint8_t		acm;
		uint8_t		timer;
		uint8_t		padding[3];
	};
	struct mt7921_mcu_mu_tx {
		uint8_t		ver;
		uint8_t		pad0;
		uint16_t	len;
		uint8_t		bss_idx;
		uint8_t		qos;
		uint8_t		wmm_idx;
		uint8_t		pad1;
		struct mu_edca	edca[IEEE80211_NUM_ACS];
		uint8_t		pad3[32];
	} __packed req_mu = {
		.bss_idx = mvif->mt76.idx,
		.qos = vif->bss_conf.qos,
		.wmm_idx = mvif->mt76.wmm_idx,
	};
#endif
	static const int to_aci[] = { 1, 0, 2, 3 };
	int ac, rv;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		//struct ieee80211_tx_queue_params *q = &mvif->queue_params[ac];
		struct edca *e = &req.edca[to_aci[ac]];

		e->aifs = htole16(/* q->aifs */ 2);
		e->txop = htole16(/* q->txop */ 0);

#ifdef NOTYET
		if (q->cw_min)
			e->cw_min = htole16(q->cw_min);
		else
#endif
			e->cw_min = htole16(5);

#ifdef NOTYET
		if ( q->cw_max)
			e->cw_max = htole16(q->cw_max);
		else
#endif
			e->cw_max = htole16(10);
	}

	rv = mwx_mcu_send_msg(sc, MCU_CE_CMD_SET_EDCA_PARMS, &req,
	    sizeof(req), NULL);

#ifdef NOTYET
	if (rv)
		return rv;
	if (!vif->bss_conf.he_support)
		return 0;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_he_mu_edca_param_ac_rec *q;
		struct mu_edca *e;

		if (!mvif->queue_params[ac].mu_edca)
			break;

		q = &mvif->queue_params[ac].mu_edca_param_rec;
		e = &(req_mu.edca[to_aci[ac]]);

		e->cw_min = q->ecw_min_max & 0xf;
		e->cw_max = (q->ecw_min_max & 0xf0) >> 4;
		e->aifsn = q->aifsn;
		e->timer = q->mu_edca_timer;
	}

	rv = mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_MU_EDCA_PARMS),
	    &req_mu, sizeof(req_mu), false);
#endif
	return rv;
}

static void mwx_awdl_strip_deaggregate(struct mbuf *m,uint32_t rxd4)
{
    // DAMSDU_EN is enabled in mwx_mac_init. Raw AWDL deaggregated RX
    // retains the subframe header and has two extra alignment bytes after
    // the QoS header. Remove only RXD4-marked AWDL padding before dispatch.
    if((rxd4 & MT_RXD4_NORMAL_PAYLOAD_FORMAT) &&
       mtkawdl::relocateDeaggregatedHeader(mtod(m,uint8_t *),mbuf_len(m)))
        m_adj(m,2);
}

int
mt7921_mac_fill_rx(struct mwx_softc *sc, struct mbuf *m,
    struct ieee80211_rxinfo *rxi)
{
	struct ieee80211com *ic = &sc->sc_ic;
	mtk_wlan_rx_info info;
	uint32_t drop;

	/* MtkWireless: descriptor layout and bounds are checked in Rust
	 * (rust/mtk-wlan/src/connac2/rxd.rs) before any group is read. */
	drop = mtk_wlan_rx_parse(mtod(m, const uint8_t *), mbuf_len(m),
	    sc->sc_capa.num_streams, sc->sc_capa.antenna_mask, &info);
	sc->sc_rx_cur_rate = info.rate;
	if (drop != MTK_WLAN_RX_OK) {
		int rv = drop == MTK_WLAN_RX_MAX_LEN ? -EINVAL : -1;

		if (drop == MTK_WLAN_RX_ICV) {
			ic->ic_stats.is_rx_decryptcrc++;
		} else if (drop == MTK_WLAN_RX_TKIP_MIC) {
			/* report MIC failures to net80211 for TKIP */
			ic->ic_stats.is_rx_locmicfail++;
			ieee80211_michael_mic_failure(ic, 0/* XXX */);
		}
		MWX_RX_DROP_LOG(sc, mtk_wlan_rx_drop_name(drop), rv, info.rxd[0],
		    info.rxd[1], info.rxd[2], info.rxd[3], info.rxd[4]);
		return rv;
	}

	if (info.hw_decrypted)
		rxi->rxi_flags |= IEEE80211_RXI_HWDEC;
	if (info.has_rssi)
		rxi->rxi_rssi = info.rssi;

	m_adj(m, info.hdr_gap);
	mwx_awdl_strip_deaggregate(m, info.rxd[4]);
#if 0
	if (status->amsdu) {
		memmove(skb->data + 2, skb->data,
			ieee80211_get_hdrlen_from_skb(skb));
		skb_pull(skb, 2);
	}

	struct ieee80211_hdr *hdr;

	if (insert_ccmp_hdr) {
		u8 key_id = FIELD_GET(MT_RXD1_NORMAL_KEY_ID, rxd1);

		mt76_insert_ccmp_hdr(skb, key_id);
	}

	hdr = mt76_skb_get_hdr(skb);
	fc = hdr->frame_control;
	if (ieee80211_is_data_qos(fc)) {
		seq_ctrl = le16_to_cpu(hdr->seq_ctrl);
		qos_ctl = *ieee80211_get_qos_ctl(hdr);
	}

	if (!status->wcid || !ieee80211_is_data_qos(fc))
		return 0;

	status->aggr = unicast && !ieee80211_is_qos_nullfunc(fc);
	status->seqno = IEEE80211_SEQ_TO_SN(seq_ctrl);
	status->qos_ctl = qos_ctl;
#endif
	rxi->rxi_chan = info.channel;

	if (rxi->rxi_flags & IEEE80211_RXI_HWDEC) {
		/*
		 * MT7921/MT7922 firmware decrypts and removes CCMP IV/MIC.
		 * Leave the protected bit clear so net80211 does not try to
		 * strip an IV which is no longer present.
		 */
		struct ieee80211_frame *wh;

		wh = mtod(m, struct ieee80211_frame *);
		wh->i_fc[1] &= ~IEEE80211_FC1_PROTECTED;
	}

	return 0;
}

uint32_t
mt7921_mac_tx_rate_val(struct mwx_softc *sc)
{
	int rateidx = 0, offset = 4;
	uint32_t rate, mode;

	/* XXX TODO basic_rates
	rateidx = ffs(vif->bss_conf.basic_rates) - 1;
	*/

	if (IEEE80211_IS_CHAN_2GHZ(sc->sc_ic.ic_bss->ni_chan))
		offset = 0;
	/* pick the lowest rate for hidden nodes */
	if (rateidx < 0)
		rateidx = 0;

	rateidx += offset;

	if (rateidx >= nitems(mt76_rates))
		rateidx = offset;

	rate =  mt76_rates[rateidx].hw_value;
	mode = (rate >> 8) << MT_TX_RATE_MODE_SHIFT;
	rate &= 0xff;

	return (rate & MT_TX_RATE_IDX_MASK) | (mode & MT_TX_RATE_MODE_MASK);
}

void
mt7921_mac_write_txwi_80211(struct mwx_softc *sc, struct mbuf *m,
    struct ieee80211_node *ni, struct mt76_txwi *txp)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	uint32_t val;
	uint8_t type, subtype, tid = 0;
	u_int hdrlen;
	int multicast;


	wh = mtod(m, struct ieee80211_frame *);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	type >>= IEEE80211_FC0_TYPE_SHIFT;
	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
	subtype >>= IEEE80211_FC0_SUBTYPE_SHIFT;
	multicast = IEEE80211_IS_MULTICAST(wh->i_addr1);

	if (type == IEEE80211_FC0_TYPE_CTL)
		hdrlen = sizeof(struct ieee80211_frame_min);
	else
		hdrlen = ieee80211_get_hdrlen(wh);

	/* Put QoS frames on the data queue which maps to their TID. */
	if (ieee80211_has_qos(wh)) {
		uint16_t qos = ieee80211_get_qos(wh);

		tid = qos & IEEE80211_QOS_TID;
	}

	/* As mt76 does for ADDBA requests built by mac80211. */
	if (type == (IEEE80211_FC0_TYPE_MGT >> IEEE80211_FC0_TYPE_SHIFT) &&
	    subtype == (IEEE80211_FC0_SUBTYPE_ACTION >>
	    IEEE80211_FC0_SUBTYPE_SHIFT) &&
	    mbuf_len(m) >= hdrlen + 5) {
		const uint8_t *frm = mtod(m, const uint8_t *) + hdrlen;

		if (frm[0] == IEEE80211_CATEG_BA &&
		    frm[1] == IEEE80211_ACTION_ADDBA_REQ) {
			/* [1] category [1] action [1] token [2] params */
			txp->txwi[5] |= htole32(MT_TXD5_ADD_BA);
			tid = ((frm[3] | frm[4] << 8) >> 2) & 0xf;
		}
	}

#ifdef NOTYET
	if (ieee80211_is_action(fc) &&
	    mgmt->u.action.category == WLAN_CATEGORY_BACK &&
	    mgmt->u.action.u.addba_req.action_code == WLAN_ACTION_ADDBA_REQ) {
		u16 capab = le16_to_cpu(mgmt->u.action.u.addba_req.capab);

		txp->txwi[5] |= htole32(MT_TXD5_ADD_BA);
		tid = (capab >> 2) & IEEE80211_QOS_CTL_TID_MASK;
	} else if (ieee80211_is_back_req(hdr->frame_control)) {
		struct ieee80211_bar *bar = (struct ieee80211_bar *)hdr;
		u16 control = le16_to_cpu(bar->control);

		tid = FIELD_GET(IEEE80211_BAR_CTRL_TID_INFO_MASK, control);
	}
#endif

	val = MT_HDR_FORMAT_802_11 | MT_TXD1_HDR_INFO(hdrlen / 2) |
	    MT_TXD1_TID(tid);
	txp->txwi[1] |= htole32(val);

	val = MT_TXD2_FRAME_TYPE(type) | MT_TXD2_SUB_TYPE(subtype);
	if (multicast)
		val |= MT_TXD2_MULTICAST;

#ifdef NOTYET
	if (key && multicast && ieee80211_is_robust_mgmt_frame(skb) &&
	    key->cipher == WLAN_CIPHER_SUITE_AES_CMAC) {
		val |= MT_TXD2_BIP;
		txp->txwi[3] &= ~htole32(MT_TXD3_PROTECT_FRAME);
	}
#endif

	/*
	 * type is shifted here, so OpenBSD's comparison against the unshifted
	 * IEEE80211_FC0_TYPE_DATA made every data frame fixed-rate. Keep that
	 * behaviour unless MWX_KNOB_DATA_RA asks for firmware rate control.
	 */
	if (multicast ||
	    type != (IEEE80211_FC0_TYPE_DATA >> IEEE80211_FC0_TYPE_SHIFT) ||
	    (sc->sc_knobs & MWX_KNOB_DATA_RA) == 0 ||
        ni == &sc->sc_awdl_vif.vif_mn.ni || ni == &sc->sc_awdl_peers[0].ni ||
        ni == &sc->sc_awdl_peers[1].ni || ni == &sc->sc_awdl_peers[2].ni) {
		/* Fixed rata is available just for 802.11 txd */
		uint32_t rate, val6;

		val |= MT_TXD2_FIX_RATE;
		/* hardware won't add HTC for mgmt/ctrl frame */
		val |= htole32(MT_TXD2_HTC_VLD);

		if (ni == &sc->sc_awdl_vif.vif_mn.ni ||
            ni == &sc->sc_awdl_peers[0].ni || ni == &sc->sc_awdl_peers[1].ni ||
            ni == &sc->sc_awdl_peers[2].ni) {
            uint32_t hw=mt76_rates[4].hw_value;
            rate=(hw & 0xff) | ((hw>>8)<<MT_TX_RATE_MODE_SHIFT);
        } else rate = mt7921_mac_tx_rate_val(sc);

		val6 = MT_TXD6_FIXED_BW;
		val6 |= (rate << MT_TXD6_TX_RATE_SHIFT) & MT_TXD6_TX_RATE_MASK;
		txp->txwi[6] |= htole32(val6);
		txp->txwi[3] |= htole32(MT_TXD3_BA_DISABLE);
	}

	txp->txwi[2] |= htole32(val);

#ifdef NOTYET
	if (ieee80211_is_beacon(fc)) {
		txp->txwi[3] &= ~htole32(MT_TXD3_SW_POWER_MGMT);
		txp->txwi[3] |= htole32(MT_TXD3_REM_TX_COUNT);
	}

	if (info->flags & IEEE80211_TX_CTL_INJECTED) {
		u16 seqno = le16_to_cpu(hdr->seq_ctrl);

		if (ieee80211_is_back_req(hdr->frame_control)) {
			struct ieee80211_bar *bar;

			bar = (struct ieee80211_bar *)skb->data;
			seqno = le16_to_cpu(bar->start_seq_num);
		}

		val = MT_TXD3_SN_VALID |
			FIELD_PREP(MT_TXD3_SEQ, IEEE80211_SEQ_TO_SN(seqno));
		txp->txwi[3] |= htole32(val);
		txp->txwi[7] &= ~htole32(MT_TXD7_HW_AMSDU);
	}
#endif

	val = MT_TXD7_TYPE(type) | MT_TXD7_SUB_TYPE(subtype);
	txp->txwi[7] |= htole32(val);

#if NBPFILTER > 0
	if (__predict_false(sc->sc_drvbpf != NULL)) {
		struct mwx_tx_radiotap_header *tap = &sc->sc_txtap;
		uint16_t chan_flags;

		tap->wt_flags = 0;
		tap->wt_chan_freq = htole16(ni->ni_chan->ic_freq);
		chan_flags = ni->ni_chan->ic_flags;
		if (ic->ic_curmode != IEEE80211_MODE_11N &&
			ic->ic_curmode != IEEE80211_MODE_11AC &&
			ic->ic_curmode != IEEE80211_MODE_11AX) {
			chan_flags &= ~IEEE80211_CHAN_HT;
			chan_flags &= ~IEEE80211_CHAN_40MHZ;
		}
		if (ic->ic_curmode != IEEE80211_MODE_11AC &&
		    ic->ic_curmode != IEEE80211_MODE_11AX)
			chan_flags &= ~IEEE80211_CHAN_VHT;
		tap->wt_chan_flags = htole16(chan_flags);
#ifdef NOTYET
		if ((ni->ni_flags & IEEE80211_NODE_HT) &&
		    !IEEE80211_IS_MULTICAST(wh->i_addr1) &&
		    type == IEEE80211_FC0_TYPE_DATA &&
		    rinfo->ht_plcp != MWX_RATE_HT_SISO_MCS_INV_PLCP) {
			tap->wt_rate = (0x80 | rinfo->ht_plcp);
		} else
			tap->wt_rate = rinfo->rate;
#endif
		tap->wt_rate = 2;
		if ((ic->ic_flags & IEEE80211_F_WEPON) &&
		    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED))
			tap->wt_flags |= IEEE80211_RADIOTAP_F_WEP;

		bpf_mtap_hdr(sc->sc_drvbpf, tap, sc->sc_txtap_len,
		    m, BPF_DIRECTION_OUT);
	}
#endif

}

static inline uint8_t
mt7921_lmac_mapping(uint8_t ac)
{
	/* LMAC uses the reverse order of mac80211 AC indexes */
	return 3 - ac;
}

void
mt7921_mac_write_txwi(struct mwx_softc *sc, struct mbuf *m,
    struct ieee80211_node *ni, struct mt76_txwi *txp)
{
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct ieee80211_frame *wh;
	uint32_t val, p_fmt, omac_idx;
	uint8_t q_idx, wmm_idx, band_idx;
	uint8_t phy_idx = 0;
	static const struct timeval txs_interval = { 0, 250000 };
	int pid = MT_PACKET_ID_NO_SKB;
	enum mt76_txq_id qid = MT_TXQ_BE;

	wh = mtod(m, struct ieee80211_frame *);
	/*
	 * Ask for TX status on a unicast data frame about four times a
	 * second; it carries the rate the firmware rate control picked.
	 * Asking on every frame doubled the RX ring traffic for nothing.
	 */
    const bool awdl = ni == &sc->sc_awdl_vif.vif_mn.ni ||
        ni == &sc->sc_awdl_peers[0].ni || ni == &sc->sc_awdl_peers[1].ni ||
        ni == &sc->sc_awdl_peers[2].ni;
	if (!awdl && (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    !IEEE80211_IS_MULTICAST(wh->i_addr1) &&
	    ratecheck(&sc->sc_txs_last, &txs_interval))
		pid = MT_PACKET_ID_FIRST;
    if(awdl) pid=(wh->i_fc[0]&IEEE80211_FC0_TYPE_MASK)==IEEE80211_FC0_TYPE_DATA ?
        (IEEE80211_IS_MULTICAST(wh->i_addr1) ? mtkawdl::txPidMulticast : mtkawdl::txPidUnicast) :
        mtkawdl::txPidAction;
    const mwx_vif *vif = awdl ? &sc->sc_awdl_vif : &sc->sc_vif;
	omac_idx = vif->omac_idx << MT_TXD1_OWN_MAC_SHIFT;
	wmm_idx = vif->wmm_idx;
	band_idx = vif->band_idx;
    // MT7921 uses MT_DRV_HW_MGMT_TXQ. MT76's off-channel management
    // path selects PSD -> ALTX for PSF/MIF action frames, independently
    // of the discovery BSS's normal WMM data queues. AWDL data retains
    // its QoS queue and all TX still requires a matching ROC grant.
    if(awdl && (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK)==IEEE80211_FC0_TYPE_MGT)
        qid=MT_TXQ_PSD;

	if (qid >= MT_TXQ_PSD) {
		p_fmt = MT_TX_TYPE_CT;
		q_idx = MT_LMAC_ALTX0;
	} else {
		/* LMAC queues: BK=0 BE=1 VI=2 VO=3, indexed by QoS TID. */
		static const uint8_t tid_to_lmac[] = { 1, 0, 0, 1, 2, 2, 3, 3 };
		uint8_t lmac_ac = mt7921_lmac_mapping(0);	/* VO */

		/* Management stays on VO, data uses BE unless QoS says otherwise. */
		if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) ==
		    IEEE80211_FC0_TYPE_DATA &&
		    (sc->sc_knobs & MWX_KNOB_TX_VO) == 0)
			lmac_ac = ieee80211_has_qos(wh) ?
			    tid_to_lmac[ieee80211_get_qos(wh) &
			    IEEE80211_QOS_TID & 7] : tid_to_lmac[0];
		p_fmt = MT_TX_TYPE_CT;
		q_idx = wmm_idx * MWX_MAX_WMM_SETS + lmac_ac;

#ifdef NOTYET
		/* counting non-offloading skbs */
		wcid->stats.tx_bytes += skb->len;
		wcid->stats.tx_packets++;
#endif
	}

	val = ((mbuf_pkthdr_len(m) + MT_TXD_SIZE) & MT_TXD0_TX_BYTES_MASK) |
		p_fmt | MT_TXD0_Q_IDX(q_idx);
	txp->txwi[0] = htole32(val);

	val = MT_TXD1_LONG_FORMAT | (mn->wcid & MT_TXD1_WLAN_IDX_MASK) |
	    (omac_idx & MT_TXD1_OWN_MAC_MASK);
	if (phy_idx || band_idx)
		val |= MT_TXD1_TGID;
	txp->txwi[1] = htole32(val);
	txp->txwi[2] = 0;

	val = 15 << MT_TXD3_REM_TX_COUNT_SHIFT;
	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
		val |= MT_TXD3_PROTECT_FRAME;
#ifdef NOTYET
	if (info->flags & IEEE80211_TX_CTL_NO_ACK)
		val |= MT_TXD3_NO_ACK;
#endif
	/*
	 * MtkWireless: keep the PM bit of the AWDL power-save null frames
	 * (mwx_awdl_infra_pm); without SW_POWER_MGMT the MAC rewrites it.
	 */
	if (!awdl && (wh->i_fc[0] & (IEEE80211_FC0_TYPE_MASK |
	    IEEE80211_FC0_SUBTYPE_MASK)) ==
	    (IEEE80211_FC0_TYPE_DATA | IEEE80211_FC0_SUBTYPE_NODATA))
		val |= MT_TXD3_SW_POWER_MGMT;
	txp->txwi[3] = htole32(val);
	txp->txwi[4] = 0;

	val = pid & MT_TXD5_PID;
	if (pid >= MT_PACKET_ID_FIRST)
		val |= MT_TXD5_TX_STATUS_HOST;
	txp->txwi[5] = htole32(val);
	txp->txwi[6] = 0;
	txp->txwi[7] = /* XXX wcid->amsdu ? htole32(MT_TXD7_HW_AMSDU) : */ 0;

#ifdef NOTYET
	if (is_8023)
		mt76_connac2_mac_write_txwi_8023(txp, m, wcid);
	else
#endif
		mt7921_mac_write_txwi_80211(sc, m, ni, txp);
    if (awdl) {
        // AWDL starts with legacy OFDM at 6 Mbps on either band. Keep the
        // supplied sequence number and never use the infrastructure key/RA.
        // The common 802.11 builder applies the complete fixed-rate setup
        // even when infrastructure RA is enabled (as in the MT76 rate fix).
        txp->txwi[3] |= htole32(MT_TXD3_BA_DISABLE | MT_TXD3_SN_VALID |
            (((mtkawdl::le16((uint8_t *)wh+22)>>4)<<16)&MT_TXD3_SEQ));
        if (IEEE80211_IS_MULTICAST(wh->i_addr1)) txp->txwi[3] |= htole32(MT_TXD3_NO_ACK);
    }
}

void
mwx_mac_tx_free(struct mwx_softc *sc, struct mbuf *m)
{
	struct mwx_txwi *mt;

	mwx_dma_tx_cleanup(sc, &sc->sc_txq);

	/* Both the event header and each PAIR/MSDU word must be present. */
	if (mbuf_pkthdr_len(m) < 2 * sizeof(uint32_t))
		goto out;

	if ((m = m_pullup(m, mbuf_pkthdr_len(m))) == NULL)
		return;

	/* MtkWireless: words and PAIR entries are walked in Rust
	 * (rust/mtk-wlan/src/connac2/event.rs). */
	mtk_wlan_tx_free_iter it;
	mtk_wlan_tx_freed freed;

	mtk_wlan_tx_free_begin(mtod(m, const uint8_t *), mbuf_len(m), &it);
	while (mtk_wlan_tx_free_next(&it, &freed)) {
		if (freed.msdu >= sc->sc_txwi.mt_count)
			continue;
		mt = &sc->sc_txwi.mt_data[freed.msdu];
		if (mt->mt_busy == 0)
			continue;
		if (mt->mt_eapol) {
			/* status 0 = acked; count = transmissions */
			MWX_DEV_LOG(sc, "eapol txfree msdu=%u stat=%u count=%u "
			    "raw=0x%08x\n", freed.msdu, freed.status, freed.count,
			    freed.raw);
			mt->mt_eapol = 0;
		}
		mwx_txwi_put(sc, mt);
	}

out:
	m_freem(m);
	mwx_tx_resume(sc);
}

int
mt7921_set_channel(struct mwx_softc *sc)
{
	int rv;

	/* stop queues, block other configs (MT76_RESET) */
	// XXX NOTYET mt76_set_channel(sc);

	rv = mt7921_mcu_set_chan_info(sc, MCU_EXT_CMD_CHANNEL_SWITCH);
	if (rv)
		return rv;
	mt7921_mac_set_timing(sc);
	mt7921_mac_reset_counters(sc);

	/* restart queues */
	return 0;
}

struct mbuf *
mwx_alloc_sta_req_tlv(int len)
{
	struct mbuf *m;

	/* Allocate mbuf cluster with enough space */
	m = MCLGETL(NULL, M_DONTWAIT, MCLBYTES);
	if (m == NULL)
		return NULL;

	/* align to have space for the mcu header */
	mbuf_setdata(m, (uint8_t *)mbuf_data(m) + sizeof(struct mwx_mcu_txd) + len, 0);
	mbuf_setlen(m, 0);
	mbuf_pkthdr_setlen(m, 0);

	return m;
}

void *
mwx_append_len(struct mbuf *m, int len)
{
	caddr_t p;
	size_t oldlen;

	KASSERT(m_trailingspace(m) >= len);

	oldlen = mbuf_len(m);
	p = mtod(m, caddr_t) + oldlen;
	mbuf_setlen(m, oldlen + len);
	mbuf_pkthdr_setlen(m, mbuf_len(m));
	memset(p, 0, len);

	return p;
}

/*
 * Reserve len bytes at the end of mbuf m, return to start of that area
 * after initializing the data. It also sets the tag and len hdr.
 */
void *
mwx_append_tlv(struct mbuf *m, uint16_t *tlvnum, int tag, int len)
{
	struct {
		uint16_t	tag;
		uint16_t	len;
	} tlv = {
		.tag = htole16(tag),
		.len = htole16(len),
	};
	caddr_t p;

	p = (caddr_t)mwx_append_len(m, len);
	memcpy(p, &tlv, sizeof(tlv));

	*tlvnum += 1;

	return p;
}

struct mbuf *
mwx_fill_sta_req_hdr(struct mbuf *m, struct mwx_vif *mvif,
    uint8_t muar_idx, uint16_t wcid, uint16_t tlvnum)
{
	struct sta_req_hdr *hdr;

	KASSERT(m_leadingspace(m) >= sizeof(struct sta_req_hdr));
	m = m_prepend(m, sizeof(struct sta_req_hdr), M_DONTWAIT);
	if (m == NULL)
		return NULL;
	hdr = mtod(m, struct sta_req_hdr *);
	memset(hdr, 0, sizeof(struct sta_req_hdr));

	hdr->bss_idx = mvif->idx;
	hdr->wlan_idx_lo = wcid & 0xff;
	hdr->wlan_idx_hi = wcid >> 8;
	hdr->muar_idx = muar_idx;
	hdr->is_tlv_append = 1;
	hdr->tlv_num = htole16(tlvnum);
	return m;
}

void
mt7921_mcu_add_basic_tlv(struct mbuf *m, uint16_t *tlvnum, struct mwx_softc *sc,
    struct ieee80211_node *ni, int add, int isnew)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct sta_rec_basic *basic;

	basic = (struct sta_rec_basic *)mwx_append_tlv(m, tlvnum,
	    STA_REC_BASIC, sizeof(*basic));

	basic->extra_info = htole16(EXTRA_INFO_VER);
	if (add) {
		if (isnew)
			basic->extra_info |= htole16(EXTRA_INFO_NEW);
		basic->conn_state = CONN_STATE_PORT_SECURE;
	} else {
		basic->conn_state = CONN_STATE_DISCONNECT;
	}

	if (ni == NULL) {
		basic->conn_type = htole32(STA_TYPE_BC | NETWORK_INFRA);
		memset(basic->peer_addr, 0xff, sizeof(basic->peer_addr));
		return;
	}

	switch (ic->ic_opmode) {
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_HOSTAP:
		basic->conn_type = htole32(STA_TYPE_STA | NETWORK_INFRA);
		break;
#endif
	case IEEE80211_M_STA:
		basic->conn_type = htole32(STA_TYPE_AP | NETWORK_INFRA);
		break;
#ifndef IEEE80211_STA_ONLY
	case IEEE80211_M_IBSS:
	case IEEE80211_M_AHDEMO:
		basic->conn_type = htole32(STA_TYPE_ADHOC | NETWORK_IBSS);
		break;
#endif
	case IEEE80211_M_MONITOR:
		panic("mt7921_mcu_sta_basic_tlv unexpected operation mode");
	}

	basic->aid = htole16(IEEE80211_AID(ni->ni_associd));
	memcpy(basic->peer_addr, ni->ni_macaddr, IEEE80211_ADDR_LEN);
	basic->qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
}

/* Index of a legacy rate in the mt76 band rate table, or -1. */
static int
mwx_legacy_rate_idx(int is_2g, uint8_t rate)
{
	static const uint8_t cck[] = { 2, 4, 11, 22 };
	static const uint8_t ofdm[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
	int i;

	for (i = 0; is_2g && i < (int)nitems(cck); i++)
		if (cck[i] == rate)
			return i;
	for (i = 0; i < (int)nitems(ofdm); i++)
		if (ofdm[i] == rate)
			return i + (is_2g ? (int)nitems(cck) : 0);
	return -1;
}

/*
 * Intersect a peer VHT MCS map with ours: per stream keep the lower
 * of both maximum MCS values, 3 means "not supported".
 */
static uint16_t
mwx_vht_mcs_map(uint16_t peer, uint16_t own)
{
	uint16_t map = 0;
	int i;

	for (i = 0; i < 8; i++) {
		uint8_t p = (peer >> (2 * i)) & 3, o = (own >> (2 * i)) & 3;
		uint8_t v;

		if (p == IEEE80211_VHT_MCS_NOT_SUPPORTED ||
		    o == IEEE80211_VHT_MCS_NOT_SUPPORTED)
			v = IEEE80211_VHT_MCS_NOT_SUPPORTED;
		else
			v = MIN(p, o);
		map |= v << (2 * i);
	}
	return map;
}

#define MWX_HE_FIELD(v, mask)	(((v) & (mask)) >> __builtin_ctz(mask))

/*
 * STA_REC_HE and STA_REC_HE_V2 for an HE AP, as in
 * mt76_connac_mcu_sta_he_tlv() and _v2(). The capabilities are the AP's;
 * the MCS map is what the AP receives limited to what we transmit.
 * No 160 MHz, so only the BW80 map is set.
 */
void
mt7921_mcu_add_sta_he_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const struct ieee80211_he_cap_elem *elem = &ni->ni_he_cap_elem;
	const uint8_t *mac = elem->mac_cap_info, *phy = elem->phy_cap_info;
	struct sta_rec_he *he;
	struct sta_rec_he_v2 *he2;
	uint32_t cap = 0;
	uint16_t mcs80;

	mcs80 = htole16(ieee80211_he_mcs_intersection(
	    le16toh(ni->ni_he_mcs_nss_supp.rx_mcs_80),
	    le16toh(ic->ic_he_mcs_nss_supp.tx_mcs_80)));

	if (mac[0] & IEEE80211_HE_MAC_CAP0_HTC_HE)
		cap |= STA_REC_HE_CAP_HTC;
	if (mac[2] & IEEE80211_HE_MAC_CAP2_BSR)
		cap |= STA_REC_HE_CAP_BSR;
	if (mac[3] & IEEE80211_HE_MAC_CAP3_OMI_CONTROL)
		cap |= STA_REC_HE_CAP_OM;
	if (mac[4] & IEEE80211_HE_MAC_CAP4_AMDSU_IN_AMPDU)
		cap |= STA_REC_HE_CAP_AMSDU_IN_AMPDU;
	if (mac[4] & IEEE80211_HE_MAC_CAP4_BQR)
		cap |= STA_REC_HE_CAP_BQR;
	if (phy[0] & (IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_2G |
	    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_RU_MAPPING_IN_5G))
		cap |= STA_REC_HE_CAP_BW20_RU242_SUPPORT;
	if (phy[1] & IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD)
		cap |= STA_REC_HE_CAP_LDPC;
	if (phy[1] & IEEE80211_HE_PHY_CAP1_HE_LTF_AND_GI_FOR_HE_PPDUS_0_8US)
		cap |= STA_REC_HE_CAP_SU_PPDU_1LTF_8US_GI;
	if (phy[2] & IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US)
		cap |= STA_REC_HE_CAP_NDP_4LTF_3DOT2MS_GI;
	if (phy[2] & IEEE80211_HE_PHY_CAP2_STBC_TX_UNDER_80MHZ)
		cap |= STA_REC_HE_CAP_LE_EQ_80M_TX_STBC;
	if (phy[2] & IEEE80211_HE_PHY_CAP2_STBC_RX_UNDER_80MHZ)
		cap |= STA_REC_HE_CAP_LE_EQ_80M_RX_STBC;
	if (phy[6] & IEEE80211_HE_PHY_CAP6_PARTIAL_BW_EXT_RANGE)
		cap |= STA_REC_HE_CAP_PARTIAL_BW_EXT_RANGE;
	if (phy[7] & IEEE80211_HE_PHY_CAP7_HE_SU_MU_PPDU_4XLTF_AND_08_US_GI)
		cap |= STA_REC_HE_CAP_SU_MU_PPDU_4LTF_8US_GI;
	if (phy[7] & IEEE80211_HE_PHY_CAP7_STBC_TX_ABOVE_80MHZ)
		cap |= STA_REC_HE_CAP_GT_80M_TX_STBC;
	if (phy[7] & IEEE80211_HE_PHY_CAP7_STBC_RX_ABOVE_80MHZ)
		cap |= STA_REC_HE_CAP_GT_80M_RX_STBC;
	if (phy[8] & IEEE80211_HE_PHY_CAP8_HE_ER_SU_PPDU_4XLTF_AND_08_US_GI)
		cap |= STA_REC_HE_CAP_ER_SU_PPDU_4LTF_8US_GI;
	if (phy[8] & IEEE80211_HE_PHY_CAP8_HE_ER_SU_1XLTF_AND_08_US_GI)
		cap |= STA_REC_HE_CAP_ER_SU_PPDU_1LTF_8US_GI;
	if (phy[9] & IEEE80211_HE_PHY_CAP9_NON_TRIGGERED_CQI_FEEDBACK)
		cap |= STA_REC_HE_CAP_TRIG_CQI_FK;
	if (phy[9] & IEEE80211_HE_PHY_CAP9_TX_1024_QAM_LESS_THAN_242_TONE_RU)
		cap |= STA_REC_HE_CAP_TX_1024QAM_UNDER_RU242;
	if (phy[9] & IEEE80211_HE_PHY_CAP9_RX_1024_QAM_LESS_THAN_242_TONE_RU)
		cap |= STA_REC_HE_CAP_RX_1024QAM_UNDER_RU242;

	he = (struct sta_rec_he *)mwx_append_tlv(m, tlvnum, STA_REC_HE,
	    sizeof(*he));
	he->he_cap = htole32(cap);
	he->max_nss_mcs[CMD_HE_MCS_BW80] = mcs80;
	he->t_frame_dur = MWX_HE_FIELD(mac[1],
	    IEEE80211_HE_MAC_CAP1_TF_MAC_PAD_DUR_MASK);
	he->max_ampdu_exp = MWX_HE_FIELD(mac[3],
	    IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_MASK);
	he->bw_set = MWX_HE_FIELD(phy[0],
	    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_MASK);
	he->device_class = MWX_HE_FIELD(phy[1],
	    IEEE80211_HE_PHY_CAP1_DEVICE_CLASS_A);
	he->punc_pream_rx = MWX_HE_FIELD(phy[1],
	    IEEE80211_HE_PHY_CAP1_PREAMBLE_PUNC_RX_MASK);
	he->dcm_tx_mode = MWX_HE_FIELD(phy[3],
	    IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_TX_MASK);
	he->dcm_tx_max_nss = MWX_HE_FIELD(phy[3],
	    IEEE80211_HE_PHY_CAP3_DCM_MAX_TX_NSS_2);
	he->dcm_rx_mode = MWX_HE_FIELD(phy[3],
	    IEEE80211_HE_PHY_CAP3_DCM_MAX_CONST_RX_MASK);
	he->dcm_rx_max_nss = MWX_HE_FIELD(phy[3],
	    IEEE80211_HE_PHY_CAP3_DCM_MAX_RX_NSS_2);
	he->dcm_max_ru = MWX_HE_FIELD(phy[8],
	    IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_MASK);
	he->pkt_ext = 2;

	he2 = (struct sta_rec_he_v2 *)mwx_append_tlv(m, tlvnum, STA_REC_HE_V2,
	    sizeof(*he2));
	memcpy(he2->he_mac_cap, mac, sizeof(he2->he_mac_cap));
	memcpy(he2->he_phy_cap, phy, sizeof(he2->he_phy_cap));
	he2->max_nss_mcs[CMD_HE_MCS_BW80] = mcs80;
	he2->pkt_ext = 2;

	MWX_DEV_LOG(sc, "sta HE cap=0x%06x mac=%02x%02x%02x%02x phy=%02x%02x%02x "
	    "mcs80=0x%04x/0x%04x\n", cap, mac[0], mac[1], mac[2], mac[3],
	    phy[0], phy[1], phy[2], le16toh(ni->ni_he_mcs_nss_supp.rx_mcs_80),
	    le16toh(mcs80));
}

/*
 * Station record TLVs (HT, VHT, PHY, RA, STATE) as in
 * mt76_connac_mcu_sta_tlv(). HT/VHT information is only sent once the
 * association response has been processed (assoc != 0); before that
 * the node only carries what the AP advertised, not what was negotiated.
 */
void
mt7921_mcu_add_sta_tlv(struct mbuf *m, uint16_t *tlvnum, struct mwx_softc *sc,
    struct ieee80211_node *ni, int assoc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct sta_rec_ra_info *ra_info;
	struct sta_rec_state *state;
	struct sta_rec_phy *phy;
	uint16_t supp_rates = 0, basic_rates = 0;
	int is_2g, ht, vht, he, i, idx;

	is_2g = IEEE80211_IS_CHAN_2GHZ(ni->ni_chan);
	ht = assoc && (ni->ni_flags & IEEE80211_NODE_HT) != 0;
	vht = ht && !is_2g && (ni->ni_flags & IEEE80211_NODE_VHT) != 0;
	he = ht && (ic->ic_flags & IEEE80211_F_HEON) != 0 &&
	    (ni->ni_flags & IEEE80211_NODE_HE) != 0;

	if (sc->sc_knobs & MWX_KNOB_OLD_STA) {
		/* What d9b69bb sent: fixed legacy PHY/RA, state NONE. */
		phy = (struct sta_rec_phy *)mwx_append_tlv(m, tlvnum,
		    STA_REC_PHY, sizeof(*phy));
		phy->basic_rate = htole16(0x0150);
		phy->phy_type = is_2g ?
		    PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP : PHY_TYPE_BIT_OFDM;
		if (ieee80211_node_supports_ht(ni))
			phy->phy_type |= PHY_TYPE_BIT_HT;
		if (!is_2g && ieee80211_node_supports_vht(ni))
			phy->phy_type |= PHY_TYPE_BIT_VHT;
		phy->rcpi = 0xdc;
		ra_info = (struct sta_rec_ra_info *)mwx_append_tlv(m, tlvnum,
		    STA_REC_RA, sizeof(*ra_info));
		ra_info->legacy = htole16(RA_LEGACY_OFDM);
		state = (struct sta_rec_state *)mwx_append_tlv(m, tlvnum,
		    STA_REC_STATE, sizeof(*state));
		state->state = 0;
		return;
	}

	if (ht) {
		struct sta_rec_ht *sht;
		uint16_t cap = ni->ni_htcaps;

		if (sc->sc_bss_bw == CMD_CBW_20MHZ)
			cap &= ~(IEEE80211_HTCAP_CBW20_40 |
			    IEEE80211_HTCAP_SGI40);
		cap &= ~((IEEE80211_HTCAP_SGI20 | IEEE80211_HTCAP_SGI40) &
		    ~ic->ic_htcaps);
		sht = (struct sta_rec_ht *)mwx_append_tlv(m, tlvnum,
		    STA_REC_HT, sizeof(*sht));
		sht->ht_cap = htole16(cap);
	}

	if (vht) {
		struct sta_rec_vht *svht;

		svht = (struct sta_rec_vht *)mwx_append_tlv(m, tlvnum,
		    STA_REC_VHT, sizeof(*svht));
		svht->vht_cap = htole32(ni->ni_vhtcaps);
		svht->vht_rx_mcs_map = htole16(mwx_vht_mcs_map(
		    le16toh(ni->ni_vht_mcsinfo.rx_mcs_map),
		    ic->ic_vht_tx_mcs_map));
		svht->vht_tx_mcs_map = htole16(mwx_vht_mcs_map(
		    le16toh(ni->ni_vht_mcsinfo.tx_mcs_map),
		    ic->ic_vht_rx_mcs_map));
	}

	if (he)
		mt7921_mcu_add_sta_he_tlv(m, tlvnum, sc, ni);

	/* Legacy rate bitmaps, indexed like the mt76 band rate tables. */
	for (i = 0; i < ni->ni_rates.rs_nrates; i++) {
		uint8_t r = ni->ni_rates.rs_rates[i];

		idx = mwx_legacy_rate_idx(is_2g, r & IEEE80211_RATE_VAL);
		if (idx < 0)
			continue;
		supp_rates |= 1U << idx;
		if (r & IEEE80211_RATE_BASIC)
			basic_rates |= 1U << idx;
	}

	phy = (struct sta_rec_phy *)mwx_append_tlv(m, tlvnum, STA_REC_PHY,
	    sizeof(*phy));
	phy->basic_rate = htole16(basic_rates ? basic_rates : 0x0150);
	phy->phy_type = is_2g ? PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP :
	    PHY_TYPE_BIT_OFDM;
	if (ht)
		phy->phy_type |= PHY_TYPE_BIT_HT;
	if (vht)
		phy->phy_type |= PHY_TYPE_BIT_VHT;
	if (he)
		phy->phy_type |= PHY_TYPE_BIT_HE;
	/* A-MPDU factor (bits 1:0) and density (bits 4:2) */
	if (ht)
		phy->ampdu = ni->ni_ampdu_param &
		    (IEEE80211_AMPDU_PARAM_LE | IEEE80211_AMPDU_PARAM_SS);
	// XXX phy->rcpi = rssi_to_rcpi(-ewma_rssi_read(&sc->sc_vif.rssi));
	phy->rcpi = 0xdc;	/* XXX STOLEN FROM LINUX DUMP */

	if (is_2g)
		supp_rates = ((supp_rates >> 4) << 6 & RA_LEGACY_OFDM) |
		    (supp_rates & RA_LEGACY_CCK);
	else
		supp_rates = (supp_rates << 6) & RA_LEGACY_OFDM;
	if (supp_rates == 0)
		supp_rates = RA_LEGACY_OFDM;

	ra_info = (struct sta_rec_ra_info *)mwx_append_tlv(m, tlvnum,
	    STA_REC_RA, sizeof(*ra_info));
	ra_info->legacy = htole16(supp_rates);
	if (ht) {
		/* MCS the AP can receive, limited to our TX streams. */
		for (i = 0; i < HT_MCS_MASK_NUM; i++)
			ra_info->rx_mcs_bitmask[i] =
			    ni->ni_rxmcs[i] & ic->ic_sup_mcs[i];
	}

	state = (struct sta_rec_state *)mwx_append_tlv(m, tlvnum,
	    STA_REC_STATE, sizeof(*state));
	/* MT76_STA_INFO_STATE_NONE=0, AUTH=1, ASSOC=2 */
	state->state = assoc ? 2 : 0;
	if (vht) {
		uint8_t nss = MAX(1, MIN(ni->ni_rx_nss,
		    MAX(1, sc->sc_capa.num_streams)));
		uint8_t bw = sc->sc_bss_bw;

		/* IEEE80211_STA_RX_BW_20/40/80/160 */
		if (bw > CMD_CBW_160MHZ)
			bw = bw == CMD_CBW_8080MHZ ? CMD_CBW_160MHZ :
			    CMD_CBW_20MHZ;
		state->vht_opmode = bw | ((nss - 1) << 4);
	}
}

int
mt7921_mcu_wtbl_generic_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct wtbl_generic *generic;
	struct wtbl_rx *rx;

	generic = (struct wtbl_generic *)mwx_append_tlv(m, tlvnum,
	    WTBL_GENERIC, sizeof(*generic));

	if (ni) {
		generic->partial_aid = htole16(IEEE80211_AID(ni->ni_associd));
		memcpy(generic->peer_addr, ni->ni_macaddr, IEEE80211_ADDR_LEN);
		generic->muar_idx = sc->sc_vif.omac_idx;
		generic->qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	} else {
		memset(generic->peer_addr, 0xff, IEEE80211_ADDR_LEN);
		generic->muar_idx = 0xe;
	}

	rx = (struct wtbl_rx *)mwx_append_tlv(m, tlvnum, WTBL_RX,
	    sizeof(*rx));
#ifndef IEEE80211_STA_ONLY
	rx->rca1 = ni ? ic->ic_opmode != IEEE80211_M_HOSTAP : 1;
#else
	rx->rca1 = 1;
#endif
	rx->rca2 = 1;
	rx->rv = 1;

	return sizeof(*generic) + sizeof(*rx);
}

int
mt7921_mcu_wtbl_hdr_trans_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct mwx_softc *sc, struct ieee80211_node *ni)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct wtbl_hdr_trans *htr;

	htr = (struct wtbl_hdr_trans *)mwx_append_tlv(m, tlvnum,
	    WTBL_HDR_TRANS, sizeof(*htr));

	/* no hdr decapsulation offload */
	htr->no_rx_trans = 1;

	if (ic->ic_opmode == IEEE80211_M_STA)
		htr->to_ds = 1;
	else
		htr->from_ds = 1;

	return sizeof(*htr);
}

int
mt7921_mcu_wtbl_ht_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct mwx_softc *sc, struct ieee80211_node *ni, int assoc)
{
	struct wtbl_smps *smps;
	struct wtbl_ht *ht = NULL;
	int len = 0;

	if (assoc && (ni->ni_flags & IEEE80211_NODE_HT)) {
		ht = (struct wtbl_ht *)mwx_append_tlv(m, tlvnum, WTBL_HT,
		    sizeof(*ht));
		ht->ht = 1;
		ht->ldpc = (ni->ni_htcaps & IEEE80211_HTCAP_LDPC) != 0;
		ht->af = ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_LE;
		ht->mm = (ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_SS) >> 2;
		len += sizeof(*ht);
	}

	if (ht != NULL && (ni->ni_flags & IEEE80211_NODE_VHT) &&
	    !IEEE80211_IS_CHAN_2GHZ(ni->ni_chan)) {
		struct wtbl_vht *vht;
		uint8_t af;

		vht = (struct wtbl_vht *)mwx_append_tlv(m, tlvnum, WTBL_VHT,
		    sizeof(*vht));
		vht->ldpc = (ni->ni_vhtcaps & IEEE80211_VHTCAP_RXLDPC) != 0;
		vht->vht = 1;
		af = (ni->ni_vhtcaps &
		    IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK) >>
		    IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT;
		ht->af = MAX(ht->af, af);
		len += sizeof(*vht);
	}

	smps = (struct wtbl_smps *)mwx_append_tlv(m, tlvnum, WTBL_SMPS,
	    sizeof(*smps));
	/* spatial multiplexing power save mode, off for now */
	//smps->smps = (sta->deflink.smps_mode == IEEE80211_SMPS_DYNAMIC);

	return len + sizeof(*smps);
}

/*
 * Delete the AP STA_REC added at AUTH. Uses the saved WCID and address
 * because ic_bss may already describe the next BSS.
 */
int
mt7921_mac_sta_remove(struct mwx_softc *sc)
{
	struct mwx_vif *mvif = &sc->sc_vif;
	struct sta_rec_basic *basic;
	struct sta_rec_wtbl *wtbl;
	struct mbuf *m;
	uint16_t tlvnum = 0, wcid = sc->sc_sta_wcid;
	int rv;

	if (wcid == 0)
		return 0;
	sc->sc_sta_wcid = 0;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;

	basic = (struct sta_rec_basic *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_BASIC, sizeof(*basic));
	basic->extra_info = htole16(EXTRA_INFO_VER);
	basic->conn_state = CONN_STATE_DISCONNECT;
	basic->conn_type = htole32(STA_TYPE_AP | NETWORK_INFRA);
	memcpy(basic->peer_addr, sc->sc_sta_addr, IEEE80211_ADDR_LEN);

	wtbl = (struct sta_rec_wtbl *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_WTBL, sizeof(*wtbl));
	wtbl->wlan_idx_lo = wcid & 0xff;
	wtbl->wlan_idx_hi = wcid >> 8;
	wtbl->operation = WTBL_RESET_AND_SET;

	m = mwx_fill_sta_req_hdr(m, mvif, mvif->omac_idx, wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;

	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
	MWX_DEV_LOG(sc, "sta remove wcid=%u rv=%d\n", wcid, rv);
	return rv;
}

int
mt7921_mac_sta_update(struct mwx_softc *sc, struct ieee80211_node *ni,
    int add, int isnew)
{
	struct mwx_node *mw = (struct mwx_node *)ni;
	struct mwx_vif *mvif = &sc->sc_vif;
	struct sta_rec_wtbl *wtbl;
	struct mbuf *m = NULL;
	uint16_t tlvnum = 0, wnum = 0;
	int wlen = 0;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;

	if (ni != NULL)
		mt7921_mcu_add_basic_tlv(m, &tlvnum, sc, ni, add, isnew);

	if (ni != NULL && add)
		mt7921_mcu_add_sta_tlv(m, &tlvnum, sc, ni, 0);

	wtbl = (struct sta_rec_wtbl *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_WTBL, sizeof(*wtbl));
	wtbl->wlan_idx_lo = mw ? mw->wcid & 0xff : 0;
	wtbl->wlan_idx_hi = mw ? mw->wcid >> 8 : 0;
	wtbl->operation = WTBL_RESET_AND_SET;

	if (add) {
		wlen += mt7921_mcu_wtbl_generic_tlv(m, &wnum, sc, ni);
		wlen += mt7921_mcu_wtbl_hdr_trans_tlv(m, &wnum, sc, ni);

		if (ni)
			wlen += mt7921_mcu_wtbl_ht_tlv(m, &wnum, sc, ni, 0);
	}

	wtbl->tlv_num = htole16(wnum);
	wtbl->len = htole16(le16toh(wtbl->len) + wlen);

	m = mwx_fill_sta_req_hdr(m, mvif, ni ? mvif->omac_idx : 0,
	    mw ? mw->wcid : 0, tlvnum);
	if (m == NULL)
		return ENOBUFS;

	return mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
}

static int
mt7921_key_to_cipher_id(struct ieee80211_key *k)
{
	switch (k->k_cipher) {
	case IEEE80211_CIPHER_CCMP:
		 return MCU_CIPHER_AES_CCMP;
	case IEEE80211_CIPHER_TKIP:
		return MCU_CIPHER_TKIP;
	case IEEE80211_CIPHER_WEP40:
		return MCU_CIPHER_WEP40;
	case IEEE80211_CIPHER_WEP104:
		 return MCU_CIPHER_WEP104;
	case IEEE80211_CIPHER_BIP:
		return MCU_CIPHER_BIP_CMAC_128;
	default:
		return MCU_CIPHER_NONE;
	}
}

void
mt7921_mcu_add_key_tlv(struct mbuf *m, uint16_t *tlvnum,
    struct ieee80211_key *k, int add)
{
	struct sta_rec_sec *sec;
	struct sta_rec_sec_key *key;

	sec = (struct sta_rec_sec *)mwx_append_tlv(m, tlvnum, STA_REC_KEY,
	    sizeof(*sec));

	if (add) {
		sec->n_cipher = 1;
		sec->add = 0;	/* connac2 has inverted logic */
		sec->len = htole16(sizeof(*sec) + sizeof(*key));

		key = (struct sta_rec_sec_key *)mwx_append_len(m, sizeof(*key));
		key->cipher_id = mt7921_key_to_cipher_id(k);
		key->cipher_len = sizeof(*key);
		key->key_id = k->k_id;
		key->key_len = k->k_len;

		KASSERT(k->k_len <= sizeof(key->key));
		memcpy(key->key, k->k_key, k->k_len);

		if (key->cipher_id == MCU_CIPHER_TKIP &&
		    k->k_len == sizeof(key->key)) {
			/* Rx/Tx MIC keys are swapped. */
			memcpy(key->key + 16, k->k_key + 24, 8);
			memcpy(key->key + 24, k->k_key + 16, 8);
		}
	} else {
		sec->add = 1;
		sec->n_cipher = 0;
	}
}

/*
 * MtkWireless: the IGTK goes to the firmware the way
 * mt76_connac_mcu_sta_key_tlv() sends it: one STA_REC_KEY with two
 * ciphers, the group CCMP key first and the BIP-CMAC-128 key second, on
 * the WCID of the group keys. Without a group CCMP key the IGTK stays
 * software only; net80211 still checks the MMIE (ieee80211_bip_decap).
 */
static int
mt7921_mcu_sta_bip_update(struct mwx_softc *sc, struct ieee80211_key *k)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mwx_vif *mvif = &sc->sc_vif;
	struct ieee80211_key *gtk = NULL;
	struct sta_rec_sec *sec;
	struct sta_rec_sec_key *key;
	struct mbuf *m;
	uint16_t tlvnum = 0;

	if (sc->sc_gtk_kid >= 1 && sc->sc_gtk_kid <= 3)
		gtk = &ic->ic_nw_keys[sc->sc_gtk_kid];
	if (gtk == NULL || gtk->k_cipher != IEEE80211_CIPHER_CCMP ||
	    !(gtk->k_flags & IEEE80211_KEY_GROUP) || gtk->k_len != 16 ||
	    k->k_len != 16) {
		MWX_DEV_LOG(sc, "IGTK id=%d kept in software: no group CCMP key\n",
		    k->k_id);
		return 0;
	}

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;
	sec = (struct sta_rec_sec *)mwx_append_tlv(m, &tlvnum, STA_REC_KEY,
	    sizeof(*sec));
	sec->add = 0;	/* connac2 has inverted logic */
	sec->n_cipher = 2;
	sec->len = htole16(sizeof(*sec) + 2 * sizeof(*key));

	key = (struct sta_rec_sec_key *)mwx_append_len(m, sizeof(*key));
	key->cipher_id = MCU_CIPHER_AES_CCMP;
	key->cipher_len = sizeof(*key);
	key->key_id = gtk->k_id;
	key->key_len = 16;
	memcpy(key->key, gtk->k_key, 16);

	key = (struct sta_rec_sec_key *)mwx_append_len(m, sizeof(*key));
	key->cipher_id = MCU_CIPHER_BIP_CMAC_128;
	key->cipher_len = sizeof(*key);
	key->key_len = 16;
	memcpy(key->key, k->k_key, 16);

	m = mwx_fill_sta_req_hdr(m, mvif, 0x0e, mvif->vif_mn.wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;
	MWX_DEV_LOG(sc, "IGTK id=%d add with GTK id=%d wcid=%u\n", k->k_id,
	    gtk->k_id, mvif->vif_mn.wcid);
	return mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
}

int
mt7921_mcu_sta_key_update(struct mwx_softc *sc, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_vif *mvif = &sc->sc_vif;
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct mbuf *m;
	int want_keymask = (MWX_NODE_FLAG_HAVE_PAIRWISE_KEY |
	    MWX_NODE_FLAG_HAVE_GROUP_KEY);
	int rv;
	uint16_t tlvnum = 0, wcid = 0;
	uint8_t muar_idx = 0;

	if (ni == NULL) {
		ni = sc->sc_ic.ic_bss;
		mn = (struct mwx_node *)ni;
	}
	if (ni == NULL)
		return ENXIO;

	if (k->k_cipher == IEEE80211_CIPHER_BIP)
		return mt7921_mcu_sta_bip_update(sc, k);

	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0) {
		wcid = mvif->vif_mn.wcid;
		muar_idx = 0x0e;
		if (k->k_cipher == IEEE80211_CIPHER_CCMP)
			sc->sc_gtk_kid = k->k_id;
	} else {
		wcid = mn->wcid;
	}

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;

	DPRINTF("%s: %s: ni %p, k_id %d, k_flags %x k_cipher %d wcid %d\n",
	    DEVNAME(sc), "add key", ni, k->k_id, k->k_flags, k->k_cipher, wcid);
	MWX_DEV_LOG(sc,
	    "key add request id=%d flags=0x%x cipher=%d len=%d wcid=%u muar=%u ni=%p\n",
	    k->k_id, k->k_flags, k->k_cipher, k->k_len, wcid, muar_idx, ni);

	mt7921_mcu_add_key_tlv(m, &tlvnum, k, 1);
	m = mwx_fill_sta_req_hdr(m, mvif, muar_idx, wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;

	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
	if (rv != 0)
		return rv;
	MWX_DEV_LOG(sc,
	    "key add done id=%d flags=0x%x cipher=%d wcid=%u node_flags_before=0x%x\n",
	    k->k_id, k->k_flags, k->k_cipher, wcid, mn->flags);

	/* Keep track of added keys to trigger link up when all are done. */
	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0)
		mn->flags |= MWX_NODE_FLAG_HAVE_GROUP_KEY;
	else
		mn->flags |= MWX_NODE_FLAG_HAVE_PAIRWISE_KEY;

	if ((mn->flags & want_keymask) == want_keymask) {
		DPRINTF("marking port %s valid\n", ether_sprintf(ni->ni_macaddr));
		MWX_DEV_LOG(sc,
		    "key add complete port valid mac=%s node_flags=0x%x\n",
		    ether_sprintf(ni->ni_macaddr), mn->flags);
		/* MtkWireless: with PMF, robust management frames are
		 * protected both ways from here on (net80211 never sets
		 * RXMGMTPROT itself). */
		if (ni->ni_flags & IEEE80211_NODE_MFP)
			ni->ni_flags |= IEEE80211_NODE_TXMGMTPROT |
			    IEEE80211_NODE_RXMGMTPROT;
		ni->ni_port_valid = 1;
		ieee80211_set_link_state(&sc->sc_ic, LINK_STATE_UP);
	}

	return 0;
}

void
mt7921_mcu_sta_key_delete(struct mwx_softc *sc, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct mwx_vif *mvif = &sc->sc_vif;
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct mbuf *m;
	uint16_t tlvnum = 0, wcid = 0;
	uint8_t muar_idx = 0;

	if (ni == NULL) {
		ni = sc->sc_ic.ic_bss;
		mn = (struct mwx_node *)ni;
	}
	if (ni == NULL)
		return;

	/*
	 * MtkWireless: a STA_REC_KEY delete carries no cipher entry, so on
	 * the group WCID it would take the GTK along with the IGTK. The
	 * IGTK goes with the group key's delete instead.
	 */
	if (k->k_cipher == IEEE80211_CIPHER_BIP)
		return;

	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0) {
		wcid = mvif->vif_mn.wcid;
		muar_idx = 0x0e;
		mn->flags &= ~MWX_NODE_FLAG_HAVE_GROUP_KEY;
		if (k->k_id == sc->sc_gtk_kid)
			sc->sc_gtk_kid = 0;
	} else {
		wcid = mn->wcid;
		mn->flags &= ~MWX_NODE_FLAG_HAVE_PAIRWISE_KEY;
	}

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return;

	DPRINTF("%s: %s: ni %p, k_id %d, k_flags %x k_cipher %d wcid %d\n",
	    DEVNAME(sc), "delete key", ni, k->k_id, k->k_flags, k->k_cipher,
	    wcid);

	mt7921_mcu_add_key_tlv(m, &tlvnum, k, 0);
	m = mwx_fill_sta_req_hdr(m, mvif, muar_idx, wcid, tlvnum);
	if (m == NULL)
		return;

	mwx_mcu_send_mbuf(sc, MCU_UNI_CMD_STA_REC_UPDATE, m, NULL);
}

#undef free

bool
MtkMwx::mwx_match(IOPCIDevice *device)
{
	if (device == NULL) {
		MWX_LOG("probe skipped: null PCI device\n");
		return false;
	}
	uint16_t vendor = device->configRead16(kIOPCIConfigVendorID);
	uint16_t product = device->configRead16(kIOPCIConfigDeviceID);
	bool matched = pci_matchbyid(vendor, product, mwx_devices,
	    nitems(mwx_devices)) != 0;
	MWX_LOG("probe vendor=0x%04x product=0x%04x matched=%d\n",
	    vendor, product, matched);
	return matched;
}

bool
MtkMwx::attach(IOPCIDevice *device)
{
	MWX_LOG("HAL attach begin device=%p\n", (void *)device);
	if (com == NULL)
		com = (struct mwx_softc *)malloc(sizeof(*com), M_DEVBUF, M_WAITOK | M_ZERO);
	if (pci == NULL)
		pci = (struct pci_attach_args *)malloc(sizeof(*pci), M_DEVBUF, M_WAITOK | M_ZERO);
	if (com == NULL || pci == NULL) {
		MWX_LOG("HAL attach allocation failed com=%p pci=%p\n",
		    (void *)com, (void *)pci);
		releaseAll();
		return false;
	}
	MWX_LOG("HAL attach allocated com=%p pci=%p\n",
	    (void *)com, (void *)pci);

	if (!taskq_init()) {
		releaseAll();
		return false;
	}
	taskQueueOwned = true;
	gMwxHal = this;
	pci->pa_tag = device;
	pci->workloop = getMainWorkLoop();
	pci->pa_id = ((pcireg_t)device->configRead16(kIOPCIConfigDeviceID) << 16) |
	    device->configRead16(kIOPCIConfigVendorID);
	strlcpy(com->sc_dev.dv_xname, "mwx0", sizeof(com->sc_dev.dv_xname));
	MWX_LOG("HAL attach pci_id=0x%08x workloop=%p\n",
	    pci->pa_id, (void *)pci->workloop);
	if (!mwx_attach(NULL, (struct device *)com, pci)) {
		MWX_LOG("HAL attach failed inside mwx_attach\n");
		detach(device);
		return false;
	}
	MWX_LOG("HAL attach complete\n");
	return true;
}

void
MtkMwx::detach(IOPCIDevice *device)
{
    MWX_LOG("HAL detach begin device=%p com=%p\n", (void *)device, (void *)com);
    if (com == NULL) {
        if (taskQueueOwned) { taskq_destroy(systq); taskQueueOwned = false; }
        releaseAll();
        return;
    }
    struct ifnet *ifp = &com->sc_ic.ic_if;
    com->sc_detaching = true;
    mwx_awdl_quiesce(com);
    timeout_del(&com->sc_reset_to);
    timeout_del(&com->sc_scan_to);
    timeout_del(&com->sc_dma_to);
    /* Keep interrupts alive while in-flight MCU requests finish. */
    taskq_quiesce(com->sc_nswq);
    if (taskQueueOwned) taskq_quiesce(systq);
    if (ifp->if_flags & IFF_RUNNING) mwx_stop(ifp);
    timeout_free(&com->sc_reset_to);
    timeout_free(&com->sc_scan_to);
    timeout_free(&com->sc_dma_to);
    /* No interrupt callback may access buffers during their destruction. */
    if (com->sc_ih) {
        pci_intr_disestablish(pci->pa_pc, com->sc_ih);
        com->sc_ih = NULL;
    }
    if (com->sc_memh) {
        mwx_write(com, MT_WFDMA0_HOST_INT_ENA, 0);
        mwx_dma_disable(com, 1);
    }
    if (com->sc_net_attached) {
        ieee80211_ifdetach(ifp);
        com->sc_net_attached = false;
    }
    taskq_destroy(com->sc_nswq);
    com->sc_nswq = NULL;
    if (taskQueueOwned) { taskq_destroy(systq); taskQueueOwned = false; }
    mwx_txwi_free(com);
    mwx_dma_free(com);
    if (com->sc_memh) {
        bus_space_unmap(com->sc_st, com->sc_memh, com->sc_mems);
        com->sc_memh = 0;
    }
    releaseAll();
    MWX_LOG("HAL detach complete\n");
}

IOReturn
MtkMwx::enable(IONetworkInterface *interface)
{
	MWX_LOG("HAL enable begin interface=%p com=%p\n",
	    (void *)interface, (void *)com);
	if (com == NULL) {
		MWX_LOG("HAL enable failed: not attached\n");
		return kIOReturnNotAttached;
	}
	struct ifnet *ifp = &com->sc_ic.ic_if;
	if (ifp->if_flags & IFF_UP) {
		MWX_DEV_LOG(com, "HAL enable skipped: already up flags=0x%x\n",
		    ifp->if_flags);
		return kIOReturnSuccess;
	}
	ifp->if_flags |= IFF_UP;
	OSIncrementAtomic(&com->sc_power_req);
	task_add(com->sc_nswq, &com->sc_power_task);
	MWX_DEV_LOG(com, "HAL enable queued flags=0x%x\n", ifp->if_flags);
	return kIOReturnSuccess;
}

IOReturn
MtkMwx::disable(IONetworkInterface *interface)
{
	MWX_LOG("HAL disable begin interface=%p com=%p\n",
	    (void *)interface, (void *)com);
	if (com == NULL)
		return kIOReturnSuccess;
	struct ifnet *ifp = &com->sc_ic.ic_if;
	if (!(ifp->if_flags & IFF_UP)) {
		MWX_DEV_LOG(com, "HAL disable skipped: already down flags=0x%x\n",
		    ifp->if_flags);
		return kIOReturnSuccess;
	}
	ifp->if_flags &= ~IFF_UP;
	OSIncrementAtomic(&com->sc_power_req);
	task_add(com->sc_nswq, &com->sc_power_task);
	MWX_DEV_LOG(com, "HAL disable queued flags=0x%x\n", ifp->if_flags);
	return kIOReturnSuccess;
}

struct ieee80211com *
MtkMwx::get80211Controller()
{
	if (com == NULL)
		com = (struct mwx_softc *)malloc(sizeof(*com), M_DEVBUF, M_WAITOK | M_ZERO);
	return com == NULL ? NULL : &com->sc_ic;
}

MtkDriverInfo *
MtkMwx::getDriverInfo()
{
	return this;
}

MtkDriverController *
MtkMwx::getDriverController()
{
	return this;
}

IOEthernetController *
MtkMwx::mwxController()
{
	return getController();
}

int
MtkMwx::mwxSleepNsec(void *ident, int priority, const char *wmesg,
    uint64_t timo)
{
    // Native AWDL requests arrive with the controller gate held. msleep
    // would retain that gate and block the interrupt delivering the reply.
    if (getMainWorkLoop() && getMainWorkLoop()->inGate() && getMainCommandGate()) {
        AbsoluteTime deadline;
        clock_interval_to_deadline((uint32_t)MIN(timo,UINT32_MAX), kNanosecondScale,
            reinterpret_cast<uint64_t *>(&deadline));
        IOReturn result = getMainCommandGate()->commandSleep(ident, deadline, THREAD_UNINT);
        return result == THREAD_AWAKENED ? 0 : EWOULDBLOCK;
    }
	return MtkHalService::tsleep_nsec(ident, priority, wmesg, timo);
}

void
MtkMwx::mwxWakeup(void *ident)
{
	wakeupOn(ident);
    // Wake native commandSleep and the ordinary HAL msleep callers.
    if(getMainCommandGate()) getMainCommandGate()->commandWakeup(ident,false);
}

IOReturn
MtkMwx::waitPowerTransition()
{
	if (!com || com->sc_detaching)
		return kIOReturnNotAttached;
	/* commandSleep drops a caller's gate so the task and its MCU replies
	 * can progress. Bound this wait below the PM acknowledgement budget.
	 */
	SInt32 requested = com->sc_power_req;
	for (unsigned i = 0; i < 400; i++) {
		OSSynchronizeIO();
		if (com->sc_power_done == requested) {
			return com->sc_power_result ? kIOReturnIOError : kIOReturnSuccess;
		}
		mwxSleepNsec(com, 0, "mwx-power", MSEC_TO_NSEC(10));
		if (com->sc_detaching)
			return kIOReturnNotAttached;
	}
	return kIOReturnTimeout;
}

const char *
MtkMwx::getFirmwareVersion()
{
	if (com == NULL)
		return "MT792x";
	return com->sc_hwtype == MWX_HW_MT7922 ? "MT7922" : "MT7921";
}

int16_t
MtkMwx::getBSSNoise()
{
	return 0;
}

bool
MtkMwx::is5GBandSupport()
{
	return com != NULL && com->sc_capa.has_5ghz;
}

int
MtkMwx::getTxNSS()
{
	return (com != NULL && com->sc_capa.num_streams > 0) ? com->sc_capa.num_streams : 1;
}

const char *
MtkMwx::getFirmwareName()
{
	if (com == NULL || com->sc_hwtype == MWX_HW_MT7922)
		return MT7922_FIRMWARE_WM;
	return MT7921_FIRMWARE_WM;
}

UInt32
MtkMwx::supportedFeatures()
{
	return kIONetworkFeatureMultiPages;
}

const char *
MtkMwx::getFirmwareCountryCode()
{
	static char alpha2[3] = "00";
	if (com != NULL) {
		alpha2[0] = com->sc_alpha2[0];
		alpha2[1] = com->sc_alpha2[1];
		alpha2[2] = '\0';
	}
	return alpha2;
}

uint32_t
MtkMwx::getTxQueueSize()
{
	return 256;
}

void
MtkMwx::clearScanningFlags()
{
	if (com) {
		com->sc_flags &= ~(MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN);
		com->sc_ic.ic_wcl_scan_inflight = false;
	}
}

void
MtkMwx::fillDebug(void *out)
{
	struct ioctl_debug *d = (struct ioctl_debug *)out;
	struct mwx_softc *sc = com;

	d->version = IOCTL_VERSION;
	if (sc == NULL)
		return;
	d->ic_state = sc->sc_ic.ic_state;
	d->sc_flags = sc->sc_flags;
	d->intr_count = sc->sc_intr_count;
	d->mcu_events = sc->sc_mcu_events;
	d->last_mcu_eid = sc->sc_last_mcu_eid;
	d->mcu_bad_seq = sc->sc_mcu_bad_seq;
	d->mcu_timeouts = sc->sc_mcu_timeouts;
	d->scan_requests = sc->sc_scan_requests;
	d->scan_done_count = sc->sc_scan_done_count;
	d->scan_watchdog_fired = sc->sc_scan_watchdog_fired;
	d->rx_pkt_normal = sc->sc_rx_pkt_normal;
	d->rx_pkt_unknown = sc->sc_rx_pkt_unknown;
	d->rx_fill_fail = sc->sc_rx_fill_fail;
	d->lpctl = mwx_read(sc, MT_CONN_ON_LPCTL);
	d->int_sta = mwx_read(sc, MT_WFDMA0_HOST_INT_STA);
	d->int_ena = mwx_read(sc, MT_WFDMA0_HOST_INT_ENA);
#define MWX_DBG_Q(q, p) do { \
	d->p##_cpu = mwx_read(sc, (q).mq_regbase + MT_DMA_CPU_IDX); \
	d->p##_dma = mwx_read(sc, (q).mq_regbase + MT_DMA_DMA_IDX); \
	d->p##_cons = (q).mq_cons; \
} while (0)
	MWX_DBG_Q(sc->sc_rxmcuq, rxmcu);
	MWX_DBG_Q(sc->sc_rxfwdlq, rxfwdl);
	MWX_DBG_Q(sc->sc_rxq, rxdata);
	MWX_DBG_Q(sc->sc_txmcuq, txmcu);
#undef MWX_DBG_Q
	d->nodes = mwx_count_nodes(&sc->sc_ic);
	d->intr_index = mtk_compat_intr_index;
	d->intr_is_msi = mtk_compat_intr_is_msi;

	struct ieee80211_node *ni = sc->sc_ic.ic_bss;
	if (ni != NULL) {
		d->port_valid = ni->ni_port_valid;
		d->node_flags = ((struct mwx_node *)ni)->flags;
		d->wcid = ((struct mwx_node *)ni)->wcid;
		d->rsn_supp_state = ni->ni_rsn_supp_state;
	}
	d->txdata_cpu = mwx_read(sc, sc->sc_txq.mq_regbase + MT_DMA_CPU_IDX);
	d->txdata_dma = mwx_read(sc, sc->sc_txq.mq_regbase + MT_DMA_DMA_IDX);
	d->txdata_cons = sc->sc_txq.mq_cons;
	d->rx_delivered = sc->sc_rx_delivered;
	d->rx_normal_mcu = sc->sc_rx_pkt_normal_mcu;
	d->rx_event = sc->sc_rx_pkt_event;
	d->rx_notify = sc->sc_rx_pkt_txrx_notify;
	d->if_ierrors = sc->sc_ic.ic_if.if_ierrors;
	d->if_oerrors = sc->sc_ic.ic_if.if_oerrors;

	d->aspm_lnkctl_before = mtk_aspm_lnkctl_before;
	d->aspm_lnkctl_after = mtk_aspm_lnkctl_after;
	d->aspm_l1ss_before = mtk_aspm_l1ss_before;
	d->aspm_l1ss_after = mtk_aspm_l1ss_after;
	d->aspm_parent_l1ss_before = mtk_aspm_parent_l1ss_before;
	d->glo_cfg = mwx_read(sc, MT_WFDMA0_GLO_CFG);
	d->dummy_cr = mwx_read(sc, MT_WFDMA_DUMMY_CR);
	d->mcu_cmd = mwx_read(sc, MT_MCU_CMD);
	d->knobs = sc->sc_knobs;
	d->dma_stalls = sc->sc_dma_stalls;
	d->full_resets = sc->sc_full_resets;
	d->mcu_cmd_irqs = sc->sc_mcu_cmd_irqs;
	d->last_mcu_cmd = sc->sc_last_mcu_cmd;

	struct ieee80211_stats *st = &sc->sc_ic.ic_stats;
	d->rx_ctl = sc->sc_rx_ctl;
	d->rx_bar = sc->sc_rx_bar;
	d->rx_dup = st->is_rx_dup;
	d->ba_agreements = st->is_ht_rx_ba_agreements;
	d->ba_below_winstart = st->is_ht_rx_frame_below_ba_winstart;
	d->ba_above_winend = st->is_ht_rx_frame_above_ba_winend;
	d->ba_window_slide = st->is_ht_rx_ba_window_slide;
	d->ba_window_jump = st->is_ht_rx_ba_window_jump;
	d->ba_no_buf = st->is_ht_rx_ba_no_buf;
	d->ba_frame_lost = st->is_ht_rx_ba_frame_lost;
	d->ba_gap_timeout = st->is_ht_rx_ba_window_gap_timeout;
	d->ba_gap_timeout_ms = sc->sc_ic.ic_ba_gap_timeout_ms > 0 ?
	    sc->sc_ic.ic_ba_gap_timeout_ms : IEEE80211_BA_GAP_TIMEOUT;
	d->rx_batches = sc->sc_rx_batches;
	d->tx_stalls = sc->sc_tx_stalls;
	d->tx_nobufs = sc->sc_tx_nobufs;
	d->tx_ring_full = sc->sc_tx_ring_full;
	d->txwi_inuse = sc->sc_txwi.mt_inuse;
	d->txwi_peak = sc->sc_txwi.mt_peak;
	d->tx_oactive = ifq_is_oactive(&sc->sc_ic.ic_if.if_snd);
	d->pci_status = pci_conf_read(sc->sc_pc, sc->sc_tag, 0x04);

	/* The descriptor the TX data DMA engine is stuck on, if any. */
	uint32_t idx = d->txdata_dma % sc->sc_txq.mq_count;
	struct mwx_desc *desc = &sc->sc_txq.mq_desc[idx];
	d->txdata_desc[0] = le32toh(desc->buf0);
	d->txdata_desc[1] = le32toh(desc->ctrl);
	d->txdata_desc[2] = le32toh(desc->buf1);
	d->txdata_desc[3] = le32toh(desc->info);
	struct mwx_txwi *mt = sc->sc_txq.mq_data[idx].md_txwi;
	if (mt != NULL && mt->mt_desc != NULL) {
		struct mt76_connac_txp_ptr *ptr = &mt->mt_desc->ptr[0];
		d->txdata_txp[0] = le32toh(ptr->buf0);
		d->txdata_txp[1] = le16toh(ptr->len0) | (le16toh(ptr->len1) << 16);
		d->txdata_txp[2] = le32toh(ptr->buf1);
		d->txdata_txp[3] = mt->mt_idx;
	}
}

IOReturn
MtkMwx::debugSet(uint32_t op, uint32_t val)
{
	struct mwx_softc *sc = com;

	if (sc == NULL)
		return kIOReturnNotReady;
	switch (op) {
	case IOCTL_DEBUG_OP_KNOBS:
		MWX_DEV_LOG(sc, "knobs 0x%x -> 0x%x\n", sc->sc_knobs, val);
		sc->sc_knobs = val;
		/* net80211 checks this before each ADDBA request. */
		if (val & MWX_KNOB_NO_TXBA)
			sc->sc_ic.ic_caps &= ~IEEE80211_C_TX_AMPDU;
		else
			sc->sc_ic.ic_caps |= IEEE80211_C_TX_AMPDU;
		return kIOReturnSuccess;
	case IOCTL_DEBUG_OP_RESET:
		if (sc->sc_detaching)
			return kIOReturnNotReady;
		task_add(systq, &sc->sc_fullreset_task);
		return kIOReturnSuccess;
	case IOCTL_DEBUG_OP_DUMP:
		mwx_dump_dma_state(sc, "user");
		return kIOReturnSuccess;
	case IOCTL_DEBUG_OP_BA_GAP:
		if (val > 1000)
			return kIOReturnBadArgument;
		MWX_DEV_LOG(sc, "ba gap timeout %d -> %u ms\n",
		    sc->sc_ic.ic_ba_gap_timeout_ms, val);
		sc->sc_ic.ic_ba_gap_timeout_ms = val;
		return kIOReturnSuccess;
	case IOCTL_DEBUG_OP_BGSCAN_DWELL:
		if (val > 200 && val != MWX_BGSCAN_DWELL_AUTO)
			return kIOReturnBadArgument;
		MWX_DEV_LOG(sc, "bgscan dwell %u -> %u ms\n",
		    sc->sc_bgscan_dwell, val);
		sc->sc_bgscan_dwell = val;
		return kIOReturnSuccess;
	default:
		return kIOReturnBadArgument;
	}
}

IOReturn
MtkMwx::setMulticastList(IOEthernetAddress *addr, int count)
{
	return kIOReturnSuccess;
}

void
MtkMwx::releaseAll()
{
	MWX_LOG("HAL releaseAll com=%p pci=%p\n", (void *)com, (void *)pci);
	if (com) {
        mwx_awdl_quiesce(com);
        if (com->sc_awdl_lock) IOLockFree(com->sc_awdl_lock);
		mwx_free_mem(com);
		com = NULL;
	}
	if (pci) {
		mwx_free_mem(pci);
		pci = NULL;
	}
	if (gMwxHal == this)
		gMwxHal = NULL;
}

void
MtkMwx::free()
	{
		MWX_LOG("HAL free\n");
		releaseAll();
		MtkHalService::free();
	}

static int mwx_send_assoc_status(struct mwx_softc *sc, uint32_t cmd, void *data, size_t len)
{
    int seq, rv;
    uint32_t status = 0;
    rv = mwx_mcu_send_msg(sc, cmd, data, len, &seq);
    if (rv) return rv;
    rv = mwx_mcu_wait_resp_int(sc, cmd, seq, &status);
    MWX_DEV_LOG(sc, "assoc_status cmd=%x transport=%d firmware=%u\n", cmd, rv, status);
    return rv ? rv : (status ? EIO : 0);
}

/*
 * Publish the associated peer with its negotiated HT/VHT parameters.
 * The WTBL is updated with WTBL_SET so the entry and any installed keys
 * survive; only the generic (QoS) and HT/VHT fields are rewritten.
 */
int
mt7921_mcu_set_associated_peer(struct mwx_softc *sc)
{
	struct ieee80211_node *ni = sc->sc_ic.ic_bss;
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct sta_rec_wtbl *wtbl;
	struct mbuf *m;
	uint16_t tlvnum = 0, wnum = 0;
	uint32_t status = 0;
	int wlen = 0, seq, rv;

	if (!ni || IEEE80211_AID(ni->ni_associd) == 0)
		return EINVAL;
	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (!m)
		return ENOBUFS;

	mt7921_mcu_add_basic_tlv(m, &tlvnum, sc, ni, 1, 0);
	if (sc->sc_knobs & MWX_KNOB_OLD_PEER) {
		struct sta_rec_state *state;

		state = (struct sta_rec_state *)mwx_append_tlv(m, &tlvnum,
		    STA_REC_STATE, sizeof(*state));
		state->state = 2;
		goto send;
	}
	mt7921_mcu_add_sta_tlv(m, &tlvnum, sc, ni, 1);

	wtbl = (struct sta_rec_wtbl *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_WTBL, sizeof(*wtbl));
	wtbl->wlan_idx_lo = mn->wcid & 0xff;
	wtbl->wlan_idx_hi = mn->wcid >> 8;
	wtbl->operation = WTBL_SET;
	wlen += mt7921_mcu_wtbl_generic_tlv(m, &wnum, sc, ni);
	wlen += mt7921_mcu_wtbl_ht_tlv(m, &wnum, sc, ni, 1);
	wtbl->tlv_num = htole16(wnum);
	wtbl->len = htole16(le16toh(wtbl->len) + wlen);

send:
	m = mwx_fill_sta_req_hdr(m, &sc->sc_vif, sc->sc_vif.omac_idx,
	    mn->wcid, tlvnum);
	if (!m)
		return ENOBUFS;
	rv = mwx_mcu_send_mbuf(sc, MCU_UNI_CMD_STA_REC_UPDATE, m, &seq);
	if (!rv)
		rv = mwx_mcu_wait_resp_int(sc, MCU_UNI_CMD_STA_REC_UPDATE, seq,
		    &status);
	MWX_DEV_LOG(sc, "assoc_peer aid=%u flags=0x%x htcaps=0x%04x ampdu=0x%02x "
	    "rxmcs=%02x%02x vhtcaps=0x%08x vhtmcs=0x%04x/0x%04x chw=%u nss=%u "
	    "bw=%u center=%u tlvs=%u/%u transport=%d firmware=%u\n",
	    IEEE80211_AID(ni->ni_associd), ni->ni_flags, ni->ni_htcaps,
	    ni->ni_ampdu_param, ni->ni_rxmcs[1], ni->ni_rxmcs[0],
	    ni->ni_vhtcaps, le16toh(ni->ni_vht_mcsinfo.rx_mcs_map),
	    le16toh(ni->ni_vht_mcsinfo.tx_mcs_map), ni->ni_chw, ni->ni_rx_nss,
	    sc->sc_bss_bw, sc->sc_bss_center, tlvnum, wnum, rv, status);
	return rv ? rv : (status ? EIO : 0);
}

int
mt7921_mcu_set_associated_bss(struct mwx_softc *sc, int associated)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct mwx_vif *mvif = &sc->sc_vif;
	struct {
		uint8_t bss_idx;
		uint8_t pad[3];
		struct mt76_connac_bss_basic_tlv basic;
		struct {
			uint16_t tag, len;
			uint8_t qos, pad[3];
		} __packed qos;
	} __packed bss = {};
	struct {
		uint8_t bss_idx;
		uint8_t pad[3];
		struct {
			uint16_t tag, len;
			uint8_t control_channel, center_chan, center_chan2, bw;
			uint8_t tx_streams, rx_streams, short_st, ht_op_info;
			uint8_t sco, band, pad[2];
		} __packed rlm;
	} __packed channel = {};
	int rv, is_2g, ht, vht, he;

	if (ic->ic_opmode != IEEE80211_M_STA)
		return 0;
	if (ni == NULL || ni->ni_chan == NULL ||
	    ni->ni_chan == IEEE80211_CHAN_ANYC)
		return EINVAL;
	is_2g = IEEE80211_IS_CHAN_2GHZ(ni->ni_chan);
	ht = (ni->ni_flags & IEEE80211_NODE_HT) != 0;
	vht = !is_2g && (ni->ni_flags & IEEE80211_NODE_VHT) != 0;
	he = ht && (ic->ic_flags & IEEE80211_F_HEON) != 0 &&
	    (ni->ni_flags & IEEE80211_NODE_HE) != 0;
	bss.bss_idx = mvif->idx;
	bss.basic.tag = htole16(UNI_BSS_INFO_BASIC);
	bss.basic.len = htole16(sizeof(bss.basic));
	bss.basic.active = 1;
	bss.basic.omac_idx = mvif->omac_idx;
	bss.basic.hw_bss_idx = mvif->omac_idx > EXT_BSSID_START ?
	    HW_BSSID_0 : mvif->omac_idx;
	bss.basic.band_idx = mvif->band_idx;
	bss.basic.conn_type = htole32(STA_TYPE_STA | NETWORK_INFRA);
	bss.basic.conn_state = associated ? 0 : 1;
	bss.basic.wmm_idx = mvif->wmm_idx;
	memcpy(bss.basic.bssid, ni->ni_bssid, ETHER_ADDR_LEN);
	bss.basic.bmc_tx_wlan_idx = htole16(mvif->vif_mn.wcid);
	bss.basic.sta_idx = htole16(mvif->vif_mn.wcid);
	bss.basic.bcn_interval = htole16(ni->ni_intval);
	bss.basic.dtim_period = ni->ni_dtimperiod;
	/* BSS phymode uses PHY_MODE bits, nonht_basic_phy PHY_TYPE bits. */
	if (is_2g)
		bss.basic.phymode = PHY_MODE_B | PHY_MODE_G |
		    (ht ? PHY_MODE_GN : 0) | (he ? PHY_MODE_AX_24G : 0);
	else
		bss.basic.phymode = PHY_MODE_A | (ht ? PHY_MODE_AN : 0) |
		    (vht ? PHY_MODE_AC : 0) | (he ? PHY_MODE_AX_5G : 0);
	bss.basic.nonht_basic_phy = htole16(is_2g ?
	    PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP : PHY_TYPE_BIT_OFDM);
	bss.qos.tag = htole16(UNI_BSS_INFO_QBSS);
	bss.qos.len = htole16(sizeof(bss.qos));
	bss.qos.qos = associated && (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	if (sc->sc_knobs & MWX_KNOB_NO_QBSS) {
		bss.qos.qos = 0;
		rv = mwx_send_assoc_status(sc, MCU_UNI_CMD_BSS_INFO_UPDATE,
		    &bss, sizeof(bss) - sizeof(bss.qos));
	} else
		rv = mwx_send_assoc_status(sc, MCU_UNI_CMD_BSS_INFO_UPDATE,
		    &bss, sizeof(bss));
	MWX_DEV_LOG(sc, "associated BSS enable=%d channel=%u beacon=%u dtim=%u phymode=0x%x qos=%u rv=%d\n",
	    associated, ieee80211_mhz2ieee(ni->ni_chan->ic_freq, 0), ni->ni_intval,
	    ni->ni_dtimperiod, bss.basic.phymode, bss.qos.qos, rv);
	if (rv != 0 || !associated)
		return rv;

	/* Also clears the BSS color left by an earlier HE association. */
	if (he || sc->sc_bss_he) {
		rv = mt7921_mcu_set_bss_he(sc, ni, he);
		sc->sc_bss_he = he && rv == 0;
	}

	channel.bss_idx = mvif->idx;
	channel.rlm.tag = htole16(UNI_BSS_INFO_RLM);
	channel.rlm.len = htole16(sizeof(channel.rlm));
	channel.rlm.control_channel = ieee80211_mhz2ieee(ni->ni_chan->ic_freq, 0);
	channel.rlm.center_chan = channel.rlm.control_channel;
	channel.rlm.bw = CMD_CBW_20MHZ;
	if (sc->sc_bss_bw != CMD_CBW_20MHZ) {
		channel.rlm.bw = sc->sc_bss_bw;
		channel.rlm.center_chan = sc->sc_bss_center;
		channel.rlm.ht_op_info = 4;	/* HT 40M allowed */
	}
	if (channel.rlm.control_channel < channel.rlm.center_chan)
		channel.rlm.sco = 1;	/* SCA */
	else if (channel.rlm.control_channel > channel.rlm.center_chan)
		channel.rlm.sco = 3;	/* SCB */
	channel.rlm.tx_streams = sc->sc_capa.num_streams;
	channel.rlm.rx_streams = sc->sc_capa.antenna_mask;
	channel.rlm.short_st = 1;
	channel.rlm.band = is_2g ? 0 : 1;
	rv = mwx_send_assoc_status(sc, MCU_UNI_CMD_BSS_INFO_UPDATE,
	    &channel, sizeof(channel));
	MWX_DEV_LOG(sc, "associated BSS RLM channel=%u center=%u bw=%u sco=%u band=%u rv=%d\n",
	    channel.rlm.control_channel, channel.rlm.center_chan,
	    channel.rlm.bw, channel.rlm.sco, channel.rlm.band, rv);
	return rv;
}

/*
 * HE BSS parameters from the AP's HE Operation element, as in
 * mt76_connac_mcu_uni_add_bss(): PE duration, TXOP RTS threshold, our
 * TX MCS maps and the BSS color. enable=0 turns the BSS color off.
 */
int
mt7921_mcu_set_bss_he(struct mwx_softc *sc, struct ieee80211_node *ni,
    int enable)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct {
		uint8_t bss_idx;
		uint8_t pad[3];
		struct bss_info_uni_he he;
		struct bss_info_uni_bss_color bss_color;
	} __packed req = {};
	uint32_t params = enable ? ni->ni_he_oper_params : 0;
	int rv;

	req.bss_idx = sc->sc_vif.idx;
	req.he.tag = htole16(UNI_BSS_INFO_HE_BASIC);
	req.he.len = htole16(sizeof(req.he));
	req.he.he_pe_duration =
	    params & IEEE80211_HE_OPERATION_DFLT_PE_DURATION_MASK;
	if (req.he.he_pe_duration == 0)
		req.he.he_pe_duration = DEFAULT_HE_PE_DURATION;
	req.he.he_rts_thres = htole16((params &
	    IEEE80211_HE_OPERATION_RTS_THRESHOLD_MASK) >>
	    IEEE80211_HE_OPERATION_RTS_THRESHOLD_OFFSET);
	if (req.he.he_rts_thres == 0)
		req.he.he_rts_thres = htole16(DEFAULT_HE_DURATION_RTS_THRES);
	/* Already little endian, unsupported widths are 0xffff. */
	req.he.max_nss_mcs[CMD_HE_MCS_BW80] = ic->ic_he_mcs_nss_supp.tx_mcs_80;
	req.he.max_nss_mcs[CMD_HE_MCS_BW160] =
	    ic->ic_he_mcs_nss_supp.tx_mcs_160;
	req.he.max_nss_mcs[CMD_HE_MCS_BW8080] =
	    ic->ic_he_mcs_nss_supp.tx_mcs_80p80;

	req.bss_color.tag = htole16(UNI_BSS_INFO_BSS_COLOR);
	req.bss_color.len = htole16(sizeof(req.bss_color));
	if (enable && (params & IEEE80211_HE_OPERATION_BSS_COLOR_DISABLED) == 0) {
		req.bss_color.enable = 1;
		req.bss_color.bss_color = (params &
		    IEEE80211_HE_OPERATION_BSS_COLOR_MASK) >>
		    IEEE80211_HE_OPERATION_BSS_COLOR_OFFSET;
	}

	rv = mwx_send_assoc_status(sc, MCU_UNI_CMD_BSS_INFO_UPDATE,
	    &req, sizeof(req));
	MWX_DEV_LOG(sc, "associated BSS HE enable=%d oper=0x%08x pe=%u rts=%u "
	    "color=%u/%u rv=%d\n", enable, params, req.he.he_pe_duration,
	    le16toh(req.he.he_rts_thres), req.bss_color.enable,
	    req.bss_color.bss_color, rv);
	return rv;
}

/*
 * HE (802.11ax) station capabilities, offered unless the amtk_he=0
 * boot-arg clears IEEE80211_F_HEON in ic_userflags (MtkWireless: on by
 * default; MWX_KNOB_NO_HE turns it off at runtime). A conservative profile
 * like itlwm's: no TWT, OMI, STBC, beamforming, PPE thresholds or
 * A-MSDU in A-MPDU, and no 160 MHz since VHT does not offer it either.
 * mwx_apply_phy_knobs() sets the channel width bits for each join.
 */
void
mwx_setup_he_caps(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_he_cap_elem *he = &ic->ic_he_cap_elem;
	uint16_t map = 0xffff;
	int i;

	ic->ic_caps &= ~IEEE80211_C_HE;
	memset(he, 0, sizeof(*he));
	memset(&ic->ic_he_mcs_nss_supp, 0xff, sizeof(ic->ic_he_mcs_nss_supp));
	memset(ic->ic_ppe_thres, 0, sizeof(ic->ic_ppe_thres));
	if ((ic->ic_userflags & IEEE80211_F_HEON) == 0)
		return;

	he->mac_cap_info[0] = IEEE80211_HE_MAC_CAP0_HTC_HE;
	he->mac_cap_info[1] = IEEE80211_HE_MAC_CAP1_TF_MAC_PAD_DUR_16US;
	he->phy_cap_info[1] = IEEE80211_HE_PHY_CAP1_DEVICE_CLASS_A |
	    IEEE80211_HE_PHY_CAP1_LDPC_CODING_IN_PAYLOAD;
	he->phy_cap_info[2] = IEEE80211_HE_PHY_CAP2_NDP_4x_LTF_AND_3_2US;
	he->phy_cap_info[7] =
	    IEEE80211_HE_PHY_CAP7_HE_SU_MU_PPDU_4XLTF_AND_08_US_GI;
	/* 16us padding for every RU, so no PPE Thresholds field. */
	he->phy_cap_info[9] = IEEE80211_HE_PHY_CAP9_NOMIMAL_PKT_PADDING_16US |
	    IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_COMP_SIGB |
	    IEEE80211_HE_PHY_CAP9_RX_FULL_BW_SU_USING_MU_WITH_NON_COMP_SIGB;

	/* HE MCS 0-11 for each spatial stream, like VHT in mwx_preinit(). */
	for (i = 0; i < MAX(1, MIN(sc->sc_capa.num_streams, 2)); i++) {
		map &= ~(0x3 << (2 * i));
		map |= IEEE80211_HE_MCS_SUPPORT_0_11 << (2 * i);
	}
	ic->ic_he_mcs_nss_supp.rx_mcs_80 = htole16(map);
	ic->ic_he_mcs_nss_supp.tx_mcs_80 = htole16(map);
	ic->ic_caps |= IEEE80211_C_HE;
	MWX_DEV_LOG(sc, "HE capability offered: mcs80=0x%04x\n",
	    map);
}

/*
 * Select what the next association offers. Knob changes take effect on
 * the next join; net80211 reads these flags when it builds the
 * association request and negotiates HT/VHT from the response.
 */
void
mwx_apply_phy_knobs(struct mwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t *he0;

	ic->ic_flags &= ~(IEEE80211_F_HTON | IEEE80211_F_VHTON |
	    IEEE80211_F_HEON);
	ic->ic_htcaps |= IEEE80211_HTCAP_CBW20_40 | IEEE80211_HTCAP_SGI40;
	if (sc->sc_knobs & MWX_KNOB_NO_40)
		ic->ic_htcaps &= ~(IEEE80211_HTCAP_CBW20_40 |
		    IEEE80211_HTCAP_SGI40);

	if ((sc->sc_knobs & MWX_KNOB_NO_HT) == 0 &&
	    (ic->ic_modecaps & (1 << IEEE80211_MODE_11N))) {
		ic->ic_flags |= IEEE80211_F_HTON;
		/* VHT requires 80 MHz support, so NO_40 disables it too. */
		if ((sc->sc_knobs & (MWX_KNOB_NO_VHT | MWX_KNOB_NO_40)) == 0 &&
		    (ic->ic_modecaps & (1 << IEEE80211_MODE_11AC)))
			ic->ic_flags |= IEEE80211_F_VHTON;
		/* net80211 adds the HE Capabilities element when HEON. */
		if ((sc->sc_knobs & MWX_KNOB_NO_HE) == 0 &&
		    (ic->ic_caps & IEEE80211_C_HE) &&
		    (ic->ic_modecaps & (1 << IEEE80211_MODE_11AX)))
			ic->ic_flags |= IEEE80211_F_HEON;
	}
	he0 = &ic->ic_he_cap_elem.phy_cap_info[0];
	*he0 &= ~(IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G |
	    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G);
	if ((ic->ic_flags & IEEE80211_F_HEON) &&
	    (sc->sc_knobs & MWX_KNOB_NO_40) == 0) {
		if (sc->sc_capa.has_2ghz)
			*he0 |= IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G;
		if (sc->sc_capa.has_5ghz)
			*he0 |=
			    IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G;
	}
	MWX_DEV_LOG(sc, "phy knobs 0x%x: ht=%d vht=%d he=%d htcaps=0x%04x modecaps=0x%x\n",
	    sc->sc_knobs, (ic->ic_flags & IEEE80211_F_HTON) != 0,
	    (ic->ic_flags & IEEE80211_F_VHTON) != 0,
	    (ic->ic_flags & IEEE80211_F_HEON) != 0, ic->ic_htcaps,
	    ic->ic_modecaps);
}

/*
 * Tell the firmware about an RX Block Ack agreement, as in
 * mt76_connac_mcu_sta_ba(): a WTBL_BA update followed by STA_REC_BA.
 * net80211 does the reordering.
 */
int
mt7921_mcu_sta_rx_ba(struct mwx_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, int enable)
{
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct ieee80211_rx_ba *ba = &ni->ni_rx_ba[tid];
	struct sta_rec_wtbl *wtbl;
	struct wtbl_ba *wba;
	struct sta_rec_ba *sba;
	struct mbuf *m;
	uint16_t tlvnum = 0, wnum = 0;
	uint16_t winsize = enable ? ba->ba_winsize : 0;
	int rv;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;
	wtbl = (struct sta_rec_wtbl *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_WTBL, sizeof(*wtbl));
	wtbl->wlan_idx_lo = mn->wcid & 0xff;
	wtbl->wlan_idx_hi = mn->wcid >> 8;
	wtbl->operation = WTBL_SET;
	wba = (struct wtbl_ba *)mwx_append_tlv(m, &wnum, WTBL_BA,
	    sizeof(*wba));
	wba->tid = tid;
	wba->ba_type = MT_BA_TYPE_RECIPIENT;
	wba->ba_winsize = htole16(winsize);
	memcpy(wba->peer_addr, ni->ni_macaddr, IEEE80211_ADDR_LEN);
	wba->rst_ba_tid = tid;
	wba->rst_ba_sel = RST_BA_MAC_TID_MATCH;
	wba->rst_ba_sb = 1;
	wtbl->tlv_num = htole16(wnum);
	wtbl->len = htole16(le16toh(wtbl->len) + sizeof(*wba));
	m = mwx_fill_sta_req_hdr(m, &sc->sc_vif, sc->sc_vif.omac_idx,
	    mn->wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;
	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
	if (rv != 0)
		goto out;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;
	tlvnum = 0;
	sba = (struct sta_rec_ba *)mwx_append_tlv(m, &tlvnum, STA_REC_BA,
	    sizeof(*sba));
	sba->tid = tid;
	sba->ba_type = MT_BA_TYPE_RECIPIENT;
	sba->amsdu = 0;
	sba->ba_en = (enable ? 1 : 0) << tid;
	sba->ssn = htole16(enable ? ba->ba_winstart : 0);
	sba->winsize = htole16(winsize);
	m = mwx_fill_sta_req_hdr(m, &sc->sc_vif, sc->sc_vif.omac_idx,
	    mn->wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;
	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
out:
	MWX_DEV_LOG(sc, "rx ba %s tid=%u ssn=%u winsize=%u rv=%d\n",
	    enable ? "start" : "stop", tid, enable ? ba->ba_winstart : 0,
	    winsize, rv);
	return rv;
}

/*
 * Tell the firmware about a TX (originator) Block Ack agreement so that it
 * aggregates QoS data of this TID, as mt7921_mcu_uni_tx_ba() does through
 * mt76_connac_mcu_sta_ba(): STA_REC_BA followed by a WTBL_BA update.
 */
int
mt7921_mcu_sta_tx_ba(struct mwx_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, int enable)
{
	static const uint8_t ba_range[] = { 4, 8, 12, 24, 36, 48, 54, 64 };
	struct mwx_node *mn = (struct mwx_node *)ni;
	struct ieee80211_tx_ba *ba = &ni->ni_tx_ba[tid];
	struct sta_rec_wtbl *wtbl;
	struct wtbl_ba *wba;
	struct sta_rec_ba *sba;
	struct mbuf *m;
	uint16_t tlvnum = 0, wnum = 0;
	uint16_t winsize = enable ? ba->ba_winsize : 0;
	uint16_t ssn = enable ? ba->ba_winstart : 0;
	int i, rv;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;
	sba = (struct sta_rec_ba *)mwx_append_tlv(m, &tlvnum, STA_REC_BA,
	    sizeof(*sba));
	sba->tid = tid;
	sba->ba_type = MT_BA_TYPE_ORIGINATOR;
	sba->amsdu = 0;
	sba->ba_en = (enable ? 1 : 0) << tid;
	sba->ssn = htole16(ssn);
	sba->winsize = htole16(winsize);
	m = mwx_fill_sta_req_hdr(m, &sc->sc_vif, sc->sc_vif.omac_idx,
	    mn->wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;
	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
	if (rv != 0)
		goto out;

	for (i = nitems(ba_range) - 1; i > 0; i--)
		if (winsize >= ba_range[i])
			break;

	m = mwx_alloc_sta_req_tlv(sizeof(struct sta_req_hdr));
	if (m == NULL)
		return ENOBUFS;
	tlvnum = 0;
	wtbl = (struct sta_rec_wtbl *)mwx_append_tlv(m, &tlvnum,
	    STA_REC_WTBL, sizeof(*wtbl));
	wtbl->wlan_idx_lo = mn->wcid & 0xff;
	wtbl->wlan_idx_hi = mn->wcid >> 8;
	wtbl->operation = WTBL_SET;
	wba = (struct wtbl_ba *)mwx_append_tlv(m, &wnum, WTBL_BA,
	    sizeof(*wba));
	wba->tid = tid;
	wba->ba_type = MT_BA_TYPE_ORIGINATOR;
	wba->sn = htole16(ssn);
	wba->ba_en = enable ? 1 : 0;
	wba->ba_winsize_idx = enable ? i : 0;
	wba->ba_winsize = htole16(winsize);
	wtbl->tlv_num = htole16(wnum);
	wtbl->len = htole16(le16toh(wtbl->len) + sizeof(*wba));
	m = mwx_fill_sta_req_hdr(m, &sc->sc_vif, sc->sc_vif.omac_idx,
	    mn->wcid, tlvnum);
	if (m == NULL)
		return ENOBUFS;
	rv = mwx_mcu_send_mbuf_wait(sc, MCU_UNI_CMD_STA_REC_UPDATE, m);
out:
	MWX_DEV_LOG(sc, "tx ba %s tid=%u ssn=%u winsize=%u rv=%d\n",
	    enable ? "start" : "stop", tid, ssn, winsize, rv);
	return rv;
}

static int
mwx_ba_enqueue(struct mwx_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, int start, int tx)
{
	struct mwx_ba_task_arg *a;

	if ((sc->sc_ba_cur + 1) % nitems(sc->sc_ba_arg) == sc->sc_ba_tail)
		return ENOSPC;
	a = &sc->sc_ba_arg[sc->sc_ba_cur];
	a->ni = ni;
	a->tid = tid;
	a->start = start;
	a->tx = tx;
	sc->sc_ba_cur = (sc->sc_ba_cur + 1) % nitems(sc->sc_ba_arg);
	task_add(sc->sc_nswq, &sc->sc_ba_task);
	return 0;
}

/* Called from the RX path: MCU commands wait, so defer to a task. */
int
mwx_ampdu_rx_start(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	int rv;

	if (sc->sc_knobs & MWX_KNOB_NO_RXBA) {
		MWX_DEV_LOG(sc, "rx ba refused tid=%u (knob)\n", tid);
		return ENOTSUP;
	}
	rv = mwx_ba_enqueue(sc, ni, tid, 1, 0);
	return rv ? rv : EBUSY;
}

void
mwx_ampdu_rx_stop(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	mwx_ba_enqueue(sc, ni, tid, 0, 0);
}

/* Called when the ADDBA response arrives (RX path): defer to the task. */
int
mwx_ampdu_tx_start(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;
	int rv;

	if (sc->sc_knobs & MWX_KNOB_NO_TXBA)
		return ENOTSUP;
	rv = mwx_ba_enqueue(sc, ni, tid, 1, 1);
	return rv ? rv : EBUSY;
}

void
mwx_ampdu_tx_stop(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct mwx_softc *sc = (struct mwx_softc *)ic->ic_softc;

	mwx_ba_enqueue(sc, ni, tid, 0, 1);
}

void
mwx_ba_task(void *arg)
{
	struct mwx_softc *sc = (struct mwx_softc *)arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct mwx_ba_task_arg *a;
	int s = splnet();

	while (sc->sc_ba_tail != sc->sc_ba_cur) {
		struct ieee80211_node *ni;
		uint8_t tid;
		int start, tx, rv;

		a = &sc->sc_ba_arg[sc->sc_ba_tail];
		ni = a->ni;
		tid = a->tid;
		start = a->start;
		tx = a->tx;
		a->ni = NULL;
		sc->sc_ba_tail = (sc->sc_ba_tail + 1) % nitems(sc->sc_ba_arg);

		/* The agreement died with the association. */
		if (ic->ic_state != IEEE80211_S_RUN || ni != ic->ic_bss)
			continue;

		if (tx) {
			rv = mt7921_mcu_sta_tx_ba(sc, ni, tid, start);
			if (!start ||
			    ni->ni_tx_ba[tid].ba_state != IEEE80211_BA_REQUESTED)
				continue;
			if (rv == 0)
				ieee80211_addba_resp_accept(ic, ni, tid);
			else
				ieee80211_addba_resp_refuse(ic, ni, tid,
				    IEEE80211_STATUS_UNSPECIFIED);
			continue;
		}

		rv = mt7921_mcu_sta_rx_ba(sc, ni, tid, start);
		if (!start)
			continue;
		if (ni->ni_rx_ba[tid].ba_state != IEEE80211_BA_REQUESTED)
			continue;
		if (rv == 0)
			ieee80211_addba_req_accept(ic, ni, tid);
		else
			ieee80211_addba_req_refuse(ic, ni, tid);
	}

	splx(s);
}

// AWDL firmware boundary. All operations run on the same gate as DMA/MCU.
static uint64_t mwx_awdl_now()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

// Only one operation may own the secondary firmware context. Never hold the
// lock across MCU waits: replies arrive on the workloop that owns the gate.
class MwxAwdlCall {
    mwx_softc *sc;
public:
    MwxAwdlCall(mwx_softc *s) : sc(NULL) {
        if(!s || !s->sc_awdl_lock) return;
        IOLockLock(s->sc_awdl_lock);
        if(!s->sc_detaching && !s->sc_awdl_stopping) { ++s->sc_awdl_calls; sc=s; }
        IOLockUnlock(s->sc_awdl_lock);
    }
    ~MwxAwdlCall() {
        if(!sc) return;
        IOLockLock(sc->sc_awdl_lock);
        --sc->sc_awdl_calls;
        IOLockUnlock(sc->sc_awdl_lock);
    }
    bool valid() const { return sc!=NULL; }
};

class MwxAwdlOperation {
    mwx_softc *sc;
public:
    uint32_t epoch;
    MwxAwdlOperation(mwx_softc *s,bool teardown=false) : sc(NULL), epoch(0) {
        if (!s || !s->sc_awdl_lock) return;
        IOLockLock(s->sc_awdl_lock);
        if (!s->sc_awdl_busy && (!s->sc_awdl_stopping || teardown) && !s->sc_detaching &&
            !s->sc_resetting && (s->sc_ic.ic_if.if_flags & IFF_RUNNING)) {
            s->sc_awdl_busy=true; epoch=s->sc_awdl_epoch; sc=s;
        }
        IOLockUnlock(s->sc_awdl_lock);
    }
    ~MwxAwdlOperation() {
        if (!sc) return;
        IOLockLock(sc->sc_awdl_lock);
        sc->sc_awdl_busy=false;
        IOLockUnlock(sc->sc_awdl_lock);
    }
    bool valid() const { return sc && mwx_awdl_current(sc,epoch); }
    bool acquired() const { return sc != NULL; }
};

static bool mwx_awdl_current(mwx_softc *sc, uint32_t epoch)
{
    if (!sc || !sc->sc_awdl_lock) return false;
    IOLockLock(sc->sc_awdl_lock);
    bool valid=sc->sc_awdl_epoch==epoch && !sc->sc_awdl_stopping &&
        !sc->sc_detaching && !sc->sc_resetting &&
        (sc->sc_ic.ic_if.if_flags & IFF_RUNNING);
    IOLockUnlock(sc->sc_awdl_lock);
    return valid;
}

static const uint32_t mwx_awdl_filter_mask=MT_WF_RFCR_DROP_OTHER_BSS |
    MT_WF_RFCR_DROP_A3_BSSID | MT_WF_RFCR_DROP_A2_BSSID |
    MT_WF_RFCR_DROP_OTHER_UC | MT_WF_RFCR_DROP_MCAST |
    MT_WF_RFCR_DROP_MCAST_FILTERED;
// MT76 mt7921_configure_filter/mt7921_mcu_set_beacon_filter encodings.
static const uint32_t mwx_fif_enable=1U<<31, mwx_fif_other_bss=1U<<6;
static const uint8_t mwx_fif_bit_set=1U<<0, mwx_fif_bit_clr=1U<<1;

void mwx_awdl_restore_filter(mwx_softc *sc)
{
    if(!sc->sc_awdl_filter_active) return;
    mwx_rmw(sc,MT_WF_RFCR(0),sc->sc_awdl_filter,mwx_awdl_filter_mask);
    // Return firmware to MT76's default FIF set, then reapply the drops
    // that were active before this AWDL context.
    int filter=mt7921_mcu_set_rxfilter(sc,mwx_fif_enable,0,0);
    if(!filter && sc->sc_awdl_filter)
        filter=mt7921_mcu_set_rxfilter(sc,0,mwx_fif_bit_set,sc->sc_awdl_filter);
    MWX_DEV_LOG(sc,"AWDL disable rx-filter result=%d\n",filter);
    sc->sc_awdl_filter_active=false;
}

static void mwx_awdl_quiesce(mwx_softc *sc,bool drainCalls)
{
    if (!sc || !sc->sc_awdl_lock) return;
    IOLockLock(sc->sc_awdl_lock);
    ++sc->sc_awdl_epoch;
    sc->sc_awdl_stopping=true;
    sc->sc_awdl_enabled=false;
    bool reserved=sc->sc_awdl_roc.pending || sc->sc_awdl_roc.granted;
    if(reserved) {
        sc->sc_awdl_roc_abort_pending=true;
        sc->sc_awdl_roc_abort_token=sc->sc_awdl_roc.token;
        sc->sc_awdl_token_used[sc->sc_awdl_roc.token]=mwx_awdl_now();
    }
    sc->sc_awdl_roc.cancel();
    bool busy=sc->sc_awdl_busy || (drainCalls && sc->sc_awdl_calls);
    IOLockUnlock(sc->sc_awdl_lock);
    // Timed sleeps also handle completion before the wait was registered.
    // The controller releases its gate here if teardown holds it.
    while (busy) {
        tsleep_nsec(&sc->sc_awdl_busy,0,"awdl-drain",MSEC_TO_NSEC(10));
        IOLockLock(sc->sc_awdl_lock);
        busy=sc->sc_awdl_busy || (drainCalls && sc->sc_awdl_calls);
        IOLockUnlock(sc->sc_awdl_lock);
    }
    if (!sc->sc_resetting && !sc->sc_detaching &&
        (sc->sc_ic.ic_if.if_flags & IFF_RUNNING)) {
        mwx_awdl_abort(sc);
    } else {
        IOLockLock(sc->sc_awdl_lock);
        sc->sc_awdl_roc_abort_pending=false;
        sc->sc_awdl_roc_abort_retry_at=0;
        IOLockUnlock(sc->sc_awdl_lock);
    }
    memset(sc->sc_awdl_peer_used,0,sizeof(sc->sc_awdl_peer_used));
    sc->sc_awdl_channel=0;
}

// The unified RLM TLV configures the secondary BSS for a legacy 20 MHz
// channel before firmware may grant a remain-on-channel interval.
static int mwx_awdl_set_channel(mwx_softc *sc,uint8_t chan,uint32_t epoch)
{
    struct {
        uint8_t bss_idx,pad[3];
        struct {
            uint16_t tag,len;
            uint8_t control_channel,center_chan,center_chan2,bw;
            uint8_t tx_streams,rx_streams,short_st,ht_op_info;
            uint8_t sco,band,pad[2];
        } __packed rlm;
    } __packed req={};
    req.bss_idx=sc->sc_awdl_vif.idx;
    req.rlm.tag=htole16(UNI_BSS_INFO_RLM);
    req.rlm.len=htole16(sizeof(req.rlm));
    req.rlm.control_channel=req.rlm.center_chan=chan;
    req.rlm.bw=CMD_CBW_20MHZ;
    req.rlm.tx_streams=sc->sc_capa.num_streams;
    req.rlm.rx_streams=sc->sc_capa.antenna_mask;
    req.rlm.short_st=1;
    req.rlm.band=chan<=14 ? 0 : 1;
    int rv=mwx_send_assoc_status(sc,MCU_UNI_CMD_BSS_INFO_UPDATE,&req,sizeof(req));
    if (!mwx_awdl_current(sc,epoch)) return ECANCELED;
    if (!rv) {
        IOLockLock(sc->sc_awdl_lock);
        if(sc->sc_awdl_epoch==epoch && !sc->sc_awdl_stopping) sc->sc_awdl_channel=chan;
        else rv=ECANCELED;
        IOLockUnlock(sc->sc_awdl_lock);
    }
    return rv;
}

void MtkMwx::awdlSetReceiver(void *context, AwdlReceive receiver)
{
    if (!com) return;
    com->sc_awdl_context=context;
    com->sc_awdl_receive=receiver;
}

bool MtkMwx::awdlInterfaceReady()
{
    if (!com || !com->sc_awdl_lock) return false;
    IOLockLock(com->sc_awdl_lock);
    bool ready=!com->sc_detaching && !com->sc_resetting &&
        !com->sc_awdl_stopping && !com->sc_ic.ic_wcl_mac_reconfig &&
        (com->sc_ic.ic_if.if_flags & IFF_RUNNING);
    IOLockUnlock(com->sc_awdl_lock);
    return ready;
}

uint8_t MtkMwx::awdlInfraChannel()
{
    if (!com || com->sc_ic.ic_opmode != IEEE80211_M_STA ||
        com->sc_ic.ic_state != IEEE80211_S_RUN || !com->sc_ic.ic_bss ||
        !com->sc_ic.ic_bss->ni_chan)
        return 0;
    unsigned channel = ieee80211_chan2ieee(&com->sc_ic, com->sc_ic.ic_bss->ni_chan);
    return channel <= 165 ? uint8_t(channel) : 0;
}

// MtkWireless: authenticating, associating, or associated with the RSN
// 4-way handshake still running.
static bool mwx_awdl_infra_joining(mwx_softc *sc)
{
    ieee80211com *ic = &sc->sc_ic;
    if (ic->ic_opmode != IEEE80211_M_STA)
        return false;
    if (ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC)
        return true;
    return ic->ic_state == IEEE80211_S_RUN && ic->ic_bss &&
        (ic->ic_flags & IEEE80211_F_RSNON) && !ic->ic_bss->ni_port_valid;
}

// MtkWireless: longest off-channel ROC while associated. The grant runs from
// its arrival, which for an off-channel ROC comes 80-160 ms after the request
// (planned with the measured lead, ~160 ms ahead of the window); a 40 ms cap
// expired before every channel-6 window began (0.1.3: 94 grants, 0 AWs). The
// scheduler aborts the ROC after the window, so this only bounds a stall.
static const uint32_t mwxAwdlOffChannelMaxMs = 300;

// MtkWireless: boot-arg amtk_awdl_offchan=0 keeps AWDL on the AP channel
// while associated (0.1.1 behaviour). Default: leave it with a PM handshake.
static bool mwx_awdl_offchan_enabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        int value = 1;
        if (!PE_parse_boot_argn("amtk_awdl_offchan", &value, sizeof(value)))
            value = 1;
        enabled = value != 0;
    }
    return enabled;
}

uint8_t MtkMwx::awdlOnlyChannel()
{
    return mwx_awdl_offchan_enabled() ? 0 : awdlInfraChannel();
}

bool MtkMwx::awdlChannelReachable(uint8_t channel)
{
    uint8_t only = awdlOnlyChannel();
    return awdlChannelAllowed(channel) && (!only || channel == only);
}

// MtkWireless: a Null data frame to the AP with PM set (sleep) or clear
// (awake). The AP buffers for a sleeping station instead of dropping it,
// which is what an unannounced channel switch got us. No payload, never
// protected; mwx_tx consumes the mbuf.
static int mwx_awdl_infra_pm(mwx_softc *sc, bool sleep)
{
    ieee80211com *ic = &sc->sc_ic;
    ieee80211_node *ni = ic->ic_bss;
    if (ic->ic_opmode != IEEE80211_M_STA || ic->ic_state != IEEE80211_S_RUN || !ni)
        return ENXIO;
    if (mwx_tx_full(sc))
        return ENOBUFS;
    mbuf_t m = m_gethdr(M_DONTWAIT, MT_DATA);
    if (!m)
        return ENOBUFS;
    ieee80211_frame *wh = mtod(m, ieee80211_frame *);
    memset(wh, 0, sizeof(*wh));
    wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_DATA |
        IEEE80211_FC0_SUBTYPE_NODATA;
    wh->i_fc[1] = IEEE80211_FC1_DIR_TODS | (sleep ? IEEE80211_FC1_PWR_MGT : 0);
    IEEE80211_ADDR_COPY(wh->i_addr1, ni->ni_bssid);
    IEEE80211_ADDR_COPY(wh->i_addr2, ic->ic_myaddr);
    IEEE80211_ADDR_COPY(wh->i_addr3, ni->ni_bssid);
    mbuf_setlen(m, sizeof(*wh));
    mbuf_pkthdr_setlen(m, sizeof(*wh));
    return mwx_tx(sc, m, ni);
}

void MtkMwx::awdlInfraSleep(uint8_t channel)
{
    MwxAwdlCall call(com);
    if (!call.valid() || com->sc_resetting || com->sc_awdl_infra_ps)
        return;
    uint8_t infra = awdlInfraChannel();
    if (!infra || channel == infra)
        return;
    if (mwx_awdl_infra_pm(com, true)) { ++com->sc_awdl_infra_ps_failed; return; }
    com->sc_awdl_infra_ps = true;
    com->sc_awdl_infra_ps_at = mwx_awdl_now();
    ++com->sc_awdl_infra_ps_sleeps;
}

void MtkMwx::awdlInfraWakeIfIdle()
{
    MwxAwdlCall call(com);
    if (!call.valid() || com->sc_resetting || !com->sc_awdl_infra_ps)
        return;
    uint8_t infra = awdlInfraChannel();
    if (!infra) { com->sc_awdl_infra_ps = false; return; }
    uint64_t now = mwx_awdl_now();
    IOLockLock(com->sc_awdl_lock);
    const mtkawdl::Reservation &roc = com->sc_awdl_roc;
    // Firmware returns to the BSS channel when the grant ends; give it 1ms.
    // awdlReserve submits the ROC only 2ms after PM=1: do not wake before.
    bool away = now - com->sc_awdl_infra_ps_at < 10000 || (roc.channel != infra &&
        ((roc.pending && now < roc.deadline) || (roc.granted && now < roc.deadline + 1000)));
    IOLockUnlock(com->sc_awdl_lock);
    if (away)
        return;
    if (mwx_awdl_infra_pm(com, false)) { ++com->sc_awdl_infra_ps_failed; return; }
    com->sc_awdl_infra_ps = false;
    ++com->sc_awdl_infra_ps_wakes;
}

bool MtkMwx::awdlChannelAllowed(uint8_t channel)
{
    if (!com || !channel || channel>165) return false;
    const ieee80211_channel *c=&com->sc_ic.ic_channels[channel];
    return isset(com->sc_ic.ic_chan_active,channel) &&
        isset(com->sc_ic.ic_chan_avail,channel) && c->ic_freq && c->ic_flags &&
        (IEEE80211_IS_CHAN_2GHZ(c) || IEEE80211_IS_CHAN_5GHZ(c)) &&
        !(c->ic_flags & (IEEE80211_CHAN_PASSIVE | IEEE80211_CHAN_DFS));
}

static int mwx_awdl_abort(mwx_softc *sc)
{
    IOLockLock(sc->sc_awdl_lock);
    bool reserved=sc->sc_awdl_roc.pending || sc->sc_awdl_roc.granted;
    if(sc->sc_awdl_roc.pending && mwx_awdl_now()>=sc->sc_awdl_roc.deadline)
        ++sc->sc_awdl_roc_timeouts;
    if(reserved) {
        sc->sc_awdl_roc_abort_pending=true;
        sc->sc_awdl_roc_abort_token=sc->sc_awdl_roc.token;
    }
    bool abortPending=sc->sc_awdl_roc_abort_pending;
    uint8_t token=sc->sc_awdl_roc_abort_token;
    uint32_t epoch=sc->sc_awdl_epoch;
    sc->sc_awdl_roc.cancel();
    IOLockUnlock(sc->sc_awdl_lock);
    if(!abortPending) return 0;
    uint8_t request[16]={};
    mtkawdl::put16(request+4,1); mtkawdl::put16(request+6,12);
    request[8]=sc->sc_awdl_vif.idx; request[9]=token;
    request[10]=0xff;
    int rv=mwx_mcu_send_msg(sc,0x00020027,request,sizeof(request),NULL);
    uint64_t now=mwx_awdl_now();
    IOLockLock(sc->sc_awdl_lock);
    if(rv) ++sc->sc_awdl_roc_abort_failed;
    else ++sc->sc_awdl_roc_abort_submitted;
    sc->sc_awdl_roc_abort_status=rv;
    if(sc->sc_awdl_epoch==epoch && sc->sc_awdl_roc_abort_token==token) {
        // A failed enqueue does not release firmware ownership. Keep the
        // cancelled token for retry before allowing any replacement acquire.
        sc->sc_awdl_roc_abort_pending=rv!=0;
        sc->sc_awdl_roc_abort_retry_at=rv ? now+10000 : 0;
        sc->sc_awdl_token_used[token]=now ? now : 1;
    }
    uint64_t failures=sc->sc_awdl_roc_abort_failed;
    IOLockUnlock(sc->sc_awdl_lock);
    if(rv && (failures<=8 || !(failures&(failures-1))))
        MWX_DEV_LOG(sc,"AWDL ROC abort token=%u result=%d failures=%llu\n",token,rv,failures);
    return rv;
}

void MtkMwx::awdlAbort()
{
    MwxAwdlCall call(com);
    if(!call.valid()) return;
    MwxAwdlOperation operation(com);
    if(operation.valid()) mwx_awdl_abort(com);
}

static int mwx_awdl_remove_station(mwx_softc *sc,uint32_t epoch,unsigned index)
{
    if(!sc->sc_awdl_station_pending[index]) return 0;
    uint16_t wcid=MWX_WTBL_STA+index;
    mbuf_t m=mwx_alloc_sta_req_tlv(sizeof(sta_req_hdr));
    if(!m) return ENOBUFS;
    uint16_t count=0;
    auto basic=(sta_rec_basic *)mwx_append_tlv(m,&count,STA_REC_BASIC,sizeof(sta_rec_basic));
    basic->conn_type=htole32(index==3 ? STA_TYPE_BC | NETWORK_INFRA : STA_TYPE_STA | NETWORK_INFRA);
    basic->conn_state=CONN_STATE_DISCONNECT;
    basic->extra_info=htole16(EXTRA_INFO_VER);
    if(index==3) memset(basic->peer_addr,0xff,6);
    else memcpy(basic->peer_addr,sc->sc_awdl_peers[index].ni.ni_macaddr,6);
    auto wtbl=(sta_rec_wtbl *)mwx_append_tlv(m,&count,STA_REC_WTBL,sizeof(sta_rec_wtbl));
    wtbl->wlan_idx_lo=wcid; wtbl->operation=WTBL_RESET_AND_SET;
    m=mwx_fill_sta_req_hdr(m,&sc->sc_awdl_vif,sc->sc_awdl_vif.omac_idx,wcid,count);
    int seq=0; uint32_t status=0;
    int rv=m ? mwx_mcu_send_mbuf(sc,MCU_UNI_CMD_STA_REC_UPDATE,m,&seq) : ENOBUFS;
    if(sc->sc_awdl_epoch!=epoch) return ECANCELED;
    if(!rv) rv=mwx_mcu_wait_resp_int(sc,MCU_UNI_CMD_STA_REC_UPDATE,seq,&status);
    MWX_DEV_LOG(sc,"AWDL remove station WCID=%u transport=%d firmware=%u\n",wcid,rv,status);
    if(sc->sc_awdl_epoch!=epoch) return ECANCELED;
    if(rv || status) return rv ? rv : EIO;
    sc->sc_awdl_station_pending[index]=false;
    if(index<3) sc->sc_awdl_peer_used[index]=false;
    return 0;
}

static int mwx_awdl_remove_context(mwx_softc *sc,uint32_t epoch)
{
    // V9's first broadcast add succeeds, but re-add returns 0x10003.
    // Explicitly disconnect owned WCIDs before removing the BSS/OMAC. Match
    // the primary STA teardown; deleting DEV_INFO alone is not STA teardown.
    for(unsigned i=0;i<4;++i) {
        int rv=mwx_awdl_remove_station(sc,epoch,i);
        if(rv) return rv;
    }
    if(!sc->sc_awdl_configured) return 0;
    int rv=mt7921_mcu_uni_add_dev(sc,&sc->sc_awdl_vif,&sc->sc_awdl_vif.vif_mn,0);
    if(sc->sc_awdl_epoch!=epoch) return ECANCELED;
    if(!rv) {
        sc->sc_awdl_configured=false;
        IOLockLock(sc->sc_awdl_lock);
        // Removing the BSS/OMAC also ends an abort that could not be queued.
        sc->sc_awdl_roc_abort_pending=false;
        sc->sc_awdl_roc_abort_retry_at=0;
        IOLockUnlock(sc->sc_awdl_lock);
    }
    return rv;
}

IOReturn MtkMwx::awdlEnable(const uint8_t *address, bool enable)
{
    mwx_softc *sc=com;
    if (!sc || !sc->sc_awdl_lock) return kIOReturnNotReady;
    MwxAwdlCall call(sc);
    if(!call.valid()) return enable ? kIOReturnNotReady : kIOReturnSuccess;
    if (!enable) {
        // This call participates in the outer stop/detach drain; its own
        // disable only waits for firmware ownership, avoiding self-deadlock.
        mwx_awdl_quiesce(sc,false);
        bool running=!sc->sc_detaching && !sc->sc_resetting &&
            (sc->sc_ic.ic_if.if_flags & IFF_RUNNING);
        MwxAwdlOperation cleanup(sc,true);
        if(running && !cleanup.acquired()) return kIOReturnBusy;
        int rv=0;
        if(running && sc->sc_awdl_configured) {
            // Restore only the filter bits this context changed; preserve
            // unrelated filter updates made while AWDL was enabled.
            mwx_awdl_restore_filter(sc);
            rv=mwx_awdl_remove_context(sc,cleanup.epoch);
        }
        if(cleanup.acquired() && sc->sc_awdl_epoch!=cleanup.epoch)
            return kIOReturnAborted;
        IOLockLock(sc->sc_awdl_lock);
        if(cleanup.acquired() && sc->sc_awdl_epoch!=cleanup.epoch) {
            IOLockUnlock(sc->sc_awdl_lock); return kIOReturnAborted;
        }
        if(running && !sc->sc_detaching && !sc->sc_resetting) sc->sc_awdl_stopping=false;
        IOLockUnlock(sc->sc_awdl_lock);
        if(sc->sc_awdl_scan_suspended) {
            sc->sc_awdl_scan_suspended=false;
            if(running && !sc->sc_detaching && !sc->sc_resetting &&
               (sc->sc_ic.ic_state==IEEE80211_S_SCAN || (sc->sc_ic.ic_flags&IEEE80211_F_BGSCAN)))
                task_add(systq,&sc->sc_scan_task);
        }
        return rv ? kIOReturnIOError : kIOReturnSuccess;
    }
    if (!address || !mtkawdl::unicast(address)) return kIOReturnBadArgument;
    // A duplicate native enable does not change firmware state. It must not
    // fail merely because a ROC/peer command currently owns the MCU context.
    IOLockLock(sc->sc_awdl_lock);
    bool alreadyEnabled=sc->sc_awdl_enabled && !sc->sc_awdl_stopping &&
        !sc->sc_detaching && !sc->sc_resetting &&
        (sc->sc_ic.ic_if.if_flags & IFF_RUNNING);
    bool sameAddress=!memcmp(address,sc->sc_awdl_address,6);
    IOLockUnlock(sc->sc_awdl_lock);
    if(alreadyEnabled) return sameAddress ? kIOReturnSuccess : kIOReturnBusy;
    MwxAwdlOperation operation(sc);
    if (!operation.valid()) {
        MWX_DEV_LOG(sc,"AWDL enable blocked busy=%d stopping=%d reset=%d running=%d\n",
                    sc->sc_awdl_busy,sc->sc_awdl_stopping,sc->sc_resetting,
                    !!(sc->sc_ic.ic_if.if_flags & IFF_RUNNING));
        return kIOReturnNotReady;
    }
    if(sc->sc_awdl_enabled) return !memcmp(address,sc->sc_awdl_address,6) ?
        kIOReturnSuccess : kIOReturnBusy;
    // A failed partial enable must be removed before retrying the same OMAC.
    if(sc->sc_awdl_configured) {
        int rv=mwx_awdl_remove_context(sc,operation.epoch);
        if(!operation.valid()) return kIOReturnAborted;
        if(rv) return kIOReturnIOError;
    }
    mwx_vif &v=sc->sc_awdl_vif;
    memset(&v,0,sizeof(v)); v.idx=1; v.omac_idx=1; v.wmm_idx=1;
    memset(sc->sc_awdl_peer_used,0,sizeof(sc->sc_awdl_peer_used));
    v.vif_mn.wcid=MWX_WTBL_RESERVED-1;
    memcpy(sc->sc_awdl_address,address,6);
    mwx_mac_wtbl_update(sc,v.vif_mn.wcid);
    // Remember a partial context even if teardown interrupts a later wait.
    sc->sc_awdl_configured=true;
    mwxController()->setProperty("MTKAWDLFirmwareStage","device-bss");
    int rv=mt7921_mcu_uni_add_dev(sc,&v,&v.vif_mn,1);
    mwxController()->setProperty("MTKAWDLFirmwareTransportStatus",static_cast<uint64_t>(rv),32);
    MWX_DEV_LOG(sc,"AWDL enable device-bss result=%d\n",rv);
    if(!operation.valid()) return kIOReturnAborted;
    if(rv) return kIOReturnIOError;
    // A separate multicast/legacy TX entry; no infrastructure encryption or
    // header translation. Firmware owns ACK/retries on the raw 802.11 frame.
    mbuf_t m=mwx_alloc_sta_req_tlv(sizeof(sta_req_hdr));
    if(!m) { mwx_awdl_remove_context(sc,operation.epoch); return kIOReturnNoMemory; }
    uint16_t count=0;
    sta_rec_basic *basic=(sta_rec_basic *)mwx_append_tlv(m,&count,STA_REC_BASIC,sizeof(*basic));
    // MT76's broadcast station record always uses CONNECTION_INFRA_BC,
    // including an IBSS BSS; BC|IBSS is not a firmware connection type.
    basic->conn_type=htole32(STA_TYPE_BC | NETWORK_INFRA);
    basic->conn_state=CONN_STATE_PORT_SECURE;
    basic->extra_info=htole16(EXTRA_INFO_VER | EXTRA_INFO_NEW);
    memset(basic->peer_addr,0xff,6);
    sta_rec_wtbl *wtbl=(sta_rec_wtbl *)mwx_append_tlv(m,&count,STA_REC_WTBL,sizeof(*wtbl));
    wtbl->wlan_idx_lo=v.vif_mn.wcid; wtbl->operation=WTBL_RESET_AND_SET;
    uint16_t wcount=0;
    wtbl_generic *generic=(wtbl_generic *)mwx_append_tlv(m,&wcount,WTBL_GENERIC,sizeof(*generic));
    memset(generic->peer_addr,0xff,6); generic->muar_idx=0xe;
    wtbl_rx *rx=(wtbl_rx *)mwx_append_tlv(m,&wcount,WTBL_RX,sizeof(*rx));
    rx->rca1=rx->rca2=rx->rv=1;
    wtbl->tlv_num=htole16(wcount);
    wtbl->len=htole16(sizeof(*wtbl)+sizeof(*generic)+sizeof(*rx));
    m=mwx_fill_sta_req_hdr(m,&v,v.omac_idx,v.vif_mn.wcid,count);
    int seq=0; uint32_t status=0;
    mwxController()->setProperty("MTKAWDLFirmwareStage","broadcast-station");
    sc->sc_awdl_station_pending[3]=true;
    rv=m ? mwx_mcu_send_mbuf(sc,MCU_UNI_CMD_STA_REC_UPDATE,m,&seq) : ENOBUFS;
    if(!operation.valid()) return kIOReturnAborted;
    if(!rv) rv=mwx_mcu_wait_resp_int(sc,MCU_UNI_CMD_STA_REC_UPDATE,seq,&status);
    mwxController()->setProperty("MTKAWDLFirmwareTransportStatus",static_cast<uint64_t>(rv),32);
    mwxController()->setProperty("MTKAWDLFirmwareResponseStatus",static_cast<uint64_t>(status),32);
    MWX_DEV_LOG(sc,"AWDL enable broadcast-station transport=%d firmware=%u\n",rv,status);
    if(!operation.valid()) return kIOReturnAborted;
    if(!rv && !status) {
        mwxController()->setProperty("MTKAWDLFirmwareStage","tx-queues");
        rv=mt7921_mcu_set_tx(sc,&v);
        mwxController()->setProperty("MTKAWDLFirmwareTransportStatus",static_cast<uint64_t>(rv),32);
        MWX_DEV_LOG(sc,"AWDL enable tx-queues result=%d\n",rv);
    }
    if(!operation.valid()) return kIOReturnAborted;
    if(rv || status) {
        mwx_awdl_remove_context(sc,operation.epoch);
        return kIOReturnIOError;
    }
    // AWDL group data carries the AWDL BSSID, which no firmware BSS owns.
    // MT7921 firmware owns RFCR, so a direct register clear alone does not
    // keep multicast (mDNS) from other-BSS peers: request it through the MCU.
    // Unicast and action RX do not depend on this; record, do not fail.
    sc->sc_awdl_filter=mwx_read(sc,MT_WF_RFCR(0)) & mwx_awdl_filter_mask;
    int filter=mt7921_mcu_set_rxfilter(sc,mwx_fif_enable|mwx_fif_other_bss,0,0);
    if(!filter) filter=mt7921_mcu_set_rxfilter(sc,0,mwx_fif_bit_clr,mwx_awdl_filter_mask);
    // An abort below still leaves teardown responsible for the firmware filter.
    sc->sc_awdl_filter_active=true;
    mwxController()->setProperty("MTKAWDLRxFilterStatus",static_cast<uint64_t>(filter),32);
    MWX_DEV_LOG(sc,"AWDL enable rx-filter result=%d\n",filter);
    if(!operation.valid()) return kIOReturnAborted;
    IOLockLock(sc->sc_awdl_lock);
    if(sc->sc_awdl_epoch!=operation.epoch || sc->sc_awdl_stopping) {
        IOLockUnlock(sc->sc_awdl_lock); return kIOReturnAborted;
    }
    mwx_clear(sc,MT_WF_RFCR(0),mwx_awdl_filter_mask);
    sc->sc_awdl_enabled=true;
    IOLockUnlock(sc->sc_awdl_lock);
    mwxController()->setProperty("MTKAWDLFirmwareStage","enabled");
    return kIOReturnSuccess;
}

IOReturn MtkMwx::awdlPrepareChannel(uint8_t channel)
{
    MwxAwdlCall call(com);
    if(!call.valid() || !com->sc_awdl_enabled || com->sc_resetting || com->sc_detaching)
        return kIOReturnNotReady;
    if(!awdlChannelAllowed(channel)) return kIOReturnBadArgument;
    MwxAwdlOperation operation(com);
    if(!operation.valid()) return kIOReturnNotReady;
    if(com->sc_awdl_channel==channel) return kIOReturnSuccess;
    IOLockLock(com->sc_awdl_lock);
    bool inUse=com->sc_awdl_roc_abort_pending || com->sc_awdl_roc.pending ||
        (com->sc_awdl_roc.granted && mwx_awdl_now()<com->sc_awdl_roc.deadline);
    IOLockUnlock(com->sc_awdl_lock);
    if(inUse) return kIOReturnBusy;
    int error=mwx_awdl_set_channel(com,channel,operation.epoch);
    if(!operation.valid()) return kIOReturnAborted;
    return error ? kIOReturnIOError : kIOReturnSuccess;
}

IOReturn MtkMwx::awdlReserve(uint8_t channel,uint32_t durationMs)
{
    MwxAwdlCall call(com);
    if(!call.valid()) return kIOReturnNotReady;
    if(!com || !com->sc_awdl_enabled || com->sc_resetting || com->sc_detaching)
        return kIOReturnNotReady;
    if(!awdlChannelAllowed(channel) || !durationMs || durationMs>mtkawdl::maxRocMs) return kIOReturnBadArgument;
    MwxAwdlOperation operation(com);
    if(!operation.valid()) { ++com->sc_awdl_roc_busy_blocked; return kIOReturnNotReady; }
    // MtkWireless: joining an AP owns the radio. AUTH/ASSOC frames and the
    // 4-way handshake are lost if AWDL takes it elsewhere meanwhile.
    if(mwx_awdl_infra_joining(com)) { ++com->sc_awdl_roc_join_blocked; return kIOReturnNotReady; }
    // MtkWireless: a real scan owns the radio until SCAN_DONE/watchdog.
    // Associated unsolicited scans are suspended before submission; an
    // explicit WCL scan must complete too, or the network list stays empty.
    if(com->sc_flags & (MWX_FLAG_SCANNING | MWX_FLAG_BGSCAN)) {
        ++com->sc_awdl_roc_scan_blocked; return kIOReturnNotReady;
    }
    IOLockLock(com->sc_awdl_lock);
    bool retryLater=com->sc_awdl_roc_abort_pending && mwx_awdl_now()<com->sc_awdl_roc_abort_retry_at;
    IOLockUnlock(com->sc_awdl_lock);
    if(retryLater) return kIOReturnNotReady;
    // MtkWireless: away from the AP's channel only after it was told we
    // sleep and that null frame had ~2ms to go out; keep the visit short.
    uint8_t infra=awdlInfraChannel();
    if(infra && channel!=infra) {
        if(!mwx_awdl_offchan_enabled()) return kIOReturnBadArgument;
        if(!com->sc_awdl_infra_ps) awdlInfraSleep(channel);
        if(!com->sc_awdl_infra_ps || mwx_awdl_now()-com->sc_awdl_infra_ps_at<2000)
            return kIOReturnNotReady;
        if(durationMs>mwxAwdlOffChannelMaxMs) durationMs=mwxAwdlOffChannelMaxMs;
    }
    int abortRv=mwx_awdl_abort(com);
    if(!operation.valid()) return kIOReturnAborted;
    if(abortRv) return kIOReturnIOError;
    // A P2P discovery device has no persistent RLM channel context. Match
    // mt7921_mcu_set_roc: acquire carries all channel/band/width fields.
    // Programming RLM here couples discovery to an active BSS and blocks
    // the initial AW timer on a synchronous firmware wait. Only the matched
    // unsolicited grant below establishes the temporary radio context.
    uint8_t token=com->sc_awdl_roc.token+1;
    if(!token) token=1;
    // Firmware returns only an 8-bit token. Do not reuse one inside the
    // response window, including after disable/enable or a reset.
    uint64_t now=mwx_awdl_now();
    for(unsigned i=0;i<255 && com->sc_awdl_token_used[token] &&
        now-com->sc_awdl_token_used[token]<mtkawdl::rocTokenQuarantineUs;++i) {
        if(++token==0) token=1;
    }
    if(com->sc_awdl_token_used[token] && now-com->sc_awdl_token_used[token]<mtkawdl::rocTokenQuarantineUs)
        return kIOReturnBusy;
    uint8_t request[28]={};
    mtkawdl::put16(request+6,24);
    request[8]=com->sc_awdl_vif.idx; request[9]=token;
    request[10]=channel; request[12]=channel<=14 ? 1 : 2;
    request[14]=channel; request[17]=channel; request[19]=1;
    com->sc_awdl_vif.vif_mn.ni.ni_chan=&com->sc_ic.ic_channels[channel];
    mtkawdl::put32(request+20,durationMs); request[24]=0xff;
    IOLockLock(com->sc_awdl_lock);
    if(com->sc_awdl_epoch!=operation.epoch || com->sc_awdl_stopping) {
        IOLockUnlock(com->sc_awdl_lock); return kIOReturnAborted;
    }
    com->sc_awdl_token_used[token]=now ? now : 1;
    com->sc_awdl_roc.request(token,channel,now);
    IOLockUnlock(com->sc_awdl_lock);
    int rv=mwx_mcu_send_msg(com,0x00020027,request,sizeof(request),NULL);
    uint64_t queuedAt=mwx_awdl_now();
    if(!rv) {
        IOLockLock(com->sc_awdl_lock);
        com->sc_awdl_roc_queue_delay_us=queuedAt-now;
        if(com->sc_awdl_roc_queue_delay_us>com->sc_awdl_roc_max_queue_delay_us)
            com->sc_awdl_roc_max_queue_delay_us=com->sc_awdl_roc_queue_delay_us;
        if(com->sc_awdl_epoch==operation.epoch && com->sc_awdl_roc.token==token) {
            // Ring admission may release the gate while waiting for space.
            // Start the response budget after a successful enqueue, preserving
            // any grant already delivered before the send helper returned.
            if(com->sc_awdl_roc.pending) {
                com->sc_awdl_roc.requestedAt=queuedAt;
                com->sc_awdl_roc.deadline=queuedAt+mtkawdl::rocResponseTimeoutUs;
            }
            com->sc_awdl_token_used[token]=queuedAt ? queuedAt : 1;
        }
        IOLockUnlock(com->sc_awdl_lock);
    }
    if(rv) ++com->sc_awdl_roc_send_failed;
    else ++com->sc_awdl_roc_submitted;
    if(rv || com->sc_awdl_roc_submitted<=4)
        MWX_DEV_LOG(com,"AWDL ROC submit token=%u channel=%u duration=%u result=%d\n",
                    token,channel,durationMs,rv);
    bool current=operation.valid();
    if(rv || !current) {
        IOLockLock(com->sc_awdl_lock);
        if(com->sc_awdl_roc.token==token) com->sc_awdl_roc.cancel();
        IOLockUnlock(com->sc_awdl_lock);
    }
    if(!current) return kIOReturnAborted;
    return rv ? kIOReturnIOError : kIOReturnSuccess;
}

bool MtkMwx::awdlCanTransmit(uint8_t channel)
{
    if(!com || !com->sc_awdl_lock) return false;
    IOLockLock(com->sc_awdl_lock);
    bool ready=com->sc_awdl_enabled && !com->sc_resetting && !com->sc_detaching &&
        !com->sc_awdl_stopping && com->sc_awdl_roc.canTransmit(channel,mwx_awdl_now());
    IOLockUnlock(com->sc_awdl_lock);
    return ready;
}

bool MtkMwx::awdlGrantSnapshot(mtkawdl::Reservation &grant)
{
    if(!com || !com->sc_awdl_lock) return false;
    IOLockLock(com->sc_awdl_lock);
    bool ready=com->sc_awdl_enabled && !com->sc_resetting && !com->sc_detaching &&
        !com->sc_awdl_stopping;
    if(ready) grant=com->sc_awdl_roc;
    IOLockUnlock(com->sc_awdl_lock);
    return ready;
}

bool MtkMwx::awdlIsEnabled()
{
    if(!com || !com->sc_awdl_lock) return false;
    IOLockLock(com->sc_awdl_lock);
    bool enabled=com->sc_awdl_enabled && !com->sc_awdl_stopping &&
        !com->sc_resetting && !com->sc_detaching;
    IOLockUnlock(com->sc_awdl_lock);
    return enabled;
}
void MtkMwx::awdlPublishDiagnostics()
{
    if(!com || !com->sc_awdl_lock) return;
    IOLockLock(com->sc_awdl_lock);
    uint64_t events=com->sc_awdl_roc_events, grants=com->sc_awdl_roc_grants,
             rejected=com->sc_awdl_roc_rejected;
    bool pending=com->sc_awdl_roc.pending;
    mtkawdl::Reservation radio=com->sc_awdl_roc;
    uint64_t delay=com->sc_awdl_roc_grant_delay_us, maxDelay=com->sc_awdl_roc_max_grant_delay_us;
    uint64_t commandResults=com->sc_awdl_roc_command_results, commandFailed=com->sc_awdl_roc_command_failed,
             commandMalformed=com->sc_awdl_roc_command_malformed, commandAt=com->sc_awdl_roc_command_result_us;
    uint32_t commandStatus=com->sc_awdl_roc_command_status;
    uint64_t reasons[static_cast<unsigned>(mtkawdl::RocResult::Count)];
    memcpy(reasons,com->sc_awdl_roc_reasons,sizeof(reasons));
    mtkawdl::RocGrant last=com->sc_awdl_roc_last_event;
    unsigned lastResult=static_cast<unsigned>(com->sc_awdl_roc_last_result);
    uint64_t lastAt=com->sc_awdl_roc_last_event_us, response=com->sc_awdl_roc_last_response_us;
    uint64_t timeouts=com->sc_awdl_roc_timeouts;
    bool abortPending=com->sc_awdl_roc_abort_pending;
    uint8_t abortToken=com->sc_awdl_roc_abort_token;
    int abortStatus=com->sc_awdl_roc_abort_status;
    uint64_t abortSubmitted=com->sc_awdl_roc_abort_submitted, abortFailed=com->sc_awdl_roc_abort_failed,
             abortRetryAt=com->sc_awdl_roc_abort_retry_at;
    uint64_t queueDelay=com->sc_awdl_roc_queue_delay_us, maxQueueDelay=com->sc_awdl_roc_max_queue_delay_us;
    IOLockUnlock(com->sc_awdl_lock);
    // Publish outside the receive lock; called at most once per second.
    uint64_t observed=mwx_awdl_now();
    const char *state=abortPending ? "abort-pending" :
        radio.pending ? (observed<radio.deadline ? "waiting-grant" : "response-expired") :
        radio.granted ? (observed<radio.deadline ? "granted" : "grant-expired") : "idle";
    mwxController()->setProperty("MTKAWDLROCState",state);
    mwxController()->setProperty("MTKAWDLInfraPowerSave",com->sc_awdl_infra_ps);
    mwxController()->setProperty("MTKAWDLInfraPSSleeps",com->sc_awdl_infra_ps_sleeps,64);
    mwxController()->setProperty("MTKAWDLInfraPSWakes",com->sc_awdl_infra_ps_wakes,64);
    mwxController()->setProperty("MTKAWDLInfraPSFailed",com->sc_awdl_infra_ps_failed,64);
    mwxController()->setProperty("MTKWiFiRxWrongBSS",com->sc_ic.ic_stats.is_rx_wrongbss,32);
    mwxController()->setProperty("MTKWiFiRxDeauth",com->sc_ic.ic_stats.is_rx_deauth,32);
    mwxController()->setProperty("MTKWiFiRxDisassoc",com->sc_ic.ic_stats.is_rx_disassoc,32);
    // MtkWireless: WPA3-SAE outcomes and whether the BSS uses 802.11w PMF.
    mwxController()->setProperty("MTKWiFiSAEAccepted",com->sc_ic.ic_sae_ok,32);
    mwxController()->setProperty("MTKWiFiSAEFailed",com->sc_ic.ic_sae_fail,32);
    mwxController()->setProperty("MTKWiFiSAELastStatus",com->sc_ic.ic_sae_last_status,16);
    mwxController()->setProperty("MTKWiFiPMF",com->sc_ic.ic_bss != NULL &&
        (com->sc_ic.ic_bss->ni_flags & IEEE80211_NODE_MFP) != 0);
    mwxController()->setProperty("MTKAWDLFirmwareContext","p2p-discovery");
    mwxController()->setProperty("MTKAWDLROCStateObservedAtUS",observed,64);
    mwxController()->setProperty("MTKAWDLROCEvents",events,64);
    mwxController()->setProperty("MTKAWDLROCGrants",grants,64);
    mwxController()->setProperty("MTKAWDLROCRejected",rejected,64);
    mwxController()->setProperty("MTKAWDLROCPending",pending);
    mwxController()->setProperty("MTKAWDLROCGranted",radio.granted && observed<radio.deadline);
    mwxController()->setProperty("MTKAWDLROCChannel",static_cast<uint64_t>(radio.channel),32);
    mwxController()->setProperty("MTKAWDLROCToken",static_cast<uint64_t>(radio.token),32);
    mwxController()->setProperty("MTKAWDLROCObservedAtUS",radio.grantObservedAt,64);
    mwxController()->setProperty("MTKAWDLROCDeadlineUS",radio.deadline,64);
    mwxController()->setProperty("MTKAWDLROCGrantDelayUS",delay,64);
    mwxController()->setProperty("MTKAWDLROCMaxGrantDelayUS",maxDelay,64);
    mwxController()->setProperty("MTKAWDLROCCommandResults",commandResults,64);
    mwxController()->setProperty("MTKAWDLROCCommandFailed",commandFailed,64);
    mwxController()->setProperty("MTKAWDLROCCommandMalformed",commandMalformed,64);
    mwxController()->setProperty("MTKAWDLROCCommandLastStatus",static_cast<uint64_t>(commandStatus),32);
    mwxController()->setProperty("MTKAWDLROCCommandLastResultUS",commandAt,64);
    static const char *const names[]={"Accepted","Malformed","NoGrant","WrongBss",
        "FirmwareStatus","WrongType","Disabled","NotPending","Token","Channel","Late","Duration"};
    static_assert(sizeof(names)/sizeof(names[0])==static_cast<unsigned>(mtkawdl::RocResult::Count),"ROC reason names");
    for(unsigned i=1;i<static_cast<unsigned>(mtkawdl::RocResult::Count);++i) {
        char property[64]; snprintf(property,sizeof(property),"MTKAWDLROCReject%s",names[i]);
        mwxController()->setProperty(property,reasons[i],64);
    }
    mwxController()->setProperty("MTKAWDLROCLastResult",events ? names[lastResult] : "None");
    mwxController()->setProperty("MTKAWDLROCResponseTimeouts",timeouts,64);
    mwxController()->setProperty("MTKAWDLROCLastEventUS",lastAt,64);
    mwxController()->setProperty("MTKAWDLROCLastResponseUS",response,64);
    mwxController()->setProperty("MTKAWDLROCLastBss",static_cast<uint64_t>(last.bss),32);
    mwxController()->setProperty("MTKAWDLROCLastToken",static_cast<uint64_t>(last.token),32);
    mwxController()->setProperty("MTKAWDLROCLastChannel",static_cast<uint64_t>(last.channel),32);
    mwxController()->setProperty("MTKAWDLROCLastFirmwareStatus",static_cast<uint64_t>(last.status),32);
    mwxController()->setProperty("MTKAWDLROCLastRequestType",static_cast<uint64_t>(last.requestType),32);
    mwxController()->setProperty("MTKAWDLROCLastDurationMS",static_cast<uint64_t>(last.durationMs),32);
    mwxController()->setProperty("MTKAWDLROCRequestedAtUS",radio.requestedAt,64);
    mwxController()->setProperty("MTKAWDLROCResponseTimeoutUS",static_cast<uint64_t>(mtkawdl::rocResponseTimeoutUs),32);
    mwxController()->setProperty("MTKAWDLROCAbortPending",abortPending);
    mwxController()->setProperty("MTKAWDLROCAbortToken",static_cast<uint64_t>(abortToken),32);
    mwxController()->setProperty("MTKAWDLROCAbortStatus",static_cast<uint64_t>(abortStatus),32);
    mwxController()->setProperty("MTKAWDLROCAbortSubmitted",abortSubmitted,64);
    mwxController()->setProperty("MTKAWDLROCAbortFailed",abortFailed,64);
    mwxController()->setProperty("MTKAWDLROCAbortRetryAtUS",abortRetryAt,64);
    mwxController()->setProperty("MTKAWDLROCQueueDelayUS",queueDelay,64);
    mwxController()->setProperty("MTKAWDLROCMaxQueueDelayUS",maxQueueDelay,64);
    mwxController()->setProperty("MTKAWDLBSSBaseChannel",static_cast<uint64_t>(com->sc_awdl_channel),32);
    mwxController()->setProperty("MTKAWDLROCSubmitted",com->sc_awdl_roc_submitted,64);
    mwxController()->setProperty("MTKAWDLROCSendFailed",com->sc_awdl_roc_send_failed,64);
    mwxController()->setProperty("MTKAWDLROCScanBlocked",com->sc_awdl_roc_scan_blocked,64);
    mwxController()->setProperty("MTKAWDLScanSuspended",com->sc_awdl_scan_suspended);
    mwxController()->setProperty("MTKAWDLScanPreempted",com->sc_awdl_scan_preempted,64);
    mwxController()->setProperty("MTKAWDLROCJoinBlocked",com->sc_awdl_roc_join_blocked,64);
    {
        static const char *names[3]={"MTKAWDLROCLatencyUnassoc","MTKAWDLROCLatencyAPChannel",
                                     "MTKAWDLROCLatencyOffChannel"};
        for(unsigned k=0;k<3;++k) {
            OSArray *a=OSArray::withCapacity(7);
            if(!a) continue;
            for(unsigned b=0;b<7;++b) {
                OSNumber *n=OSNumber::withNumber(com->sc_awdl_roc_latency[k][b],32);
                if(n) { a->setObject(n); n->release(); }
            }
            mwxController()->setProperty(names[k],a); a->release();
        }
    }
    mwxController()->setProperty("MTKAWDLROCBusyBlocked",com->sc_awdl_roc_busy_blocked,64);
    mwxController()->setProperty("MTKAWDLTXSubmitted",com->sc_awdl_tx_submitted,64);
    mwxController()->setProperty("MTKAWDLTXBadFrame",com->sc_awdl_tx_bad_frame,64);
    mwxController()->setProperty("MTKAWDLTXNoGrant",com->sc_awdl_tx_no_grant,64);
    mwxController()->setProperty("MTKAWDLTXBusy",com->sc_awdl_tx_busy,64);
    mwxController()->setProperty("MTKAWDLTXNoPeer",com->sc_awdl_tx_no_peer,64);
    mwxController()->setProperty("MTKAWDLTXBackpressure",com->sc_awdl_tx_backpressure,64);
    mwxController()->setProperty("MTKAWDLTXCopyFailed",com->sc_awdl_tx_copy_failed,64);
    mwxController()->setProperty("MTKAWDLTXDMAFailed",com->sc_awdl_tx_dma_failed,64);
    mwxController()->setProperty("MTKAWDLTXLastErrno",static_cast<uint64_t>(com->sc_awdl_tx_errno),32);
    const auto &tx=com->sc_awdl_tx_status;
    mwxController()->setProperty("MTKAWDLTXDataAcked",tx.dataAcked,64);
    mwxController()->setProperty("MTKAWDLTXDataFailed",tx.dataFailed,64);
    mwxController()->setProperty("MTKAWDLTXMulticastCompleted",tx.multicastCompleted,64);
    mwxController()->setProperty("MTKAWDLTXMulticastFailed",tx.multicastFailed,64);
    mwxController()->setProperty("MTKAWDLTXActionCompleted",tx.actionCompleted,64);
    mwxController()->setProperty("MTKAWDLTXActionFailed",tx.actionFailed,64);
    mwxController()->setProperty("MTKAWDLTXLastStatus",static_cast<uint64_t>(tx.lastStatus),32);
    mwxController()->setProperty("MTKAWDLTXLastSequence",static_cast<uint64_t>(tx.lastSequence),32);
    mwxController()->setProperty("MTKAWDLTXLastWcid",static_cast<uint64_t>(tx.lastWcid),32);
    mwxController()->setProperty("MTKAWDLTXLastPid",static_cast<uint64_t>(tx.lastPid),32);
    // Firmware owns RFCR; a nonzero AWDL-mask value means it reapplied drops.
    if(com->sc_awdl_enabled && !com->sc_resetting && !com->sc_detaching &&
       (com->sc_ic.ic_if.if_flags & IFF_RUNNING))
        mwxController()->setProperty("MTKAWDLRxFilterRegister",
            static_cast<uint64_t>(mwx_read(com,MT_WF_RFCR(0))),32);
}

IOReturn MtkMwx::awdlTransmit(mbuf_t frame,uint8_t channel)
{
    IOReturn status=awdlTryTransmit(&frame,channel);
    // Management API retains its always-consumed contract. Data uses the
    // queue API below so resource pressure does not discard the original.
    if(frame) mbuf_freem(frame);
    return status;
}

IOReturn MtkMwx::awdlTryTransmit(mbuf_t *packet,uint8_t channel)
{
    if(!packet || !*packet) return kIOReturnBadArgument;
    mbuf_t frame=*packet;
    MwxAwdlCall call(com);
    if(!call.valid()) return kIOReturnNotReady;
    // Frame shape/identity is a permanent rejection, independently of ROC.
    if(mbuf_len(frame)<24 || mbuf_len(frame)!=mbuf_pkthdr_len(frame) ||
       memcmp((uint8_t *)mbuf_data(frame)+16,mtkawdl::bssid,6) ||
       memcmp((uint8_t *)mbuf_data(frame)+10,com->sc_awdl_address,6)) {
        ++com->sc_awdl_tx_bad_frame;
        mbuf_freem(frame); *packet=NULL; return kIOReturnBadArgument;
    }
    MwxAwdlOperation operation(com);
    if(!operation.valid()) { ++com->sc_awdl_tx_busy; return kIOReturnNotReady; }
    if(!awdlCanTransmit(channel)) {
        ++com->sc_awdl_tx_no_grant; return kIOReturnNotReady;
    }
    ieee80211_node *node=&com->sc_awdl_vif.vif_mn.ni;
    const uint8_t *destination=(uint8_t *)mbuf_data(frame)+4;
    if(!(destination[0]&1)) {
        node=NULL;
        for(unsigned i=0;i<3;++i) if(com->sc_awdl_peer_used[i] &&
            !memcmp(com->sc_awdl_peers[i].ni.ni_macaddr,destination,6)) {
            node=&com->sc_awdl_peers[i].ni; break;
        }
        if(!node) { ++com->sc_awdl_tx_no_peer; return kIOReturnNotFound; }
    }
    // Infrastructure and AWDL share TXWIs/the DMA ring. Check capacity before
    // calling the consuming primitive, just as infrastructure mwx_start does.
    if(mwx_tx_full(com)) { ++com->sc_awdl_tx_backpressure; return kIOReturnNoResources; }
    mbuf_t attempt=NULL;
    if(mbuf_dup(frame,MBUF_DONTWAIT,&attempt)) {
        ++com->sc_awdl_tx_copy_failed; return kIOReturnNoMemory;
    }
    node->ni_chan=&com->sc_ic.ic_channels[channel];
    // mwx_tx consumes attempt on every return, including mapping/ring failure.
    // Preserve the queued frame's bytes, sequence and timestamp until success.
    int rv=mwx_tx(com,attempt,node);
    com->sc_awdl_tx_errno=rv;
    if(rv) { ++com->sc_awdl_tx_dma_failed; return kIOReturnNoResources; }
    ++com->sc_awdl_tx_submitted;
    mbuf_freem(frame); *packet=NULL; return kIOReturnSuccess;
}

IOReturn MtkMwx::awdlPeer(const uint8_t *address,bool add)
{
    MwxAwdlCall call(com);
    if(!call.valid()) return kIOReturnNotReady;
    if(!com || !com->sc_awdl_enabled || com->sc_resetting || com->sc_detaching)
        return kIOReturnNotReady;
    if(!address || !mtkawdl::unicast(address)) return kIOReturnBadArgument;
    // The caller may pass node.ni_macaddr itself when deleting a peer.
    // Preserve it before clearing that node after the firmware response.
    uint8_t peerAddress[6]; memcpy(peerAddress,address,6); address=peerAddress;
    MwxAwdlOperation operation(com);
    if(!operation.valid()) return kIOReturnNotReady;
    int index=-1;
    for(unsigned i=0;i<3;++i) if(com->sc_awdl_peer_used[i] &&
        !memcmp(address,com->sc_awdl_peers[i].ni.ni_macaddr,6)) { index=i; break; }
    if(index>=0 && add) return kIOReturnSuccess;
    if(index<0 && !add) return kIOReturnSuccess;
    if(index<0) for(unsigned i=0;i<3;++i) if(!com->sc_awdl_peer_used[i]) { index=i; break; }
    if(index<0) return kIOReturnNoResources;
    // Primary nodes use 1..14. AWDL unicast uses 15..17, broadcast 18;
    // infrastructure broadcast retains 19.
    mwx_node &node=com->sc_awdl_peers[index];
    uint16_t wcid=MWX_WTBL_STA+index;
    if(add && com->sc_awdl_station_pending[index]) {
        int rv=mwx_awdl_remove_station(com,operation.epoch,index);
        if(!operation.valid()) return kIOReturnAborted;
        if(rv) return kIOReturnIOError;
    }
    mbuf_t m=mwx_alloc_sta_req_tlv(sizeof(sta_req_hdr));
    if(!m) return kIOReturnNoMemory;
    uint16_t count=0;
    auto basic=(sta_rec_basic *)mwx_append_tlv(m,&count,STA_REC_BASIC,sizeof(sta_rec_basic));
    // MT76 explicitly excludes MT7921 from P2P station connection types:
    // remote stations use CONNECTION_INFRA_STA even on a P2P AP interface.
    // Keep the discovery BSS type separate from this chip's station type,
    // including disconnect records for the same WCID.
    basic->conn_type=htole32(STA_TYPE_STA | NETWORK_INFRA);
    basic->conn_state=add ? CONN_STATE_PORT_SECURE : CONN_STATE_DISCONNECT;
    basic->extra_info=htole16(EXTRA_INFO_VER | (add ? EXTRA_INFO_NEW : 0));
    memcpy(basic->peer_addr,address,6);
    auto wtbl=(sta_rec_wtbl *)mwx_append_tlv(m,&count,STA_REC_WTBL,sizeof(sta_rec_wtbl));
    wtbl->wlan_idx_lo=wcid; wtbl->operation=WTBL_RESET_AND_SET;
    uint16_t wcount=0, wlen=sizeof(*wtbl);
    if(add) {
        auto generic=(wtbl_generic *)mwx_append_tlv(m,&wcount,WTBL_GENERIC,sizeof(wtbl_generic));
        memcpy(generic->peer_addr,address,6); generic->muar_idx=com->sc_awdl_vif.omac_idx;
        auto rx=(wtbl_rx *)mwx_append_tlv(m,&wcount,WTBL_RX,sizeof(wtbl_rx));
        rx->rca1=rx->rca2=rx->rv=1;
        auto trans=(wtbl_hdr_trans *)mwx_append_tlv(m,&wcount,WTBL_HDR_TRANS,sizeof(wtbl_hdr_trans));
        trans->no_rx_trans=1;
        wlen+=sizeof(*generic)+sizeof(*rx)+sizeof(*trans);
    }
    wtbl->tlv_num=htole16(wcount); wtbl->len=htole16(wlen);
    m=mwx_fill_sta_req_hdr(m,&com->sc_awdl_vif,com->sc_awdl_vif.omac_idx,wcid,count);
    int seq=0; uint32_t status=0;
    if(add) {
        memcpy(node.ni.ni_macaddr,address,6);
        com->sc_awdl_station_pending[index]=true;
    }
    int rv=m ? mwx_mcu_send_mbuf(com,MCU_UNI_CMD_STA_REC_UPDATE,m,&seq) : ENOBUFS;
    if(!operation.valid()) return kIOReturnAborted;
    if(!rv) rv=mwx_mcu_wait_resp_int(com,MCU_UNI_CMD_STA_REC_UPDATE,seq,&status);
    if(!operation.valid()) return kIOReturnAborted;
    if(rv || status) return kIOReturnIOError;
    IOLockLock(com->sc_awdl_lock);
    if(com->sc_awdl_epoch!=operation.epoch || com->sc_awdl_stopping) {
        IOLockUnlock(com->sc_awdl_lock); return kIOReturnAborted;
    }
    memset(&node,0,sizeof(node)); node.wcid=wcid;
    if(add) memcpy(node.ni.ni_macaddr,address,6);
    com->sc_awdl_peer_used[index]=add;
    com->sc_awdl_station_pending[index]=add;
    IOLockUnlock(com->sc_awdl_lock);
    MWX_DEV_LOG(com,"AWDL peer %s WCID=%u add=%d\n",ether_sprintf(address),wcid,add);
    return kIOReturnSuccess;
}
