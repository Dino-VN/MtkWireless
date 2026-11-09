/* SPDX-License-Identifier: GPL-2.0-only
 * MtkWireless: bounded, key-free join diagnostics. Owned by heap-backed ic.
 * Values come from existing validated parsers or locally constructed frames.
 */
#ifndef MTK_JOIN_DIAG_H
#define MTK_JOIN_DIAG_H
#define MTK_JOIN_DIAG_FIELDS(X) \
    X(NodeFlags) \
    X(EapolRxV0) \
    X(EapolRxV1) \
    X(EapolRxV2) \
    X(EapolRxV3) \
    X(EapolRxVUnknown) \
    X(EapolTxV0) \
    X(EapolTxV1) \
    X(EapolTxV2) \
    X(EapolTxV3) \
    X(EapolRejectedDescriptor) \
    X(WCLLinkUpAtUS) \
    X(ConnectCompleteAtUS) \
    X(FirstBeaconAtUS) \
    X(EapolRawM1) \
    X(EapolRawM3) \
    X(EapolRawOther) \
    X(EapolRawAtUS) \
    X(EapolRawState) \
    X(EapolRawWrongMAC) \
    X(EapolRawPreRun) \
    X(ObservedAtUS) \
    X(State) \
    X(RequestedState) \
    X(PortValid) \
    X(IGTKSoftware) \
    X(AuthDMA) \
    X(AuthDMAErrors) \
    X(AssocDMA) \
    X(AssocDMAErrors) \
    X(AuthTxAcked) \
    X(AuthTxFailed) \
    X(AuthTxLastStatus) \
    X(AuthTxLastCount) \
    X(AuthTxLastRaw) \
    X(AuthTxCompletedAtUS) \
    X(AssocTxAcked) \
    X(AssocTxFailed) \
    X(AssocTxLastStatus) \
    X(AssocTxLastCount) \
    X(AssocTxLastRaw) \
    X(AssocTxCompletedAtUS) \
    X(EapolTxAcked) \
    X(EapolTxFailed) \
    X(EapolTxLastStatus) \
    X(EapolTxLastCount) \
    X(EapolTxLastRaw) \
    X(EapolTxCompletedAtUS) \
    X(Attempt) \
    X(Compat) \
    X(AppleJoinFlags) \
    X(AppleIE) \
    X(Active) \
    X(StartedAtUS) \
    X(EndedAtUS) \
    X(Outcome) \
    X(LowerAuth) \
    X(UpperAuth) \
    X(KeyCipher) \
    X(KeyLength) \
    X(DeferredAtUS) \
    X(ReadyAtUS) \
    X(ScanStartedAtUS) \
    X(ScanDoneAtUS) \
    X(ScanRestarted) \
    X(ScanSkippedBusy) \
    X(ScanSuspendedAWDL) \
    X(RunBw) \
    X(RunCenter) \
    X(RunSwitchError) \
    X(MacTimingBand) \
    X(MacTimingSIFSUS) \
    X(MacTimingSlotUS) \
    X(MacTimingICR0) \
    X(MacTimingICR0Readback) \
    X(EapolStartTx) \
    X(EapolStartAtUS) \
    X(AuthRequestedAtUS) \
    X(AuthTaskAtUS) \
    X(AuthCommittedAtUS) \
    X(AuthTxAtUS) \
    X(AuthTxCount) \
    X(AuthRxAtUS) \
    X(AuthRxCount) \
    X(AuthAlgorithm) \
    X(AuthSequence) \
    X(AuthStatus) \
    X(AssocRequestedAtUS) \
    X(AssocTxAtUS) \
    X(AssocTxCount) \
    X(AssocRxAtUS) \
    X(AssocRxCount) \
    X(AssocStatus) \
    X(RunRequestedAtUS) \
    X(RunTaskAtUS) \
    X(RunCommittedAtUS) \
    X(HalError) \
    X(APProtos) \
    X(APAKMs) \
    X(APCiphers) \
    X(APGroupCipher) \
    X(APGroupMgmtCipher) \
    X(APRSNCaps) \
    X(AssocAKMs) \
    X(AssocCipher) \
    X(AssocGroupCipher) \
    X(AssocGroupMgmtCipher) \
    X(AssocRSNCaps) \
    X(PMF) \
    X(EapolRxM1) \
    X(EapolRxM2) \
    X(EapolRxM3) \
    X(EapolRxM4) \
    X(EapolTxM1) \
    X(EapolTxM2) \
    X(EapolTxM3) \
    X(EapolTxM4) \
    X(EapolRxGroup) \
    X(EapolTxGroup) \
    X(EapolRxAtUS) \
    X(EapolTxAtUS) \
    X(EapolRxDescriptor) \
    X(EapolTxDescriptor) \
    X(EapolDescriptorRejected) \
    X(EapolTxEnqueueErrors) \
    X(EapolTxDMA) \
    X(EapolTxDMAErrors) \
    X(EapolRxState) \
    X(EapolRxSuppState) \
    X(EapolTxState) \
    X(MICFailures) \
    X(ReplayDrops) \
    X(M3Reject) \
    X(M3KDEs) \
    X(PTKQueued) \
    X(GTKQueued) \
    X(IGTKQueued) \
    X(PTKInstalled) \
    X(GTKInstalled) \
    X(IGTKInstalled) \
    X(PTKFailed) \
    X(GTKFailed) \
    X(IGTKFailed) \
    X(KeyLastError) \
    X(KeyFirmwareStatus) \
    X(KeyCompletedAtUS) \
    X(PortValidAtUS) \
    X(ROCRefused) \
    X(ROCGrants) \
    X(ROCGrantsDuringHandshake) \
    X(ROCAbortAtUS) \
    X(ROCAbortError) \
    X(DeauthRxAtUS) \
    X(DeauthRxReason) \
    X(DeauthTxAtUS) \
    X(DeauthTxReason) \
    X(DisassocRxAtUS) \
    X(DisassocRxReason) \
    X(DisassocTxAtUS) \
    X(DisassocTxReason) \
    X(MacRequestedAtUS) \
    X(MacPlan) \
    X(FirmwareMacError) \
    X(FirmwareMacAtUS) \
    X(RxBssBeacon) \
    X(RxBssMgmt) \
    X(RxBssData) \
    X(RxBssDataProtected) \
    X(RxBssDataToMe) \
    X(RxBssLastFC) \
    X(RxBssLastLLC) \
    X(RxBssLastAtUS) \
    X(RxBssDataMcast) \
    X(RxBssLastA1) \
    X(RxBssLastRate) \
    X(RxBssMgmtRate) \
    X(RxOtherTAToMe) \
    X(RxOtherTALast) \
    X(RxHalDropAmsdu) \
    X(RxHalDropIcv) \
    X(RxHalDropOther) \
    X(RxHalDropLastReason) \
    X(RxHalDropLastRxd1) \
    X(RxHalDropLastRxd2) \
    X(RxHalDropLastRxd3) \
    X(RxHalDropLastAtUS) \
    X(InfraPSSleeps) \
    X(InfraPSWakes) \
    X(InfraPSRefused) \
    X(InfraPSAtUS) \
    X(InfraPSAtStart)
