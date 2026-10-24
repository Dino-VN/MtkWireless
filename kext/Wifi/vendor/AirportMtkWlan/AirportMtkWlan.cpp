//
//  AirportMtkWlan.cpp
//  AirportMtkWlan-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportMtkWlan.hpp"
#include <sys/_netstat.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>

#include "AirportMtkSkywalkInterface.hpp"
#include "MtkPCIEDeviceWrapper.hpp"
#include "MtkPCIe.hpp"
#include "hal_mwx/MtkMwx.hpp"

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportMtkWlan, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IO80211WorkQueue *_fWorkloop;
IOCommandGate *_fCommandGate;

void AirportMtkWlan::releaseAll()
{
#ifdef MTK_NATIVE_AWDL
    stopAwdlInterface();
#endif
    if (fNetIf)
        static_cast<AirportMtkSkywalkInterface *>(fNetIf)->stopWCL();
    OSSafeReleaseNULL(driverFaultReporter);
    OSSafeReleaseNULL(driverCoreFaultReporter);
    OSSafeReleaseNULL(driverFaultStream);
    OSSafeReleaseNULL(driverLogger);
    // Stop only successfully started pipes, while their owner workloop exists.
    if (driverSnapshotsPipeStarted) {
        driverSnapshotsPipe->CCPipe::stopPipe();
        driverSnapshotsPipeStarted = false;
    }
    if (driverDataPathPipeStarted) {
        driverDataPathPipe->CCPipe::stopPipe();
        driverDataPathPipeStarted = false;
    }
    if (driverLogPipeStarted) {
        driverLogPipe->CCPipe::stopPipe();
        driverLogPipeStarted = false;
    }
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    OSSafeReleaseNULL(driverSnapshotsPipe);
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
//            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (scanSource) {
            scanSource->cancelTimeout();
            scanSource->disable();
            _fWorkloop->removeEventSource(scanSource);
            scanSource->release();
            scanSource = NULL;
        }
        if (fWatchdogWorkLoop && watchdogTimer) {
            watchdogTimer->cancelTimeout();
            fWatchdogWorkLoop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
            fWatchdogWorkLoop->release();
            fWatchdogWorkLoop = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
}

void AirportMtkWlan::
eventHandler(struct ieee80211com *ic, int msgCode, void *data)
{
    AirportMtkWlan *that = OSDynamicCast(AirportMtkWlan, ic->ic_ac.ac_if.controller);
    if (!that)
        return;
    IO80211SkywalkInterface *interface = that->fNetIf;
    if (!interface)
        return;
    static_cast<AirportMtkSkywalkInterface *>(interface)->handleWCLEvent(msgCode, data);
}

void AirportMtkWlan::watchdogAction(IOTimerEventSource *timer)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    (*ifp->if_watchdog)(ifp);

    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}

void AirportMtkWlan::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
    AirportMtkWlan *that = (AirportMtkWlan *)owner;
    if (that->fNetIf)
        static_cast<AirportMtkSkywalkInterface *>(that->fNetIf)->completeWCLScan();
}

bool AirportMtkWlan::init(OSDictionary *properties)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool ret = super::init(properties);
    awdlSyncEnable = false;
#ifdef MTK_NATIVE_AWDL
    // Allow native AWDL requests by default on Tahoe. The personality and
    // boot argument can disable it; creation still requires a role-6 request.
    auto option=OSDynamicCast(OSBoolean,getProperty("MTKAWDLExperimental"));
    awdlExperimental=option ? option->isTrue() : true;
    uint32_t awdlOption=0;
    if(PE_parse_boot_argn("amtk_awdl",&awdlOption,sizeof(awdlOption)))
        awdlExperimental=awdlOption==1;
    setProperty("MTKAWDLEnabled",awdlExperimental);
    setProperty("MTKAWDLDiagnosticVersion",static_cast<uint64_t>(13),32);
    setProperty("MTKAWDLNativeDataPathAvailable",AirportMtkAwdlInterface::nativeDataPathAvailable());
    setProperty("MTKAWDLCreateStage","awaiting-request");
    setProperty("MTKAWDLCreateStatus",static_cast<uint64_t>(static_cast<uint32_t>(kIOReturnNotReady)),32);
#endif
    power_state = 0;
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    return ret;
}

