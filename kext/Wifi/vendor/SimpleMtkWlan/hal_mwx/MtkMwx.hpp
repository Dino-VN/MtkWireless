//
//  MtkMwx.hpp
//  SimpleMtkWlan
//
//  Created by laobamac (王孝慈) on 2026/5/17.
//  Copyright © 2026 laobamac (王孝慈). All rights reserved.
//

/*
 * MediaTek MT7922 HAL for SimpleMtkWlan.
 *
 * The driver body is ported from OpenBSD if_mwx.  This header only exposes
 * the SimpleMtkWlan HAL boundary; OpenBSD driver internals stay in MtkMwx.cpp.
 */

#ifndef MtkMwx_hpp
#define MtkMwx_hpp

#include <compat.h>
#include <HAL/MtkHalService.hpp>
#include <HAL/MtkDriverInfo.hpp>
#include <HAL/MtkDriverController.hpp>

struct mwx_softc;
struct pci_attach_args;
namespace mtkawdl { struct Reservation; }

class MtkMwx : public MtkHalService, MtkDriverInfo, MtkDriverController {
    OSDeclareDefaultStructors(MtkMwx)

public:
    bool attach(IOPCIDevice *device) override;
    void detach(IOPCIDevice *device) override;
    IOReturn enable(IONetworkInterface *interface) override;
    IOReturn disable(IONetworkInterface *interface) override;
    struct ieee80211com *get80211Controller() override;
    MtkDriverInfo *getDriverInfo() override;
    MtkDriverController *getDriverController() override;
    void free() override;

    const char *getFirmwareVersion() override;
    int16_t getBSSNoise() override;
    bool is5GBandSupport() override;
    bool is6GBandSupport() override;
    int getTxNSS() override;
    const char *getFirmwareName() override;
    UInt32 supportedFeatures() override;
    const char *getFirmwareCountryCode() override;
    uint32_t getTxQueueSize() override;

    void clearScanningFlags() override;
    void fillDebug(void *out) override;
    IOReturn debugSet(uint32_t op, uint32_t val) override;
    IOReturn setMulticastList(IOEthernetAddress *addr, int count) override;

    static bool mwx_match(IOPCIDevice *device);

    IOEthernetController *mwxController();
    int mwxSleepNsec(void *ident, int priority, const char *wmesg, uint64_t timo);
    void mwxWakeup(void *ident);
    IOReturn waitPowerTransition();

    // Called on the driver's main workloop. RX callback borrows the frame;
    // awdlTransmit always consumes its mbuf, including on error.
    typedef bool (*AwdlReceive)(void *, const uint8_t *, size_t, uint8_t, int8_t);
    void awdlSetReceiver(void *context, AwdlReceive receiver);
    struct AwdlTxResult {
        uint8_t address[6], failed, reason, pid;
        uint16_t wcid, sequence;
        uint64_t session;
    };
    // Polling owns no interface pointer. Active + terminal capacity is 32.
    bool awdlTakeTxResult(AwdlTxResult &result);
    uint64_t awdlTxSession();
    struct AwdlSyncSnapshot {
        uint64_t session;
        uint32_t epoch;
        bool attached, wantedUp, ready;
    };
    // Read on the controller gate. Epoch/session and primary-task admission
    // markers are AWDL-lock protected; settled power and scan/join flags also
    // veto setup. An idle unassociated radio is eligible.
    bool awdlSyncSnapshot(AwdlSyncSnapshot &snapshot);
    struct AwdlSyncGuard {
        uint64_t generation, deadlineUS;
        uint32_t epoch;
        void *context;
        bool (*current)(void *,uint64_t);
    };
    // The optional guard is borrowed only for this gate-ordered call. Its
    // current callback must not sleep, acquire locks or call native services.
    IOReturn awdlEnable(const uint8_t *address, bool enable, const AwdlSyncGuard *guard=nullptr);
    struct AwdlRocTiming;
    IOReturn awdlReserve(uint8_t channel, uint32_t durationMs, AwdlRocTiming *timing=nullptr);
    IOReturn awdlPrepareChannel(uint8_t channel);
    bool awdlCanTransmit(uint8_t channel);
    bool awdlGrantSnapshot(mtkawdl::Reservation &grant);
    // Host observations for one ROC, not firmware/PHY retune timestamps.
    // Session + epoch + token + issuedAt identify it across gate-releasing
    // enqueue calls; queueReturnedAt is an upper bound on actual enqueue.
    struct AwdlRocTiming {
        uint64_t session, issuedAtUS, queueReturnedAtUS, grantAtUS, pmAtUS;
        uint32_t epoch;
        int32_t queueStatus;
        uint8_t token, channel, infraChannel;
    };
    bool awdlRocTimingSnapshot(AwdlRocTiming &timing);
    bool awdlIsEnabled();
    // Native interface registration requires running firmware, not an AP
    // association or a configured/enabled secondary AWDL BSS.
    bool awdlInterfaceReady();
    void awdlPublishDiagnostics();
    void awdlAbort();
    IOReturn awdlTransmit(mbuf_t frame, uint8_t channel);
    // Queue API: leave *frame owned by the caller on a transient failure;
    // clear it after successful submission or permanent rejection.
    IOReturn awdlTryTransmit(mbuf_t *frame, uint8_t channel);
    bool awdlChannelAllowed(uint8_t channel);
    // MtkWireless: the AP's channel while associated, else 0. Leaving it
    // unannounced gets us disassociated; see awdlInfraSleep.
    uint8_t awdlInfraChannel();
    // Whether AWDL may offer `channel` in every idle EAW during a transfer.
    bool awdlDensePresenceAllowed(uint8_t channel);
    // The only channel AWDL may use, or 0 for any: the AP's channel while
    // associated if boot-arg amtk_awdl_offchan=0, else 0.
    uint8_t awdlOnlyChannel();
    // Allowed and, if awdlOnlyChannel() is set, equal to it.
    bool awdlChannelReachable(uint8_t channel);
    // Power-save handshake with the AP around off-channel ROC: Sleep sends
    // PM=1 before leaving for `channel` (no-op on the AP's channel), Wake
    // sends PM=0 once no off-channel ROC is pending or granted. Idempotent.
    void awdlInfraSleep(uint8_t channel);
    void awdlInfraWakeIfIdle();
    IOReturn awdlPeer(const uint8_t *address, bool add);
    // RX Block Ack recipient session for an installed AWDL peer station.
    // kIOReturnNotFound: no station; kIOReturnUnsupported: amtk_awdl_rxba=0.
    IOReturn awdlRxBlockAck(const uint8_t *address, uint8_t tid, uint16_t ssn,
                            uint16_t window, bool enable);
    // TX (originator) Block Ack session to an installed AWDL peer station:
    // QoS data of `tid` to it may then be aggregated (A-MPDU). Boot-arg
    // amtk_awdl_txba=0: kIOReturnUnsupported. Only TID 0 is supported.
    IOReturn awdlTxBlockAck(const uint8_t *address, uint8_t tid, uint16_t ssn,
                            uint16_t window, bool enable);
    bool awdlTxBlockAckActive(const uint8_t *address, uint8_t tid);
    unsigned awdlPeerCapacity() const { return 3; }

private:
    void releaseAll();

    bool taskQueueOwned;
    struct mwx_softc *com;
    struct pci_attach_args *pci;
};

#endif /* MtkMwx_hpp */
