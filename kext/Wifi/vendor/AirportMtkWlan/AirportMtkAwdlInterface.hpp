// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_INTERFACE_HPP
#define MTK_AWDL_INTERFACE_HPP
// Recovery builds omit the native AWDL class and its private P2P imports.
#if defined(AIRPORT_WCL) && __IO80211_TARGET == __MAC_26_0 && !defined(AMTK_DISABLE_NATIVE_AWDL)
#define MTK_NATIVE_AWDL 1
#include <Airport/macOS26/IO80211AWDLProtocol.h>
#include <Airport/macOS26/IOSkywalkPacketQueue.h>
#include <AWDL/AwdlProtocol.hpp>
#include <AWDL/AwdlAdvertisers.hpp>
#include <AWDL/AwdlTimeline.hpp>
#include <AWDL/AwdlForwardAdmission.hpp>
#include <AWDL/AwdlRocTrace.hpp>
#include <IOKit/IOTimerEventSource.h>
#include "MtkMwx.hpp"
class AirportMtkWlan;
class AirportMtkAwdlInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(AirportMtkAwdlInterface)
public:
    bool initWithController(AirportMtkWlan*, ether_addr*, MtkMwx*);
    bool start(IOService*) override;
    // Native BSD matching requires queues, pools and a logical link registered
    // through Skywalk, not just a copied RegistrationInfo structure.
    static bool nativeDataPathAvailable() { return true; }
    void *getTxSubQueue(apple80211_wme_ac) override;
    void *getTxCompQueue() override;
    void *getRxCompQueue() override;
    void *getTxPacketPool() override;
    void *getRxPacketPool() override;
    int getNumTxQueues() override { return 1; }
    int getTxHeadroom() override { return 0; }
    UInt getHardwareAssists() override;
    void setHardwareAssists(unsigned int,unsigned int) override;
    IOReturn prepareBSDInterface(ifnet_t,unsigned int) override;
    IOReturn enable(unsigned int) override;
    IOReturn disable(unsigned int) override;
    void enableDatapath() override;
    void disableDatapath() override;
    void free() override;
    void stop(IOService*) override;
    void shutdown();
    void setBSDInterface(ifnet_t);
    IOReturn getHardwareAddress(ether_addr*) override;
    ether_addr getSelfMacAddr() override;
    bool isChipInterfaceReady() override;
    const char *getBSDNamePrefix() override { return "awdl"; }
    IOReturn setMacAddress(ether_addr&) override;
    IOReturn setInterfaceEnable(bool) override;
    bool setLinkState(IO80211LinkState, unsigned int) override;
    IOReturn reportDataPathEvents(unsigned int,void*,unsigned long,bool) override;
    void publishCacheDiagnostics(); // controller watchdog gate, also when sync is off
    UInt32 output(mbuf_t);
    static bool receive(void*, const uint8_t*, size_t, uint8_t, int8_t);
    bool bpfScratchReady();
    IOReturn getSSID(apple80211_ssid_data*) override;
    IOReturn getCHANNEL(apple80211_channel_data*) override;
    IOReturn getBSSID(apple80211_bssid_data*) override;
    IOReturn getSTATE(apple80211_state_data*) override;
    IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;
    IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;
    IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) override;
    IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;
    IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;
    IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) override;
    IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*) override;
    IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;
    IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;
    IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) override;
    IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;
    IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;
    IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*) override;
    IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) override;
    IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;
    IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;
    IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override { return kIOReturnUnsupported; }
    IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override { return kIOReturnUnsupported; }
    IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats*) override { return kIOReturnUnsupported; }
    IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override { return kIOReturnUnsupported; }
    IOReturn getAWDL_CCA(apple80211_awdl_cca*) override { return kIOReturnUnsupported; }
    IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*) override;
    IOReturn getLLW_PARAMS(apple80211_llw_params*) override { return kIOReturnUnsupported; }
    IOReturn setSSID(apple80211_ssid_data*) override { return kIOReturnUnsupported; }
    IOReturn setCHANNEL(apple80211_channel_data*) override { return kIOReturnUnsupported; }
    IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;
    IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;
    IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) override;
    IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;
    IOReturn setAWDL_BSSID(apple80211_awdl_bssid*) override;
    IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;
    IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) override;
    IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;
    IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;
    IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) override;
    IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;
    IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*) override;
    IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*) override;
    IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*) override;
    IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;
    IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration*) override;
    IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) override;
    IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;
    IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;
    IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key*) override { return kIOReturnUnsupported; }
    IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override { return kIOReturnUnsupported; }
    IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*) override;
    IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override { return kIOReturnUnsupported; }
    IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*) override;
    IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override { return kIOReturnUnsupported; }
    IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*) override { return kIOReturnUnsupported; }
    IOReturn setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create*) override { return kIOReturnUnsupported; }
    IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request*) override { return kIOReturnUnsupported; }
    IOReturn setLLW_PARAMS(apple80211_llw_params*) override { return kIOReturnUnsupported; }
    IOReturn setTX_RATE(apple80211_tx_rate*) override { return kIOReturnUnsupported; }
    IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*) override { return kIOReturnUnsupported; }
    IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params*) override { return kIOReturnUnsupported; }
    IOReturn setAWDL_CONFIG(apple80211_awdl_config_request*) override;
    IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*) override { return kIOReturnUnsupported; }
    IOReturn getTX_RATE(apple80211_tx_rate*) override { return kIOReturnUnsupported; }