IOService* AirportMtkWlan::probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    MtkPCIEDeviceWrapper *wrapper = OSDynamicCast(MtkPCIEDeviceWrapper, provider);
    if (!wrapper) {
        XYLog("%s Not a MtkPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    pciNub = wrapper->pciNub;
    fHalService = wrapper->fHalService;
    if (!pciNub || !fHalService) {
        XYLog("%s Not a valid MtkPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    return super::probe(provider, score);
}

#define LOWER32(x)  ((uint64_t)(x) & 0xffffffff)
#define HIGHER32(x) ((uint64_t)(x) >> 32)

bool AirportMtkWlan::
initCCLogs()
{
    CCPipeOptions driverLogOptions = { 0 };
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 1;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AirportMtkWlan_Logs", sizeof(driverLogOptions.file_name));
    snprintf(driverLogOptions.name, sizeof(driverLogOptions.name), "wlan%d", 0);
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = 0x1000000;
    driverLogOptions.pad10 = 2;
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverLogPipe = CCPipe::withOwnerNameCapacity(this, "com.eiyuumiru.AirportMtkWlan", "DriverLogs", &driverLogOptions);
    XYLog("%s driverLogPipeRet %d\n", __FUNCTION__, driverLogPipe != NULL);
    if (!driverLogPipe || !getWorkLoop())
        return false;
    // The factory only initializes the pipe. startPipe creates its event sources.
    // Qualify the native method: do not dispatch through the legacy CCPipe header.
    driverLogPipeStarted = driverLogPipe->CCPipe::startPipe();
    if (!driverLogPipeStarted)
        return false;
    CCStreamOptions loggerOptions = {};
    CCStream *logger = CCStream::withPipeAndName(driverLogPipe, "AirportMtkWlan", &loggerOptions);
    driverLogger = OSDynamicCast(CCLogStream, logger);
    if (!driverLogger) {
        OSSafeReleaseNULL(logger);
        return false;
    }

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 0;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AppleBCMWLAN_Datapath", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = HIGHER32(0x202800000);
    driverLogOptions.pad10 = LOWER32(0x202800000);
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.eiyuumiru.AirportMtkWlan", "DatapathEvents", &driverLogOptions);
    XYLog("%s driverDataPathPipeRet %d\n", __FUNCTION__, driverDataPathPipe != NULL);
    if (!driverDataPathPipe)
        return false;
    driverDataPathPipeStarted = driverDataPathPipe->CCPipe::startPipe();
    if (!driverDataPathPipeStarted)
        return false;

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0x200000001;
    driverLogOptions.log_data_type = 2;
    strlcpy(driverLogOptions.file_name, "StateSnapshots", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.name, "0", sizeof(driverLogOptions.name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pipe_size = 128;
    driverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.eiyuumiru.AirportMtkWlan", "StateSnapshots", &driverLogOptions);
    XYLog("%s driverSnapshotsPipeRet %d\n", __FUNCTION__, driverSnapshotsPipe != NULL);
    if (!driverSnapshotsPipe)
        return false;
    driverSnapshotsPipeStarted = driverSnapshotsPipe->CCPipe::startPipe();
    if (!driverSnapshotsPipeStarted)
        return false;

    CCStreamOptions faultReportOptions = { 0 };
    faultReportOptions.stream_type = 1;
    faultReportOptions.console_level = 0xFFFFFFFFFFFFFFFF;
    driverFaultStream = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
    CCDataStream *dataStream = OSDynamicCast(CCDataStream, driverFaultStream);
    if (!dataStream || !_fWorkloop) {
        return false;
    }
    driverCoreFaultReporter = CCFaultReporter::withStreamWorkloop(dataStream, _fWorkloop);
    if (!driverCoreFaultReporter) {
        return false;
    }
    driverFaultReporter = IO80211FaultReporter::allocWithParams(driverCoreFaultReporter);
    if (!OSDynamicCast(CommonFaultReporter, driverFaultReporter)) {
        return false;
    }
    XYLog("%s driverFaultReporterRet %d\n", __FUNCTION__, driverFaultReporter != NULL);
    return driverLogPipe && driverDataPathPipe && driverSnapshotsPipe && driverFaultReporter;
}

bool AirportMtkWlan::start(IOService *provider)
{
    if (version_major != 25)
        return false;
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct IOSkywalkEthernetInterface::RegistrationInfo registInfo;
    int boot_value = 0;

    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    setProperty("DriverKitDriver", kOSBooleanFalse);
    if (!initCCLogs()) {
        releaseAll();
        return false;
    }
    if (!super::start(provider)) {
        releaseAll();
        return false;
    }
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        super::stop(provider);
        return false;
    }
    if (initPCIPowerManagment(pciNub) == false) {
        super::stop(pciNub);
        return false;
    }
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportMtkWlan::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(_fCommandGate);
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        releaseAll();
        return false;
    }
    fHalService->initWithController(this, _fWorkloop, _fCommandGate);
    fHalService->get80211Controller()->ic_event_handler = eventHandler;

    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;

    // MtkWireless: 802.11ax is offered by default; amtk_he=0 turns it off.
    int he_enabled = 1;
    PE_parse_boot_argn("amtk_he", &he_enabled, sizeof(he_enabled));
    if (he_enabled != 0)
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_HEON;

    if (!fHalService->attach(pciNub)) {
        XYLog("attach fail\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportMtkWlan::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop->addEventSource(watchdogTimer);
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
    if (!scanSource || _fWorkloop->addEventSource(scanSource) != kIOReturnSuccess) {
        super::stop(provider);
        releaseAll();
        return false;
    }
    scanSource->enable();

    fNetIf = new AirportMtkSkywalkInterface;
    if (!fNetIf || !static_cast<AirportMtkSkywalkInterface *>(fNetIf)->init(this)) {
        XYLog("Skywalk interface init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->setInterfaceRole(1);
    fNetIf->setInterfaceId(1);

    if (!fNetIf->attach(this)) {
        XYLog("attach to service fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!attachInterface(fNetIf, this)) {
        XYLog("attach to interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!IONetworkController::attachInterface((IONetworkInterface **)&bsdInterface,
                                             false
                                             )) {
        XYLog("attach to IONetworkController interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    memset(&registInfo, 0, sizeof(registInfo));
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    if (!fNetIf->mExpansionData->fRegistrationInfo || !fNetIf->mExpansionData2->fRegistrationInfo) {
        super::stop(provider);
        releaseAll();
        return false;
    }
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1)
        fNetIf->deferBSDAttach(true);
    if (!fNetIf->start(this)) {
        super::stop(provider);
        releaseAll();
        return false;
    }

    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess))
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
    bsdInterface->registerService();
    registerService();
    // Do not create the native AWDL interface during controller start.
    // Tahoe may probe/start the controller while Bluetooth and the native
    // peer manager are still coming up; creating a role-6 interface here
    // races that lifecycle and can leave a retained virtual interface during
    // boot.  AWDL is created only from the explicit role-6 request below.
    return true;
}

void AirportMtkWlan::stop(IOService *provider)
{
#ifdef MTK_NATIVE_AWDL
    stopAwdlInterface();
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (fNetIf)
        static_cast<AirportMtkSkywalkInterface *>(fNetIf)->stopWCL();
    super::stop(provider);
    disableAdapter(bsdInterface);
    setLinkStatus(kIONetworkLinkValid);
    fHalService->detach(pciNub);
    ether_ifdetach(ifp);
    detachInterface(fNetIf, true);
    OSSafeReleaseNULL(fNetIf);
    releaseAll();
}

void AirportMtkWlan::free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    if (syncFrameTemplate != NULL && syncFrameTemplateLength > 0) {
        IOFree(syncFrameTemplate, syncFrameTemplateLength);
        syncFrameTemplateLength = 0;
        syncFrameTemplate = NULL;
    }
    if (roamProfile != NULL) {
        IOFree(roamProfile, sizeof(struct apple80211_roam_profile_band_data));
        roamProfile = NULL;
    }
    if (btcProfile != NULL) {
        IOFree(btcProfile, sizeof(struct apple80211_btc_profiles_data));
        btcProfile = NULL;
    }
    super::free();
}

bool AirportMtkWlan::createWorkQueue()
{
    XYLog("%s %d\n", __FUNCTION__, _fWorkloop != 0);
    return _fWorkloop != 0;
}

IO80211WorkQueue *AirportMtkWlan::getWorkQueue()
const
{
    return _fWorkloop;
}

CommonFaultReporter *AirportMtkWlan::getFaultReporterFromDriver()
{
    return driverFaultReporter;
}

IOReturn AirportMtkWlan::enable(IO80211SkywalkInterface *netif)
{
#ifdef MTK_NATIVE_AWDL
    if(netif==awdlInterface) return enableVirtualInterface(awdlInterface);
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::enable(netif);
    _fCommandGate->enable();
    if (power_state)
        enableAdapter(bsdInterface);
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::disable(IO80211SkywalkInterface *netif)
{
#ifdef MTK_NATIVE_AWDL
    if(netif==awdlInterface) return disableVirtualInterface(awdlInterface);
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool AirportMtkWlan::configureInterface(IONetworkInterface *netif)
{
#ifdef MTK_NATIVE_AWDL
    if(creatingAwdl) {
        return IOEthernetController::configureInterface(netif) &&
            netif->configureOutputPullModel(64,kIONetworkWorkLoopSynchronous,0,
                IOEthernetInterface::kOutputPacketSchedulingModelNormal,0)==kIOReturnSuccess;
    }
#endif
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }

    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    // TXWI freelist and DMA rings are also accessed by interrupt events on
    // this work loop. The default pull thread is not serialized with them.
    if (netif->configureOutputPullModel(
            fHalService->getDriverInfo()->getTxQueueSize(),
            kIONetworkWorkLoopSynchronous, 0,
            IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0)
            != kIOReturnSuccess)
        return false;
#endif

    return true;
}

IONetworkInterface *AirportMtkWlan::createInterface()
{
    AirportMtkEthernetInterface *netif = new AirportMtkEthernetInterface;
    if (!netif)
        return NULL;
    if (!netif->initWithSkywalkInterfaceAndProvider(this,
#ifdef MTK_NATIVE_AWDL
        creatingAwdl ? static_cast<IO80211SkywalkInterface *>(awdlInterface) : fNetIf
#else
        fNetIf
#endif
        )) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool AirportMtkWlan::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;

    OSDictionary *mediumDict = OSDictionary::withCapacity(2);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }

    medium = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    medium = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();

    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

IOReturn AirportMtkWlan::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

bool AirportMtkWlan::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    struct _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    // MtkWireless: a same-ESS roam keeps the BSD and WCL link up while
    // net80211 re-associates (docs/ROAMING.md).
    if (fNetIf && static_cast<AirportMtkSkywalkInterface *>(fNetIf)->holdWCLRoamLink(
            status & kIONetworkLinkActive))
        return true;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            bsdInterface->startOutputThread();
#endif
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
//            fNetIf->setLinkQualityMetric(100);
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            bsdInterface->stopOutputThread();
            bsdInterface->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)fHalService->get80211Controller()->ic_deauth_reason);
        }
    }
    return ret;
}

IOReturn AirportMtkWlan::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportMtkWlan *that = OSDynamicCast(AirportMtkWlan, target);
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    static_cast<AirportMtkSkywalkInterface *>(that->fNetIf)->handleWCLLink(
        (IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    return kIOReturnSuccess;
}

#ifdef __PRIVATE_SPI__
IOReturn AirportMtkWlan::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
#ifdef MTK_NATIVE_AWDL
    if(interface==awdlBSD && awdlInterface) {
        mbuf_t m=NULL;
        while(kIOReturnSuccess==interface->dequeueOutputPackets(1,&m))
            awdlInterface->output(m);
        return kIOReturnSuccess;
    }
#endif
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::networkInterfaceNotification(
                    IONetworkInterface * interface,
                    uint32_t              type,
                    void *                  argument )
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

UInt32 AirportMtkWlan::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!m || mbuf_type(m) == MBUF_TYPE_FREE) {
        ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR)) {
        ifp->netStat->outputErrors++;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}

const OSString * AirportMtkWlan::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportMtkWlan::newModelString() const
{
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

IOReturn AirportMtkWlan::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn AirportMtkWlan::setHardwareAddress(const void *addrP, UInt32 addrBytes)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
    const UInt8 *address = static_cast<const UInt8 *>(addrP);
    auto ic = fHalService->get80211Controller();
    bool initializing = (ic->ic_if.if_flags & IFF_UP) && !(ic->ic_if.if_flags & IFF_RUNNING);
    auto update = AirportWCL::planMacUpdate(ic->ic_myaddr, address, addrBytes, initializing);
    if (addrBytes >= IEEE80211_ADDR_LEN)
        mtk_printf("AirportMtkWlan-wcl: set mac %02x:%02x:%02x:%02x:%02x:%02x plan=%d "
                   "state=%d if_flags=0x%x\n", address[0], address[1], address[2],
                   address[3], address[4], address[5], int(update), ic->ic_state,
                   ic->ic_if.if_flags);
    if (update == AirportWCL::MacUpdate::Invalid)
        return kIOReturnBadArgument;
    if (update == AirportWCL::MacUpdate::Busy) {
        return kIOReturnBusy;
    }
    if (!bsdInterface || !bsdInterface->getIfnet())
        return kIOReturnNotReady;
    // WCL calls this directly, bypassing IOEthernetInterface's normal MAC ioctl.
    // Update the BSD address used by DHCP/ARP as well as the firmware address.
    errno_t error = ifnet_set_lladdr(bsdInterface->getIfnet(), address, addrBytes);
    if (error)
        return kIOReturnError;
    bsdInterface->setProperty(kIOMACAddress, const_cast<UInt8 *>(address), addrBytes);
    // WCL replays the current address after every driver-available notification.
    // Keep BSD synchronized, but do not restart firmware for a replay.
    if (update == AirportWCL::MacUpdate::Unchanged) {
        return kIOReturnSuccess;
    }
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, (const UInt8 *)addrP);
    if (ic->ic_if.if_flags & IFF_RUNNING) {
        ic->ic_wcl_mac_reconfig = true;
        fHalService->disable(bsdInterface);
        fHalService->enable(bsdInterface);
    }
    return kIOReturnSuccess;
}