struct mtk_join_diag {
#define MTK_JOIN_FIELD(name) uint64_t name;
    MTK_JOIN_DIAG_FIELDS(MTK_JOIN_FIELD)
#undef MTK_JOIN_FIELD
    uint8_t WCLMAC[6], AssocMAC[6], FirmwareMAC[6], PTKMAC[6], BSSID[6];
    uint16_t APRSNLength, AssocRSNLength;
    uint8_t APRSN[257], AssocRSN[257];
};
/* MtkWireless: a failed attempt. Later attempts that never left SCAN must
 * not hide the one that associated and then timed out in the 4-way
 * handshake, so those are also kept in *assoc_failed. */
static inline void mtk_join_record_failure(const struct mtk_join_diag *d,
    struct mtk_join_diag *failed, struct mtk_join_diag *assoc_failed) {
    *failed = *d;
    if (d->AssocRxCount || d->WCLLinkUpAtUS || d->EapolRxAtUS || d->EapolTxAtUS)
        *assoc_failed = *d;
}
/* MtkWireless: associated (and in RUN) but not one EAPOL-Key frame from the
 * AP arrived: the iPhone Instant Hotspot failure with HE + PMF negotiated
 * (2026-11-01, 1.0.16: 8.5 s, no M1; WPA2 home AP without them fine).
 * An open (or WEP) join carries no EAPOL at all and never sets ni_port_valid,
 * so "no EAPOL" there is normal, not a stall. Only upper-layer auth
 * (WPA/WPA2/802.1X/SAE) can stall in the 4-way handshake; treating an open
 * join as a stall made every later join take the compat level 2 path
 * (NO_HE|NO_VHT|NO_40 -> 11n/20 MHz, seen 2026-11-06). */
/* WARNING 2026-11-06: this predicate fix alone was built as 1.0.13 and froze
 * the machine on the first post-install boot:
 *   CoreCapture/WiFi/[13,26,22]=watchdog@0xe0823c02~missed beacons timeout
 * With the downgrade gone the link really ran HE/80 MHz and the firmware lost
 * beacons. Do NOT ship this without fixing the 80 MHz/HE instability first;
 * 1.0.10 (downgrade in place) is the known-good build. */