private:
    static IOReturn startGated(OSObject*,void*,void*,void*,void*);
    bool startInGate(IOService*);
    static IOReturn stopGated(OSObject*,void*,void*,void*,void*);
    void stopInGate(IOService*);
    static IOReturn enableGated(OSObject*,void*,void*,void*,void*);
    IOReturn enableInGate(unsigned int);
    static IOReturn disableGated(OSObject*,void*,void*,void*,void*);
    IOReturn disableInGate(unsigned int);
    static IOReturn interfaceEnableGated(OSObject*,void*,void*,void*,void*);
    IOReturn setInterfaceEnableInGate(bool);
    static IOReturn linkStateGated(OSObject*,void*,void*,void*,void*);
    bool setLinkStateInGate(IO80211LinkState,unsigned int);
    void publishBSDOffloadState(ifnet_t);
    bool createDataPath();
    void releaseDataPath();
    static IOReturn releaseDataPathGated(OSObject*,void*,void*,void*,void*);
    void releaseDataPathInGate();
    void drainCompletions();
    void drainTxStatus();
    void inputNative(const uint8_t*,size_t);
    // True only when Skywalk accepted the packet, not end-to-end delivery.
    bool inputNativeView(const mtkawdl::DataView&);
    // Canonical replay gets an allocation-only preflight. Preparation, decode
    // and enqueue failures still consume replay; this is not a delivery commit.
    struct NativeRxAllocation {
        AirportMtkAwdlInterface *self;
        IOSkywalkPacketBufferPool *pool;
        IOSkywalkRxCompletionQueue *queue;
        IOSkywalkPacket *packet=NULL;
        uint64_t generation;
        uint8_t mac[6];
        NativeRxAllocation(AirportMtkAwdlInterface*,uint64_t,const uint8_t*);
        ~NativeRxAllocation();
        NativeRxAllocation(const NativeRxAllocation&)=delete;
        NativeRxAllocation& operator=(const NativeRxAllocation&)=delete;
        int currentPeer(const uint8_t*,const mtkawdl::DataView&) const;
    };
    bool admitDataRx(int,unsigned,const uint8_t*,const mtkawdl::DataView&,
        uint64_t,uint8_t,int8_t);
    // Returns only this frame's own admission generation, never one changed
    // by a native callback. The aggregate cancels if that token is now stale.
    uint64_t inputCanonicalView(const uint8_t*,const mtkawdl::DataView&,unsigned,
        uint64_t,uint8_t,int8_t,uint64_t,const uint8_t*);
    bool deliverNativeView(NativeRxAllocation&,const mtkawdl::DataView&,size_t);
    static unsigned submitNative(OSObject*,IOSkywalkTxSubmissionQueue*,IOSkywalkPacket *const*,unsigned,void*);
    static IOReturn dataPathGated(OSObject*,void*,void*,void*,void*);
    void setDataPathEnabled(bool);
    static IOReturn shutdownGated(OSObject*,void*,void*,void*,void*);
    void shutdownInGate();
    static void tick(OSObject*, IOTimerEventSource*);
    void service();
    void serviceInGate();
    void publishScheduler(uint64_t);
    void publishRocDiagnostics();
    mtkawdl::RocTrace rocTrace;
    IOReturn recordCacheControlResult(IOReturn,uint64_t);
    // MtkWireless: observations only. Native message delivery can precede
    // entry into the controller gate; use atomics also for publication.
    // Last fields are independent samples, not a transaction-wide snapshot.
    struct CacheDiagnostic {
        uint64_t requests=0, failures=0, requestTimeUS=0;
        uint64_t requestVersionOp=0, requestAddressTag=0;
        uint64_t result=kIOReturnNotReady, durationUS=0, maxDurationUS=0;
        uint64_t firmwareCalls=0, firmwareFailures=0;
        uint64_t firmwareStatus=kIOReturnNotReady, firmwareDurationUS=0;
        uint64_t posts=0, postFailures=0, postStatus=kIOReturnNotReady, postTimeUS=0;
        uint64_t events=0, invalidEvents=0, eventLength=0, eventTimeUS=0;
        uint64_t eventVersionOp=0, eventAddressTag=0, eventStatus=kIOReturnNotReady;
    };
    CacheDiagnostic cacheDiagnostic;
    bool forwardEnabled=true;
    mtkawdl::ForwardAdmission forward;
    mtkawdl::RocChannelTiming forwardTiming;
    mtkawdl::ForwardRadioContext forwardContext{},forwardTimingContext{};
    bool forwardContextSet=false,forwardTimingContextSet=false;
    uint64_t forwardGrantObservedAt=0;
    mtkawdl::ForwardRadioContext forwardRadioContext() {
        mtkawdl::ForwardRadioContext c{};
        c.session=hal->awdlTxSession(); c.infra=hal->awdlInfraChannel(); c.only=hal->awdlOnlyChannel();
        const auto *ic=hal->get80211Controller();
        if(ic) {
            c.joinAttempt=ic->ic_join_diag.Attempt; c.infraState=static_cast<uint8_t>(ic->ic_state);
            if(c.infra && ic->ic_bss) memcpy(c.bssid,ic->ic_bss->ni_bssid,6);
        }
        return c;
    }
    void resetForwardAdmission() {
        forward.reset(); forwardTiming={}; forwardGrantObservedAt=0;
        forwardContextSet=forwardTimingContextSet=false;
    }
    bool radioServiceCurrent(uint64_t session) {
        if(stopping || !enabled || txSession!=session) return false;
        if(hal->awdlTxSession()==session) return true;
        // A HAL-only reset still needs a one-shot for frontend cleanup. Never
        // rearm over a replacement frontend session's timer or teardown.
        timer->setTimeoutMS(1); return false;
    }
    bool abortRadioForService(uint64_t session) {
        const uint64_t call=forwardEnabled ? forward.beginCall(session) : 0;
        if(forwardEnabled && !call) { timer->setTimeoutMS(1); return false; }
        hal->awdlAbort();
        const bool completed=!forwardEnabled || forward.completeCall(call);
        if(!radioServiceCurrent(session) || !completed) return false;
        requestedChannel=grantedChannel=0; requestDeadline=0;
        return true;
    }
    uint64_t txSession = 0;
    uint64_t txStatusCompletions = 0, txStatusDiscarded = 0;
    void notifyAvailabilityWindowStart(uint8_t,uint64_t);
    void notifyAvailabilityWindowEnd(uint64_t);
    IOReturn setSync(bool);
    static IOReturn syncGated(OSObject*,void*,void*,void*,void*);
    IOReturn setSyncInGate(bool);
    mbuf_t makeFrame(const uint8_t*, size_t);
    // Build one QoS A-MSDU from consecutive ready unicast data frames.
    // On success only the following queue entries are consumed; the caller
    // still owns `first`. Returns NULL to keep the legacy one-MPDU path.
    mbuf_t buildAmsdu(mbuf_t first, uint8_t channel, uint64_t now, unsigned &subframes);
    AirportMtkWlan *owner = NULL; MtkMwx *hal = NULL;
    IOTimerEventSource *timer = NULL; bool timerAdded = false, stopping = false, enabled = false;
    // 1.0.41: the first firmware enable must not race NVRM's BAR1 remap, which
    // holds PCIe and the registry busy for tens of seconds at boot. The request
    // is acknowledged (airportd never retries) and completed from service().
    // 1.0.44: also wait for the infra link to reach RUN. The HAL gate that used
    // to reject the request (1.0.25) assumed airportd retries; it does not, so
    // a single early request left AWDL dead for the whole boot.
    bool awdlSystemReady();
    bool readyGatePending = false, readyGateBypassed = false, readyGateForce = false;
    uint64_t readyGateStartUS = 0, readyGateWaitedUS = 0;
    uint64_t readyGateBypassCount = 0, readyGateCompletions = 0, readyGateChecks = 0;
    static constexpr uint64_t kReadyGateMaxUS = 180ull * 1000 * 1000;
    // Firmware commandSleep releases the controller gate. Serialize sync
    // transitions across that interval, including duplicate native requests.
    IOThread syncOwner = NULL;
    uint64_t syncRequests = 0, syncWaits = 0, syncFailures = 0;
    IOSkywalkPacketBufferPool *txPool = NULL, *rxPool = NULL;
    IOSkywalkTxSubmissionQueue *txSubmit = NULL;
    IOSkywalkTxCompletionQueue *txComplete = NULL;
    IOSkywalkRxCompletionQueue *rxComplete = NULL;
    unsigned queuesAdded = 0;
    bool nativeStarted = false, nativeRegistered = false, dataPathEnabled = false;
    bool nativeBaseInitialized = false, receiverRegistered = false;
    IOSkywalkPacket *completions[128] = {};
    unsigned completionCount = 0;
    uint64_t nativeTx = 0, nativeRx = 0, nativeDrops = 0;
    uint64_t dataTxSubmitted = 0, dataTxFailed = 0, dataTxExpired = 0, dataTxDeferred = 0;
    // 1.0.49 diagnostics: distinguish "no ROC grant" from "peer not on this
    // channel". Both currently look like an empty service tick.
    uint64_t dataTxNotReady = 0, dataTxNoGrantTicks = 0;
    uint64_t dataTxGateUnknown = 0;
    uint64_t amsduFrames = 0, amsduSubframes = 0, amsduBytes = 0, amsduAllocFailed = 0;
    // Stations prepared for discovery unicast before native cache registration.
    uint8_t discoveryTxPeers[3][6] = {};
    uint64_t dataRxFrames = 0, dataRxInvalid = 0, dataRxUnknownPeer = 0;
    uint64_t dataRxWrongDestination = 0, dataRxReplay = 0, dataRxDecoded = 0;
    // Heap-owned, bounded trace before the replay gate. awdl0 BPF sees only
    // accepted packets, so it cannot explain a stalled RX sequence window.
    struct __attribute__((packed)) DataRxSample {
        uint64_t timeUS, previousBitmap;
        uint16_t sequence, previousSequence, mpduSequence, payloadLength;
        uint8_t result, channel; int8_t rssi; uint8_t prefixLength;
        uint8_t source[6], destination[6], header[26], prefix[80];
    };
    static_assert(sizeof(DataRxSample)==146,"AWDL RX trace format");
    DataRxSample dataRxTrace[64] = {}, dataRxLastRejected = {};
    uint64_t dataRxTraceCount = 0, dataRxDuplicate = 0, dataRxTooOld = 0;
    void recordDataRx(const uint8_t*,const mtkawdl::DataView&,uint64_t,
        uint8_t,int8_t,uint16_t,uint64_t,bool,uint16_t=0xffff);
    // Rejected frames never reach awdl0 BPF. Keep bounded prefixes so a
    // structural rejection can be distinguished from a harmless QoS Null.
    struct __attribute__((packed)) InvalidDataRxSample {
        uint64_t timeUS;
        uint32_t frameLength;
        uint16_t prefixLength;
        uint8_t channel; int8_t rssi;
        uint8_t prefix[192];
    };
    static_assert(sizeof(InvalidDataRxSample)==208,"invalid AWDL RX trace ABI");
    InvalidDataRxSample dataRxInvalidTrace[32]={};
    InvalidDataRxSample dataRxWrongDestinationLast={};
    uint64_t dataRxInvalidTraceCount=0;
    uint64_t nativeIPv6Tags[128]={},nativeIPv6TagTimes[128]={};
    unsigned nativeIPv6TagNext=0;
    uint64_t nativeIPv6Decoded=0,nativeIPv6Duplicate=0;
    void recordInvalidDataRx(const uint8_t *,size_t,uint64_t,uint8_t,int8_t,bool=false);
    IOReturn dataTxStatus = kIOReturnNotReady;
    uint64_t serviceTicks = 0, reserveAttempts = 0, grantObservations = 0, awStartNotifications = 0;
    uint64_t reserveWaitUS = 0, reserveMaxWaitUS = 0, reserveInvalidations = 0;
    uint32_t reserveInvalidReason = 0;
    uint32_t reserveLeadUS = mtkawdl::rocLeadUs;
    mtkawdl::RocLeadTiming reserveTiming;
    mtkawdl::RocChannelTiming reserveChannelTiming;
    uint64_t grantLatencyObservedAt = 0;
    uint64_t actionSubmitted = 0, actionFailed = 0, actionReceived = 0, lastDiagnostic = 0;
    uint64_t actionHostAccepted = 0, actionHostRejected = 0, actionRSSIRejected = 0;
    uint64_t actionNativeDelivered = 0, actionInvalid = 0, actionNoBuffer = 0;
    uint64_t bpfScratchPrepared = 0, bpfScratchFailed = 0;
    OSObject *bpfScratchPacket = NULL;  // IO80211's, not retained
    uint8_t rejectedAction[2048] = {}; size_t rejectedActionLength = 0;
    int32_t lastActionRSSI = 0;
    uint8_t miTemplate[2048] = {}; size_t miTemplateLength = 0;
    uint64_t miStart = 0, miSubmitted = 0, miFailed = 0;
    uint64_t umiSubmitted = 0, umiFailed = 0, umiExpired = 0, umiDeferred = 0;
    uint64_t umiRequests = 0, umiQueued = 0;
    IOReturn umiStatus = kIOReturnNotReady;
    // Peer ADDBA requests (answered by the service loop through the queue)
    // and DELBAs (firmware session teardown).
    // awdld's sequence; densified with the data channel during transfers.
    mtkawdl::Sequence hostSequence = {}; bool hostSequenceValid = false, densePresence = false;
    uint64_t presenceSampleAt = 0, presenceFrames = 0, presenceActiveAt = 0, presenceTransitions = 0;
    void applySequence(uint64_t);
    void updatePresence(uint64_t);
    struct AddbaDecline { uint8_t request[24+9]; bool valid; } addbaPending[4] = {};
    uint64_t addbaRequests = 0, addbaQueued = 0, addbaSubmitted = 0, addbaFailed = 0, addbaAccepted = 0;
    uint64_t addbaExpired = 0, addbaDeferred = 0, addbaDropped = 0, delbaReceived = 0;
    uint64_t delbaDropped = 0;  // Shared pending queue overflow, not firmware failure.
    uint16_t miCounter = 0; bool miCounterValid = false;
    mtkawdl::Timeline awTimeline;
    void finishAvailabilityWindowForDisable();
    mtkawdl::Window requestedWindow={};
    struct NativePeerSample {
        mtkawdl::Advertisement advertisement;
        uint8_t peer[6]; uint64_t received; bool valid;
    } nativePeerSamples[16]={};
    unsigned nativePeerSampleNext=0;
    uint64_t advertisersRequests=0, advertisersApplied=0, advertisersRejected=0;
    uint64_t advertisersGeneration=0, advertisersAppliedGeneration=0;
    mtkawdl::AdvertiserRecord advertisersRecord={};
    bool advertisersRecordValid=false, advertisersClockValid=false;
    uint16_t advertisersAWCounter=0, advertisersDownCounter=0;
    uint8_t advertisersPeer[6]={}, advertisersRoot[6]={};
    IOReturn reserveStatus = kIOReturnNotReady, actionStatus = kIOReturnNotReady;
    IOReturn miStatus = kIOReturnNotReady;
    uint32_t reserveDuration = 0, secondaryMasterChannel = 0;
    ifnet_t bsd = NULL;
    mtkawdl::State state;
    mtkawdl::DataPeers dataPeers;
    // Controller-gate metadata (initialization/final free are unshared). Zero
    // is a permanent fail-closed exhaustion state, never a wrapping ABA token.
    uint64_t dataRxAdmissionGeneration=1;
    unsigned dataRxPeerProgramming=0;
    void invalidateDataRxAdmission();
    void resetDataPeers();
    int observeDataPeer(const uint8_t*,uint64_t);
    bool cacheDataPeer(const uint8_t*,bool,uint64_t);
    bool trafficDataPeer(const uint8_t*,bool,uint64_t);
    IOReturn programDataPeer(const uint8_t*,bool);
    static IOReturn macAddressGated(OSObject*,void*,void*,void*,void*);
    IOReturn setMacAddressInGate(ether_addr&);
    static IOReturn peerCacheGated(OSObject*,void*,void*,void*,void*);
    IOReturn setPeerCacheInGate(apple80211_peer_cache_control*,uint64_t);
    static IOReturn peerTrafficGated(OSObject*,void*,void*,void*,void*);
    IOReturn setPeerTrafficInGate(apple80211_awdl_peer_traffic_registration*);
    uint8_t requestedChannel = 0, grantedChannel = 0; uint64_t nextAction = 0, requestDeadline = 0;
    uint8_t syncTemplate[2048]; size_t templateLength = 0;
    // 64 frames is ~15 ms at 54 Mbps; an off-channel AWDL slot lasts
    // ~220 ms, so a full-rate sender overflowed it once per period.
    static constexpr unsigned kDataQueueSize = 256;
    mbuf_t queue[kDataQueueSize] = {};
    uint64_t queueTime[kDataQueueSize], queueDeadline[kDataQueueSize];
    unsigned queueHead = 0, queueCount = 0;
    apple80211_awdl_sync_params syncParams;
    apple80211_awdl_extension_state_machine_parameter extension;
    apple80211_awdl_election_id electionId;
    apple80211_awdl_presence_mode presence;
    apple80211_awdl_sync_state syncState;
    apple80211_awdl_election_rssi_thresholds rssiThresholds;
    apple80211_awdl_af_tx_mode afMode;
    apple80211_awdl_min_rate minRate;
    apple80211_awdl_sync_channel_sequence channelSequence;
    uint8_t trafficPeer[6] = {}, installedTrafficPeer[6] = {}; bool trafficActive = false, trafficPeerInstalled = false;
};
#endif
#endif