UInt32 AirportMtkWlan::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn AirportMtkWlan::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn AirportMtkWlan::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

SInt32 AirportMtkWlan::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

bool AirportMtkWlan::getLogPipes(CCPipe**logPipe, CCPipe**eventPipe, CCPipe**snapshotsPipe)
{
    bool ret = false;
    if (logPipe) {
        *logPipe = driverLogPipe;
        ret = true;
    }
    if (eventPipe) {
        *eventPipe = driverDataPathPipe;
        ret = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = driverSnapshotsPipe;
        ret = true;
    }
    return ret;
}

#define APPLE80211_CAPA_AWDL_FEATURE_AUTO_UNLOCK    0x00000004
#define APPLE80211_CAPA_AWDL_FEATURE_WOW            0x00000080

IOReturn AirportMtkWlan::
getCARD_CAPABILITIES(OSObject *object,
                                     struct apple80211_capability_data *cd)
{
    uint32_t caps = fHalService->get80211Controller()->ic_caps;
    memset(cd, 0, sizeof(struct apple80211_capability_data));

    if (caps & IEEE80211_C_WEP)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_WEP;
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_TKIP | 1 << APPLE80211_CAP_AES_CCM;
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_PMGT)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_PMGT;
    // if (caps & IEEE80211_C_IBSS)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_IBSS;
    // if (caps & IEEE80211_C_HOSTAP)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_HOSTAP;
    // AES not enabled, like on Apple cards

    if (caps & IEEE80211_C_SHSLOT)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHSLOT - 8);
    if (caps & IEEE80211_C_SHPREAMBLE)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHPREAMBLE - 8);
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_WPA1 - 8) | 1 << (APPLE80211_CAP_WPA2 - 8) | 1 << (APPLE80211_CAP_TKIPMIC - 8);
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_TXPMGT)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_TXPMGT - 8);
    // if (caps & IEEE80211_C_MONITOR)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_MONITOR - 8);
    // WPA not enabled, like on Apple cards

    cd->version = APPLE80211_VERSION;
    // WPA2 can use TKIP for group traffic even when unicast uses CCMP.
    // net80211 handles TKIP/MIC in software; hiding it makes Apple80211
    // reject these networks before it submits an association request.
    cd->capabilities[0] = (caps & IEEE80211_C_RSN) ?
        (1 << APPLE80211_CAP_AES_CCM) | (1 << APPLE80211_CAP_TKIP) : 0;
    cd->capabilities[1] &= ~(1 << (APPLE80211_CAP_WPA1 - 8));
    cd->capabilities[5] = 0x40; // Scan completion events.
    cd->capabilities[2] = (1 << (APPLE80211_CAP_WME - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 16));
    // MtkWireless: Instant Hotspot uses SCAN_SSID_LIST, even for one SSID.
    // Tahoe Apple80211Scan uses bit 73 to encode lists in the 0x1598-byte
    // SCAN_REQ accepted by WCL. Without it, it selects legacy IOC 86, then
    // rejects it with -3903 because legacy multiple-scan bit 22 is absent.
    // WCL preserves the target list in setWCL_SCAN_REQ; decodeScan parses it
    // for the firmware's primary and extended SSID arrays.
    cd->capabilities[9] |= 0x02;
    // BSD CARD_CAPABILITIES strips version and returns 21 capability bytes.
    // wifip2pd tests returned byte 4 bit 0 before requesting native AWDL.