static inline bool mtk_join_eapol_stall(const struct mtk_join_diag *d) {
    return d->AssocRxCount && !d->AssocStatus && d->RunCommittedAtUS &&
        !d->PortValidAtUS && !d->EapolRxAtUS && !d->EapolRawAtUS &&
        d->UpperAuth != 0;
}
static inline uint64_t mtk_join_now(void) {
    struct timeval tv;
    microuptime(&tv);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
enum mtk_join_tx_kind {
    MTK_JOIN_TX_NONE, MTK_JOIN_TX_AUTH, MTK_JOIN_TX_ASSOC, MTK_JOIN_TX_EAPOL
};
/* Locally constructed TX headers. Association's subtype is zero, so a
 * default-zero management subtype also counted every data frame as ASSOC.
 * Only unicast frames can receive an ACK. */
static inline unsigned mtk_join_tx_kind(uint8_t fc0, bool eapol, bool multicast) {
    if (multicast) return MTK_JOIN_TX_NONE;
    if ((fc0 & 0x0c) == 0x08)
        return eapol ? MTK_JOIN_TX_EAPOL : MTK_JOIN_TX_NONE;
    if ((fc0 & 0x0c) != 0) return MTK_JOIN_TX_NONE;
    switch (fc0 & 0xf0) {
    case 0xb0: return MTK_JOIN_TX_AUTH;
    case 0x00: case 0x20: return MTK_JOIN_TX_ASSOC;
    default: return MTK_JOIN_TX_NONE;
    }
}
static inline void mtk_join_tx_dma(struct mtk_join_diag *d, unsigned kind, int error) {
    if (!d->Active) return;
    switch (kind) {
    case MTK_JOIN_TX_AUTH: if (error) ++d->AuthDMAErrors; else ++d->AuthDMA; break;
    case MTK_JOIN_TX_ASSOC: if (error) ++d->AssocDMAErrors; else ++d->AssocDMA; break;
    case MTK_JOIN_TX_EAPOL: if (error) ++d->EapolTxDMAErrors; else ++d->EapolTxDMA; break;
    }
}
/* Rust has validated the TX-free entry. Retain firmware's ACK result and
 * transmission count separately from DMA submission. A token's old join
 * must never update the diagnostics for a newer attempt. */
static inline void mtk_join_tx_complete(struct mtk_join_diag *d, uint64_t attempt,
    unsigned kind, unsigned status, unsigned count, uint32_t raw) {
    if (!d->Active || d->Attempt != attempt) return;
#define MTK_JOIN_TX_RESULT(prefix) \
    if (status) ++d->prefix##TxFailed; else ++d->prefix##TxAcked; \
    d->prefix##TxLastStatus = status; d->prefix##TxLastCount = count; \
    d->prefix##TxLastRaw = raw; d->prefix##TxCompletedAtUS = mtk_join_now();
    switch (kind) {
    case MTK_JOIN_TX_AUTH: { MTK_JOIN_TX_RESULT(Auth) break; }
    case MTK_JOIN_TX_ASSOC: { MTK_JOIN_TX_RESULT(Assoc) break; }
    case MTK_JOIN_TX_EAPOL: { MTK_JOIN_TX_RESULT(Eapol) break; }
    }
#undef MTK_JOIN_TX_RESULT
}
/* Key Info classification; request frames are not handshake messages.
 * Non-ACK M2/M4 distinguished by nonzero Key Data Length, not Secure:
 * legacy WPA M4 need not set Secure. */
static inline unsigned mtk_join_message(uint16_t info, uint16_t paylen) {
    if (info & 0x0800) return 0;
    if (!(info & 0x0008)) return 5;
    if (info & 0x0080) return (info & 0x0100) ? 3 : 1;
    if (info & 0x0100) return paylen ? 2 : 4;
    return 0;
}
static inline void mtk_join_eapol(struct mtk_join_diag *d, bool tx,
    uint16_t info, uint16_t paylen, unsigned state, unsigned suppstate) {
    if (!d->Active) return;
    unsigned msg = mtk_join_message(info, paylen);
    if (tx) {
        d->EapolTxAtUS = mtk_join_now(); d->EapolTxDescriptor = info & 7;
        d->EapolTxState = state;
        switch (info & 7) {
        case 0: ++d->EapolTxV0; break; case 1: ++d->EapolTxV1; break;
        case 2: ++d->EapolTxV2; break; case 3: ++d->EapolTxV3; break;
        }
        switch (msg) {
        case 1: ++d->EapolTxM1; break; case 2: ++d->EapolTxM2; break;
        case 3: ++d->EapolTxM3; break; case 4: ++d->EapolTxM4; break;
        case 5: ++d->EapolTxGroup; break;
        }
    } else {
        d->EapolRxAtUS = mtk_join_now(); d->EapolRxDescriptor = info & 7;
        d->EapolRxState = state; d->EapolRxSuppState = suppstate;
        switch (info & 7) {
        case 0: ++d->EapolRxV0; break; case 1: ++d->EapolRxV1; break;
        case 2: ++d->EapolRxV2; break; case 3: ++d->EapolRxV3; break;
        default: ++d->EapolRxVUnknown; break;
        }
        switch (msg) {
        case 1: ++d->EapolRxM1; break; case 2: ++d->EapolRxM2; break;
        case 3: ++d->EapolRxM3; break; case 4: ++d->EapolRxM4; break;
        case 5: ++d->EapolRxGroup; break;
        }
    }
}
#endif