#ifdef MTK_NATIVE_AWDL
    if (awdlExperimental && AirportMtkAwdlInterface::nativeDataPathAvailable())
        cd->capabilities[4] |= 0x01;
#endif
    // Tahoe configureValidAWDLChannels reads capability bit 29 for dual band.
    // A single radio can support both bands; this does not advertise RSDB.
    if (fHalService->getDriverInfo()->is5GBandSupport()) cd->capabilities[3] |= 0x20;
    // Tahoe WCLDeviceConfiguration::isHeSupported reads byte 17, bit 7.
    // WCL uses this capability together with the BSS HE IE for PHY reporting.
    if (caps & IEEE80211_C_HE)
        cd->capabilities[17] |= 0x80;
    // MtkWireless: IO80211.framework (Tahoe 25G) _getMFPCaps offers PMF only
    // when byte 2 has 0x11 set (bit 0 used here; bit 4 sits with WoW), and
    // Apple80211IsAuthSelAllowed / Apple80211Associate2 allow WPA3-SAE only
    // with byte 5 bit 3 ("SAE is not supported" otherwise). The driver does
    // SAE itself from the password WCL passes (cipher APPLE80211_CIPHER_PWD).
    if (caps & IEEE80211_C_MFP) {
        cd->capabilities[2] |= 0x01;
        cd->capabilities[5] |= 0x08;
    }
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::
getDRIVER_VERSION(OSObject *object,
                                  struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    snprintf(hv->string, sizeof(hv->string), "AirportMtkWlan: %s%s fw: %s", SMTW_VERSION, GIT_COMMIT, fHalService->getDriverInfo()->getFirmwareVersion());
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::
getHARDWARE_VERSION(OSObject *object,
                                    struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fHalService->getDriverInfo()->getFirmwareVersion(), sizeof(hv->string));
    hv->string_len = strlen(fHalService->getDriverInfo()->getFirmwareVersion());
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::
getCOUNTRY_CODE(OSObject *object,
                                struct apple80211_country_code_data *cd)
{
    char user_override_cc[3];
    const char *cc_fw = fHalService->getDriverInfo()->getFirmwareCountryCode();

    if (!cd)
        return kIOReturnError;
    cd->version = APPLE80211_VERSION;
    memset(user_override_cc, 0, sizeof(user_override_cc));
    PE_parse_boot_argn("amtk_cc", user_override_cc, 3);
    /* user_override_cc > firmware_cc > geo_location_cc */
    strncpy((char*)cd->cc, user_override_cc[0] ? user_override_cc : ((cc_fw[0] == 'Z' && cc_fw[1] == 'Z' && geo_location_cc[0]) ? geo_location_cc : cc_fw), sizeof(cd->cc));
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::
setCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (data->cc[0] != 'x' && data->cc[0] != 'X' &&
        memcmp(geo_location_cc, data->cc, sizeof(geo_location_cc)) != 0) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        if (fNetIf)
            static_cast<AirportMtkSkywalkInterface *>(fNetIf)->handleWCLEvent(
                IEEE80211_EVT_COUNTRY_CODE_UPDATE, nullptr);
    }
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::
getPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 4;
    pd->power_state[0] = power_state;
    pd->power_state[1] = power_state;
    pd->power_state[2] = power_state;
    pd->power_state[3] = power_state;
    return kIOReturnSuccess;
}

IOReturn AirportMtkWlan::
setPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    IOLog("AirportMtkWlan: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & IFF_RUNNING) != 0;
        if (pd->power_state[0] == 0) {
            fHalService->get80211Controller()->ic_wcl_mac_reconfig = false;
            changePowerStateToPriv(kPowerStateOff);
            if (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) {
                net80211_ifstats(fHalService->get80211Controller());
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(kPowerStateOn);
            if (!isRunning)
                enableAdapter(bsdInterface);
        }
        power_state = (pd->power_state[0]);
    }

    return kIOReturnSuccess;
}

SInt32 AirportMtkWlan::apple80211_ioctl(IO80211SkywalkInterface *interface,unsigned long cmd,void *data, bool b1, bool b2)
{
    if (!ml_at_interrupt_context())
        XYLog("%s cmd: %s b1: %d b2: %d\n", __FUNCTION__, convertApple80211IOCTLToString((unsigned int)cmd), b1, b2);
    return kIOReturnUnsupported;
}

SInt32 AirportMtkWlan::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 1 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

SInt32 AirportMtkWlan::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data,void *)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 2 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

IOReturn AirportMtkWlan::enableAdapter(IONetworkInterface *netif)
{
    IOReturn status = fHalService->enable(netif);
    if (status != kIOReturnSuccess)
        return status;
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

void AirportMtkWlan::disableAdapter(IONetworkInterface *netif)
{
#ifdef MTK_NATIVE_AWDL
    if(awdlInterface) {
        apple80211_awdl_sync_enabled disabled={APPLE80211_VERSION,0,0};
        awdlInterface->setAWDL_SYNC_ENABLED(&disabled);
    }
#endif
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    fHalService->disable(netif);
}

IOReturn AirportMtkWlan::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportMtkWlan* dev = OSDynamicCast(AirportMtkWlan, owner);
    if (dev == 0)
        return kIOReturnError;

    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}

bool AirportMtkWlan::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold)
            magicPacketSupported = true;
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    mtkDisableASPM(this, provider);
    return true;
}

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

void AirportMtkWlan::unregistPM()
{
    if (powerOffThreadCall) {
        thread_call_free(powerOffThreadCall);
        powerOffThreadCall = NULL;
    }
    if (powerOnThreadCall) {
        thread_call_free(powerOnThreadCall);
        powerOnThreadCall = NULL;
    }
}

IOReturn AirportMtkWlan::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;

    if (pmPowerState == powerStateOrdinal)
        return result;
    switch (powerStateOrdinal) {
        case kPowerStateOff:
            if (powerOffThreadCall) {
                retain();
                if (thread_call_enter(powerOffThreadCall))
                    release();
                result = 5000000;
            }
            break;
        case kPowerStateOn:
            if (powerOnThreadCall) {
                retain();
                if (thread_call_enter(powerOnThreadCall))
                    release();
                result = 5000000;
            }
            break;

        default:
            break;
    }
    return result;
}

IOReturn AirportMtkWlan::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0,
                             thread_call_param_t param1)
{
    AirportMtkWlan *self = (AirportMtkWlan *)param0;

    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOff,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOff();
        self->release();
    }
}

static void handleSetPowerStateOn(thread_call_param_t param0,
                            thread_call_param_t param1)
{
    AirportMtkWlan *self = (AirportMtkWlan *) param0;

    if (param1 == 0)
    {
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOn,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOn();
        self->release();
    }
}

IOReturn AirportMtkWlan::registerWithPolicyMaker(IOService *policyMaker)
{
    IOReturn ret;

    pmPowerState = kPowerStateOn;
    pmPolicyMaker = policyMaker;

    powerOffThreadCall = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOff,
                                            (thread_call_param_t)this);
    powerOnThreadCall  = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOn,
                                              (thread_call_param_t)this);
    ret = pmPolicyMaker->registerPowerDriver(this,
                                             powerStateArray,
                                             kPowerStateCount);
    return ret;
}

void AirportMtkWlan::setPowerStateOff()
{
    XYLog("%s\n", __FUNCTION__);
    pmPowerState = kPowerStateOff;
    disableAdapter(bsdInterface);
    if (auto *hal = OSDynamicCast(MtkMwx, fHalService)) {
        IOReturn status = hal->waitPowerTransition();
        setProperty("MTKSleepStopStatus", static_cast<uint64_t>(status), 32);
        IOLog("AirportMtkWlan: sleep HAL stop status=0x%x\n", status);
    }
    pmPolicyMaker->acknowledgeSetPowerState();
}

void AirportMtkWlan::setPowerStateOn()
{
    XYLog("%s\n", __FUNCTION__);
    pmPowerState = kPowerStateOn;
    if (power_state && bsdInterface && fHalService) {
        IOReturn status = enableAdapter(bsdInterface);
        if (status == kIOReturnSuccess) {
            if (auto *hal = OSDynamicCast(MtkMwx, fHalService))
                status = hal->waitPowerTransition();
        }
        setProperty("MTKWakeStartStatus", static_cast<uint64_t>(status), 32);
        IOLog("AirportMtkWlan: wake HAL start status=0x%x\n", status);
    }
    pmPolicyMaker->acknowledgeSetPowerState();
}

#ifdef MTK_NATIVE_AWDL
void AirportMtkWlan::recordAwdlCreation(const char *stage, IOReturn status)
{
    awdlCreateStatus=status;
    setProperty("MTKAWDLCreateStage",stage);
    setProperty("MTKAWDLCreateStatus",static_cast<uint64_t>(static_cast<uint32_t>(status)),32);
    // The user-client log ring survives missing unified-log messages.
    mtk_printf("AirportMtkWlan: MTK AWDL create stage=%s status=0x%x\n",stage,status);
    IOLog("AirportMtkWlan: MTK AWDL create stage=%s status=0x%x\n",stage,status);
}
IOReturn AirportMtkWlan::requestAwdlCreation()
{
    if(isInactive() || !_fCommandGate) return kIOReturnNotReady;
    // Run on the caller's thread inside the gate. Native registration must
    // not run on the IO80211WorkQueue event thread (the removed timer did).
    retain();
    IOReturn status=_fCommandGate->runAction(createAwdlGated);
    release();
    return status;
}
IOReturn AirportMtkWlan::createAwdlGated(OSObject *target,void*,void*,void*,void*)
{
    auto self=OSDynamicCast(AirportMtkWlan,target);
    if(!self || self->isInactive()) return kIOReturnNotReady;
    self->setProperty("MTKAWDLCreateSource","manual");
    if(!self->awdlExperimental) return kIOReturnUnsupported;
    if(self->awdlCreating || self->awdlStopping) return kIOReturnBusy;
    // Registration is idempotent even while infrastructure is scanning.
    // Do not replace a successfully registered interface's diagnostic stage
    // with a creation failure solely because en0 disconnected afterwards.
    if(self->awdlInterface) return kIOReturnSuccess;
    return self->createAwdlInterface() ? kIOReturnSuccess : self->awdlCreateStatus;
}
IO80211VirtualInterface *AirportMtkWlan::createVirtualInterface(ether_addr *address,unsigned int role)
{
    IOLog("AirportMtkWlan: MTK AWDL createVirtualInterface role=%u\n",role);
    setProperty("MTKAWDLCreateSource","controller-request");
    if(role!=6 || !awdlExperimental || !createAwdlInterface(address)) return NULL;
    return awdlInterface;
}
SInt32 AirportMtkWlan::enableVirtualInterface(IO80211VirtualInterface *interface)
{
    if(!interface || interface!=awdlInterface || awdlStopping) return kIOReturnUnsupported;
    return awdlInterface->setInterfaceEnable(true);
}
SInt32 AirportMtkWlan::disableVirtualInterface(IO80211VirtualInterface *interface)
{
    if(!interface || interface!=awdlInterface || awdlStopping) return kIOReturnUnsupported;
    apple80211_awdl_sync_enabled disabled={APPLE80211_VERSION,0,0};
    awdlInterface->setAWDL_SYNC_ENABLED(&disabled);
    return awdlInterface->setInterfaceEnable(false);
}
IOReturn AirportMtkWlan::createAwdlInterfaceRequest(apple80211_virt_if_create_data *data)
{
    IOLog("AirportMtkWlan: MTK AWDL VIRTUAL_IF_CREATE version=%u role=%u\n",
          data ? data->version : 0,data ? data->role : 0);
    setProperty("MTKAWDLCreateSource","virtual-if-request");
    if(!data || data->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    if(data->role!=6 || !awdlExperimental) return kIOReturnUnsupported;
    if(awdlCreating || awdlStopping) return kIOReturnBusy;
    bool existed=awdlInterface && !awdlStopping;
    ether_addr address;
    memcpy(&address,data->mac,6);
    if(!existed && !mtkawdl::unicast((uint8_t *)&address)) return kIOReturnBadArgument;
    if(!createAwdlInterface(&address)) return awdlCreateStatus;
    bzero(data->bsd_name,sizeof(data->bsd_name));
    const char *name=awdlInterface->getBSDName();
    if(name && name[0]) strlcpy((char *)data->bsd_name,name,sizeof(data->bsd_name));
    else strlcpy((char *)data->bsd_name,"awdl0",sizeof(data->bsd_name));
    // Native BCM writes the existing BSD name and returns EEXIST on a
    // duplicate request, which lets the caller recover the interface.
    return existed ? EEXIST : kIOReturnSuccess;
}
bool AirportMtkWlan::createAwdlInterface(ether_addr *address)
{
    if(!awdlExperimental) { recordAwdlCreation("disabled",kIOReturnUnsupported); return false; }
    if(awdlCreating || awdlStopping) return false;
    if(awdlInterface) return true;
    // The interface start method defers BSD matching until native queue,
    // pool and logical-link registration succeeds.
    if(!AirportMtkAwdlInterface::nativeDataPathAvailable()) {
        recordAwdlCreation("native-data-path-unavailable",kIOReturnUnsupported);
        return false;
    }
    MtkMwx *hal=OSDynamicCast(MtkMwx,fHalService);
    if(!hal || !fNetIf || !bsdInterface) {
        recordAwdlCreation("infrastructure-not-ready",kIOReturnNotReady); return false;
    }
    // wifip2pd automatically requests role 6 after Wi-Fi becomes available.
    // AWDL is independent of association: INIT/SCAN/AUTH/ASSOC are valid
    // once firmware is running. Reject early boot/power/reset before any
    // native allocation, and keep creation on the native caller's thread.
    auto ic=hal->get80211Controller();
    if(!ic || !hal->awdlInterfaceReady()) {
        recordAwdlCreation("firmware-not-ready",kIOReturnNotReady); return false;
    }
    if(!hal->awdlChannelAllowed(6) && !hal->awdlChannelAllowed(44) && !hal->awdlChannelAllowed(149)) {
        recordAwdlCreation("no-social-channel",kIOReturnNotReady); return false;
    }
    ether_addr mac;
    if(address) {
        if(!mtkawdl::unicast((uint8_t *)address)) {
            recordAwdlCreation("invalid-address",kIOReturnBadArgument); return false;
        }
        memcpy(&mac,address,6);
    } else {
        memcpy(&mac,hal->get80211Controller()->ic_myaddr,6);
        ((uint8_t *)&mac)[0]=(((uint8_t *)&mac)[0]|2)&0xfe;
        ((uint8_t *)&mac)[5]^=0x80;
    }
    awdlCreating=true;
    recordAwdlCreation("allocate",kIOReturnNotReady);
    awdlInterface=new AirportMtkAwdlInterface;
    if(!awdlInterface) {
        recordAwdlCreation("allocate-failed",kIOReturnNoMemory);
        stopAwdlInterface(); return false;
    }
    recordAwdlCreation("native-init",kIOReturnNotReady);
    if(!awdlInterface->initWithController(this,&mac,hal)) {
        stopAwdlInterface(); return false;
    }
    recordAwdlCreation("attach",kIOReturnNotReady);
    if(!awdlInterface->attach(this)) {
        recordAwdlCreation("attach-failed",kIOReturnError);
        stopAwdlInterface(); return false;
    }
    awdlAttached=true;
    awdlInterface->setInterfaceRole(6);
    awdlInterface->setInterfaceId(2);
    recordAwdlCreation("native-start",kIOReturnNotReady);
    if(!awdlInterface->start(this)) {
        if(awdlCreateStatus==kIOReturnNotReady) recordAwdlCreation("native-start-failed",kIOReturnError);
        stopAwdlInterface(); return false;
    }
    awdlStarted=true;
    awdlCreating=false;
    setProperty("MTKAWDLInterfaceReady",true);
    recordAwdlCreation("registered",kIOReturnSuccess);
    // Removing IODeferBSDAttach republishes only the fully registered
    // provider. State is ready before its BSD client can call us back.
    awdlInterface->deferBSDAttach(false);
    return true;
}
void AirportMtkWlan::stopAwdlInterface()
{
    if(awdlStopping) return;
    awdlStopping=true;
    if(!awdlInterface) {
        awdlBSD=NULL;
        creatingAwdl=awdlCreating=false;
        removeProperty("MTKAWDLInterfaceReady");
        awdlStopping=false;
        return;
    }
    awdlInterface->shutdown();
    if(awdlStarted) {
        awdlInterface->stop(this);
        awdlStarted=false;
    }
    if(awdlAttached) {
        awdlInterface->detach(this);
        awdlAttached=false;
    }
    OSSafeReleaseNULL(awdlInterface);
    awdlBSD=NULL;
    creatingAwdl=awdlCreating=false;
    removeProperty("MTKAWDLInterfaceReady");
    awdlStopping=false;
}
#endif
