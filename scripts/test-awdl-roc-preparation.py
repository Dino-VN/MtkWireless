#!/usr/bin/env python3
"""Regression for the production AWDL PM-preparation polling block.

Extracts serviceInGate's exact preparation/submission/request-poll slices,
whole HAL sleep/wake and joining/lifecycle helpers, and awdlReserve's admission
prefix (through PM age and dwell cap). Forward-call revalidation between those
slices is exercised by test-awdl-forward-admission.py, not this fixed-plan test.
The remaining service path is a fixed-plan, 1ms-retry host
boundary; successful admission stops before abort/MCU submission. Clocks,
timers, locks, channel eligibility and Null-frame TX outcomes are host models,
not native workloop, RF, AP ACK, firmware grant or end-to-end scheduler tests.
ASan/UBSan cover C++, not the normal Rust host build. Each invocation rebuilds
an offline, wifi-only mtk-ffi archive in a new temporary target directory and
requires the old production block to fail the same zero-jitter phase assertion.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'kext/Wifi/vendor'
SERVICE = VENDOR / 'AirportMtkWlan/AirportMtkAwdlInterface.cpp'
HAL = VENDOR / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp'

# Previous production block, not a second implementation of the new scheduler.
OLD_BLOCK = r'''
            // MtkWireless: tell the AP we sleep before an off-channel ROC;
            // awdlReserve waits ~2ms after it, so send it ahead of requestAt.
            if(plan.requestAt<=now+3000) hal->awdlInfraSleep(target.channel);
            if(plan.requestAt>now) {
                uint64_t delay=plan.requestAt-now;
                timer->setTimeoutUS(static_cast<uint32_t>(delay>2000 ? 2000 : delay)); return;
            }
'''


def definition(source, signature, end='\n}\n'):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    return source[start:source.index(end, start) + len(end)]


def fixture():
    service = definition(SERVICE.read_text(), 'void AirportMtkAwdlInterface::serviceInGate()')
    # Extract the exact timing slices around the scheduler's call/session
    # revalidation. This fixed-plan fixture owns no forward intent; the full
    # production service regression separately exercises every intervening
    # callback check. Preserve all original phase/latency/negative assertions.
    start = service.index('            // MtkWireless: clip the poll at the PM preparation edge, not')
    end = service.index('            const uint64_t pmCall=', start)
    submission = service.index('            hal->awdlInfraSleep(target.channel);', end)
    refreshed = service.index('            now=awdlNow();\n', submission) + len('            now=awdlNow();\n')
    request = service.index('            if(plan.requestAt>now) {', refreshed)
    request_end = service.index('\n            }\n', request) + len('\n            }\n')
    preparation = service[start:end] + service[submission:refreshed] + service[request:request_end]
    assert 'forward.completeCall(pmCall)' in service[refreshed:request]
    assert 'radioServiceCurrent(serviceSession)' in service[refreshed:request]
    assert preparation.count('hal->awdlInfraSleep(target.channel)') == 1
    wake = '    hal->awdlInfraWakeIfIdle();\n    uint64_t now=awdlNow();\n'
    assert service.count(wake) == 1
    # The modeled fallthrough/retry below matches these service boundaries.
    assert 'IOReturn status=hal->awdlReserve(target.channel,duration,&timing);' in service
    assert '        timer->setTimeoutMS(1); return;\n    }' in service
    hal = HAL.read_text()
    header = HAL.with_suffix('.hpp').read_text()
    timing = definition(header, '    struct AwdlRocTiming {', '\n    };\n')
    reserve_decl = re.search(r'^    IOReturn awdlReserve\([^\n]+;', header, re.M).group()
    reserve = definition(hal, 'IOReturn MtkMwx::awdlReserve(')
    admission = reserve[:reserve.index('    int abortRv=mwx_awdl_abort(com);')]
    helpers = definition(hal, 'static bool mwx_awdl_current(mwx_softc *sc, uint32_t epoch)\n')
    for name in ('MwxAwdlCall', 'MwxAwdlOperation'):
        helpers += definition(hal, 'class ' + name + ' {', '\n};\n')
    helpers += definition(hal, 'static bool mwx_awdl_infra_joining(mwx_softc *sc)\n')
    helpers += re.search(r'^static const uint32_t mwxAwdlOffChannelMaxMs = \d+;', hal, re.M).group() + '\n'
    for name in ('uint8_t MtkMwx::awdlInfraChannel()', 'void MtkMwx::awdlInfraSleep(',
                 'void MtkMwx::awdlInfraWakeIfIdle()'):
        helpers += definition(hal, name)
    return r'''
#include <AWDL/AwdlLayout.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK_MSG(c, message) do { if(!(c)) { \
    std::fprintf(stderr,"CHECK failed: %s (line %d: %s)\n",message,__LINE__,#c); \
    std::exit(1); } } while(0)
#define CHECK(c) CHECK_MSG(c, #c)
using IOReturn=unsigned;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnBadArgument=2;
constexpr int IEEE80211_M_STA=1, IEEE80211_S_SCAN=0, IEEE80211_S_AUTH=2,
    IEEE80211_S_ASSOC=3, IEEE80211_S_RUN=4;
constexpr unsigned IEEE80211_F_RSNON=1, IFF_RUNNING=1, MWX_FLAG_SCANNING=1, MWX_FLAG_BGSCAN=2;
constexpr uint64_t requestTime=1000000, preparationTime=requestTime-3000;
static uint64_t clockUS;
uint64_t awdlNow() { return clockUS; }
uint64_t mwx_awdl_now() { return clockUS; }
uint64_t mtk_join_now() { return clockUS; }
struct IOLock { bool held=false; };
void IOLockLock(IOLock *lock) { CHECK(lock && !lock->held); lock->held=true; }
void IOLockUnlock(IOLock *lock) { CHECK(lock && lock->held); lock->held=false; }
struct ieee80211_channel { unsigned number=149; bool sixGHz=false; };
bool IEEE80211_IS_CHAN_6GHZ(const ieee80211_channel *c) { return c->sixGHz; }
struct ieee80211_node { bool ni_port_valid=true; ieee80211_channel *ni_chan=nullptr; };
struct ieee80211com {
    int ic_opmode=IEEE80211_M_STA, ic_state=IEEE80211_S_RUN;
    unsigned ic_flags=IEEE80211_F_RSNON;
    ieee80211_node *ic_bss=nullptr;
    struct { unsigned if_flags=IFF_RUNNING; } ic_if;
    struct {
        bool Active=true;
        uint64_t InfraPSRefused=0, InfraPSSleeps=0, InfraPSAtUS=0, InfraPSWakes=0, ROCRefused=0;
    } ic_join_diag;
};
unsigned ieee80211_chan2ieee(ieee80211com *, const ieee80211_channel *c) { return c->number; }
struct Submission { uint32_t latencyUS; int result; };
struct mwx_softc {
    ieee80211_channel ap; ieee80211_node node; ieee80211com sc_ic;
    int sc_ns_state=IEEE80211_S_RUN;
    IOLock lock; IOLock *sc_awdl_lock=&lock;
    bool sc_resetting=false, sc_detaching=false, sc_awdl_stopping=false,
        sc_awdl_enabled=true, sc_awdl_busy=false, sc_awdl_infra_ps=false,
        sc_awdl_roc_abort_pending=false;
    unsigned sc_awdl_calls=0, sc_awdl_epoch=1, sc_flags=0;
    uint64_t sc_awdl_infra_ps_at=0, sc_awdl_infra_ps_failed=0,
        sc_awdl_infra_ps_sleeps=0, sc_awdl_infra_ps_wakes=0,
        sc_awdl_roc_busy_blocked=0, sc_awdl_roc_join_blocked=0,
        sc_awdl_roc_scan_blocked=0, sc_awdl_roc_pm_blocked=0, sc_awdl_roc_abort_retry_at=0;
    mtkawdl::Reservation sc_awdl_roc{};
    std::vector<Submission> pm{{0,0}}; // Repeat the final outcome when exhausted.
    unsigned sleepCalls=0, wakeCalls=0, wakeFailures=0, accepted=0, acceptedDuration=0;
    uint64_t lastSuccessfulPM=0, acceptedAt=0;
    mwx_softc() { node.ni_chan=&ap; sc_ic.ic_bss=&node; }
};
static bool offchanEnabled=true;
bool mwx_awdl_offchan_enabled() { return offchanEnabled; }
int mwx_awdl_infra_pm(mwx_softc *sc, bool sleep) {
    CHECK(!sc->lock.held);
    if(!sleep) { ++sc->wakeCalls; if(sc->wakeFailures) { --sc->wakeFailures; return 5; } return 0; }
    CHECK(!sc->pm.empty());
    const auto result=sc->pm[std::min<size_t>(sc->sleepCalls++,sc->pm.size()-1)];
    clockUS+=result.latencyUS;
    if(!result.result) sc->lastSuccessfulPM=clockUS;
    return result.result; // Submission completion, deliberately NOT an AP ACK.
}
struct MtkMwx {
    mwx_softc *com; bool allowed=true;
    explicit MtkMwx(mwx_softc *sc):com(sc) {}
    bool awdlChannelAllowed(uint8_t channel) { return allowed && channel && channel<=165; }
    uint8_t awdlInfraChannel();
    void awdlInfraSleep(uint8_t);
    void awdlInfraWakeIfIdle();
''' + timing + reserve_decl + r'''
};
''' + helpers + admission + r'''
    // Host success boundary after the exact production admission prefix.
    // No abort, MCU command, grant, or RF ownership is simulated as success.
    CHECK(!infra || channel==infra || (com->sc_awdl_infra_ps &&
          clockUS-com->sc_awdl_infra_ps_at>=2000));
    CHECK(!infra || channel==infra || durationMs<=300);
    ++com->accepted; com->acceptedAt=clockUS; com->acceptedDuration=durationMs;
    com->sc_awdl_roc.request(1,channel,clockUS); // Pending only, real Rust layout/FFI.
    CHECK(com->sc_awdl_roc.pending && !com->sc_awdl_roc.granted);
    return kIOReturnSuccess;
}
struct Timer {
    bool armed=false; uint32_t delay=0; uint64_t due=0;
    void setTimeoutUS(uint32_t us) {
        CHECK(us && us<=2000); armed=true; delay=us; due=clockUS+us;
    }
    void setTimeoutMS(unsigned ms) { setTimeoutUS(ms*1000); }
};
struct Fixture {
    mwx_softc sc; MtkMwx radio{&sc}; MtkMwx *hal=&radio;
    Timer boundaryTimer; Timer *timer=&boundaryTimer;
    mtkawdl::RadioPlan plan{};
    unsigned attempts=0, ageRefusals=0, missingPMRefusals=0;
    uint64_t firstAt=0, firstAge=0; IOReturn firstStatus=kIOReturnSuccess;
    explicit Fixture(uint64_t first=preparationTime) {
        clockUS=first; offchanEnabled=true;
        plan.requestAt=requestTime; plan.window.channel=6; plan.durationMs=400;
    }
    ~Fixture() { CHECK(!sc.lock.held && !sc.sc_awdl_calls && !sc.sc_awdl_busy); }
    void tick(bool old=false) {
        timer->armed=false;
''' + wake + r'''
        mtkawdl::Window target=plan.window; // Fixed plan input; no replanning model.
        if(old) {
''' + OLD_BLOCK + r'''
        } else {
''' + preparation + r'''
        }
        CHECK(now==clockUS); // Refresh after even a slow/failed PM submission.
        CHECK(clockUS>=plan.requestAt);
        const uint64_t calledAt=clockUS, blocked=sc.sc_awdl_roc_pm_blocked;
        MtkMwx::AwdlRocTiming timing{};
        const uint32_t duration=plan.durationMs;
        IOReturn status=hal->awdlReserve(target.channel,duration,&timing);
        if(!attempts++) {
            firstAt=calledAt; firstStatus=status;
            firstAge=sc.sc_awdl_infra_ps ? clockUS-sc.sc_awdl_infra_ps_at : 0;
        }
        if(sc.sc_awdl_roc_pm_blocked!=blocked) {
            if(sc.sc_awdl_infra_ps) { CHECK(clockUS-sc.sc_awdl_infra_ps_at<2000); ++ageRefusals; }
            else ++missingPMRefusals;
        }
        timer->setTimeoutMS(1); // The service's existing post-attempt retry boundary.
    }
};
void drive(Fixture &f, bool old=false, unsigned jitterMax=0, unsigned seed=0) {
    for(unsigned tick=0;tick<64 && !f.sc.accepted;++tick) {
        f.tick(old);
        if(f.sc.accepted) break;
        CHECK(f.timer->armed);
        // Varies on EVERY tick, not a constant phase offset.
        const unsigned jitter=(seed*131+tick*83+tick*tick*17)%(jitterMax+1);
        clockUS=f.timer->due+jitter;
    }
    CHECK(f.sc.accepted==1 && !f.sc.sc_awdl_roc.granted);
}
void phases(bool old) {
    unsigned refused=0, atEdge=0;
    for(unsigned phase=0;phase<2000;++phase) {
        Fixture f(requestTime-10000+phase); drive(f,old);
        CHECK(f.firstAt==requestTime && f.sc.sleepCalls==1 && !f.sc.wakeCalls);
        refused+=f.ageRefusals!=0;
        atEdge+=f.sc.sc_awdl_infra_ps_at==preparationTime;
    }
    std::printf("zero-jitter phases: %u/2000 PM-age refusals\n",refused);
    CHECK_MSG(refused==0,"zero-jitter PM-age refusals");
    CHECK(atEdge==2000);
}
void jitter_and_latency() {
    for(unsigned limit : {500u,1500u}) {
        unsigned refused=0;
        for(unsigned phase=0;phase<2000;++phase) {
            Fixture f(requestTime-10000+phase);
            f.sc.pm={{(phase*37)%(limit+1),0}};
            drive(f,false,limit,phase);
            CHECK(f.firstAt>=requestTime && f.firstAt<=requestTime+limit);
            CHECK(f.sc.sleepCalls==1 && !f.sc.wakeCalls);
            CHECK(f.sc.sc_awdl_infra_ps_at==f.sc.lastSuccessfulPM);
            refused+=f.ageRefusals!=0;
            if(limit==500) CHECK(f.sc.sc_awdl_infra_ps_at<=preparationTime+1000);
        }
        if(limit==500) CHECK(refused==0); // Jitter + submission cost fit the 1ms slack.
        else CHECK(refused>0); // Exceeded slack MUST retain safe refusals/retries.
        std::printf("PASS variable jitter/PM latency up to %uus each: %u/2000 age-refused, all safely admitted\n",
                    limit,refused);
    }
}
void delayed_submission(uint64_t first, uint32_t latency) {
    Fixture f(first); f.sc.pm={{latency,0}}; drive(f);
    const uint64_t submitted=first+latency, called=std::max(requestTime,submitted);
    const uint64_t age=called-submitted;
    const unsigned refusals=age>=2000 ? 0 : unsigned((2000-age+999)/1000);
    CHECK(f.sc.sc_awdl_infra_ps_at==submitted && f.sc.lastSuccessfulPM==submitted);
    CHECK(f.firstAt==called && f.firstAge==age && f.ageRefusals==refusals);
    CHECK(f.firstStatus==(refusals ? kIOReturnNotReady : kIOReturnSuccess));
    CHECK(f.sc.acceptedAt==called+refusals*1000 && f.sc.sleepCalls==1 && !f.sc.wakeCalls);
}
void late_and_failed_pm() {
    for(uint32_t latency : {0u,250u,999u,1000u,1001u,1500u,1999u,2000u,2999u,3000u,3500u,12000u})
        delayed_submission(preparationTime,latency);
    for(uint64_t first : {preparationTime+1,requestTime-2000,requestTime-1999,
                         requestTime-1000,requestTime-1,requestTime,requestTime+1,requestTime+20000})
        delayed_submission(first,0);
    for(unsigned failures : {1u,2u,3u,5u}) {
        Fixture f; f.sc.pm.clear();
        for(unsigned i=0;i<failures;++i) f.sc.pm.push_back({73+i*137,5});
        f.sc.pm.push_back({777,0}); drive(f);
        CHECK(f.sc.sleepCalls==failures+1 && f.sc.sc_awdl_infra_ps_failed==failures);
        CHECK(f.sc.sc_awdl_infra_ps_sleeps==1 && f.ageRefusals>0 && !f.sc.wakeCalls);
        CHECK(f.sc.sc_awdl_infra_ps_at==f.sc.lastSuccessfulPM);
    }
    { Fixture f; f.sc.pm={{333,5}};
      for(unsigned i=0;i<12;++i) {
          f.tick();
          CHECK(!f.sc.accepted && !f.sc.sc_awdl_infra_ps && !f.sc.sc_awdl_infra_ps_at);
          CHECK(!f.sc.lastSuccessfulPM && f.timer->armed); clockUS=f.timer->due;
      }
      CHECK(f.missingPMRefusals>0 && !f.ageRefusals);
      CHECK(f.sc.sc_awdl_infra_ps_failed==f.sc.sleepCalls && !f.sc.sc_awdl_infra_ps_sleeps); }
    // Direct reserve fallback retries a failed PM send, but cannot age it from
    // the earlier failed call (or from entry into a slow successful call).
    { Fixture f; f.sc.pm={{333,5},{777,0}};
      f.hal->awdlInfraSleep(6);
      CHECK(!f.sc.sc_awdl_infra_ps && !f.sc.sc_awdl_infra_ps_at);
      CHECK(f.hal->awdlReserve(6,400)==kIOReturnNotReady);
      CHECK(f.sc.sc_awdl_infra_ps_at==preparationTime+1110 && f.sc.sc_awdl_roc_pm_blocked==1);
      CHECK(!f.sc.accepted); }
    std::puts("PASS initial late service, refreshed clock, PM latency/slack boundaries, failed submissions and retries");
}
void grace_and_guards() {
    { Fixture f; f.hal->awdlInfraSleep(6); const uint64_t pm=clockUS;
      for(unsigned age : {0u,1999u,2000u,3000u,9999u}) {
          clockUS=pm+age; f.hal->awdlInfraWakeIfIdle();
          CHECK(f.sc.sc_awdl_infra_ps && !f.sc.wakeCalls);
      }
      clockUS=pm+10000; f.hal->awdlInfraWakeIfIdle();
      CHECK(!f.sc.sc_awdl_infra_ps && f.sc.sc_awdl_infra_ps_wakes==1); }
    for(bool granted : {false,true}) {
        Fixture f; f.hal->awdlInfraSleep(6);
        auto &roc=f.sc.sc_awdl_roc; roc.channel=6; roc.deadline=clockUS+15000;
        roc.pending=!granted; roc.granted=granted;
        clockUS=granted ? roc.deadline+999 : roc.deadline-1;
        f.hal->awdlInfraWakeIfIdle(); CHECK(f.sc.sc_awdl_infra_ps && !f.sc.wakeCalls);
        clockUS=roc.deadline+(granted ? 1000 : 0);
        f.hal->awdlInfraWakeIfIdle(); CHECK(!f.sc.sc_awdl_infra_ps && f.sc.wakeCalls==1);
    }
    { Fixture f; f.hal->awdlInfraSleep(6);
      f.sc.sc_awdl_roc.channel=149; f.sc.sc_awdl_roc.granted=true;
      f.sc.sc_awdl_roc.deadline=clockUS+20000; clockUS+=10000;
      f.hal->awdlInfraWakeIfIdle(); CHECK(!f.sc.sc_awdl_infra_ps); }
    for(bool joining : {false,true}) {
        Fixture f; f.hal->awdlInfraSleep(6); ++clockUS;
        if(joining) f.sc.node.ni_port_valid=false; else f.sc.sc_awdl_enabled=false;
        f.hal->awdlInfraWakeIfIdle(); CHECK(!f.sc.sc_awdl_infra_ps && f.sc.wakeCalls==1);
    }
    { Fixture f; f.hal->awdlInfraSleep(6); clockUS+=10000; f.sc.wakeFailures=1;
      f.hal->awdlInfraWakeIfIdle(); CHECK(f.sc.sc_awdl_infra_ps && !f.sc.sc_awdl_infra_ps_wakes);
      ++clockUS; f.hal->awdlInfraWakeIfIdle();
      CHECK(!f.sc.sc_awdl_infra_ps && f.sc.sc_awdl_infra_ps_wakes==1 && f.sc.sc_awdl_infra_ps_failed==1); }
    for(unsigned age : {0u,1999u,2000u}) {
        Fixture f; f.hal->awdlInfraSleep(6); clockUS+=age;
        CHECK(f.hal->awdlReserve(6,400)==(age<2000 ? kIOReturnNotReady : kIOReturnSuccess));
        CHECK(f.sc.accepted==unsigned(age>=2000));
        if(f.sc.accepted) CHECK(f.sc.acceptedDuration==300);
    }
    for(unsigned scan : {MWX_FLAG_SCANNING,MWX_FLAG_BGSCAN}) {
        Fixture f; f.sc.sc_flags=scan;
        CHECK(f.hal->awdlReserve(6,400)==kIOReturnNotReady);
        CHECK(f.sc.sc_awdl_roc_scan_blocked==1 && !f.sc.sleepCalls && !f.sc.accepted);
    }
    { Fixture f; f.sc.node.ni_port_valid=false; f.hal->awdlInfraSleep(6);
      CHECK(f.hal->awdlReserve(6,400)==kIOReturnNotReady);
      CHECK(f.sc.sc_awdl_roc_join_blocked==1 && !f.sc.sleepCalls); }
    { Fixture f; offchanEnabled=false;
      CHECK(f.hal->awdlReserve(6,400)==kIOReturnBadArgument && !f.sc.sleepCalls); }
    for(bool associated : {false,true}) {
        Fixture f; if(associated) f.plan.window.channel=149; else f.sc.sc_ic.ic_state=IEEE80211_S_SCAN;
        f.sc.sc_ns_state=f.sc.sc_ic.ic_state; drive(f);
        CHECK(!f.sc.sleepCalls && !f.ageRefusals && !f.sc.sc_awdl_infra_ps);
    }
    std::puts("PASS exact 2ms PM-age guard, 10ms preparation grace, pending/granted grace, forced wake and admission guards");
}
void helpers() {
    const uint64_t values[]={0,1,1999,2000,2999,3000,3001,UINT32_MAX,UINT64_MAX-3000,UINT64_MAX-1,UINT64_MAX};
    for(uint64_t request : values)
        CHECK(mtkawdl::rocPrepareAt(request)==(request<3000 ? 0 : request-3000));
    for(uint64_t now : values) for(uint64_t deadline : values) {
        const uint64_t remaining=deadline>now ? deadline-now : 0;
        CHECK(mtkawdl::rocPollDelayUS(now,deadline)==std::min<uint64_t>(remaining,2000));
    }
    std::puts("PASS real Rust scalar FFI: saturating preparation/subtraction and bounded poll arithmetic");
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !std::strcmp(argv[1],"--old-block")));
    CHECK(mtkawdl::layoutMatches());
    phases(argc==2); // EXACT same assertion must reject the old block.
    if(argc==2) return 0;
    jitter_and_latency(); late_and_failed_pm(); grace_and_guards(); helpers();
    std::puts("PASS AWDL ROC preparation: extracted production + Rust FFI; C++ ASan/UBSan; host models are not RF/ACK verification");
}
'''


def fingerprints():
    paths = {Path(__file__).resolve(), SERVICE, HAL, HAL.with_suffix('.hpp'), ROOT / 'rust/Cargo.lock'}
    paths.update((VENDOR / 'include/AWDL').glob('*.hpp'))
    paths.update((ROOT / 'rust').glob('*.toml'))
    for crate in (ROOT / 'rust').glob('mtk-*'):
        paths.update(crate.rglob('*.rs'))
        paths.update(crate.rglob('*.toml'))
    return {path: hashlib.sha256(path.read_bytes()).digest() for path in paths}


def run(command, env, *, cwd=ROOT, check=True):
    command = [str(arg) for arg in command]
    print('$ ' + shlex.join(command), flush=True)
    result = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True)
    if check and result.returncode:
        print(result.stdout + result.stderr, end='', flush=True)
        result.check_returncode()
    return result


def main():
    before = fingerprints()
    code = fixture()
    env = os.environ.copy()
    if 'DEVELOPER_DIR' not in env and 'CommandLineTools' in run(['xcode-select', '-p'], env).stdout:
        env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
    channel = re.search(r'^channel\s*=\s*"([^"]+)"',
                        (ROOT / 'rust/rust-toolchain.toml').read_text(), re.M).group(1)
    installed = run(['rustup', 'toolchain', 'list'], env).stdout
    assert any(line.split()[0] == channel or line.split()[0].startswith(channel + '-')
               for line in installed.splitlines() if line.split()), 'Pinned toolchain missing; will not install/download'
    env['RUSTUP_TOOLCHAIN'] = channel
    rustc = run(['rustup', 'which', '--toolchain', channel, 'rustc'], env).stdout.strip()
    cargo = run(['rustup', 'which', '--toolchain', channel, 'cargo'], env).stdout.strip()
    env.update(RUSTC=rustc, RUSTC_WRAPPER='', RUSTC_WORKSPACE_WRAPPER='',
               CARGO_NET_OFFLINE='true', RUSTFLAGS='')
    env.pop('CARGO_ENCODED_RUSTFLAGS', None)
    host = re.search(r'^host: (.+)$', run([rustc, '-vV'], env).stdout, re.M).group(1)
    with tempfile.TemporaryDirectory(prefix='mtk-awdl-roc-preparation-') as tmp:
        work = Path(tmp)
        built = run([cargo, 'build', '--offline', '--locked', '--no-default-features',
                     '-p', 'mtk-ffi', '--features', 'wifi', '--target', host,
                     '--target-dir', work / 'target', '--message-format=json-render-diagnostics'],
                    env, cwd=ROOT / 'rust')
        rows = [json.loads(line) for line in built.stdout.splitlines() if line.startswith('{')]
        artifacts = {row['target']['name']: row for row in rows if row.get('reason') == 'compiler-artifact'}
        ffi, wlan = artifacts['mtk_ffi'], artifacts['mtk_wlan']
        assert not ffi['fresh'] and not wlan['fresh'], 'Fresh target unexpectedly reused Rust artifacts'
        assert 'wifi' in ffi['features'] and 'bt' not in ffi['features'] and 'mtk_bt' not in artifacts
        lib = next(Path(name) for name in ffi['filenames'] if name.endswith('.a'))
        assert lib.is_file() and work in lib.parents
        assert before == fingerprints(), 'Production inputs changed during the build; rerun'
        print('PASS fresh offline wifi-only archive: mtk-ffi and mtk-wlan rebuilt (' + channel + ')', flush=True)
        cpp, executable = work / 'test.cpp', work / 'test'
        cpp.write_text(code)
        env['ASAN_OPTIONS'] = 'detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        run(['/usr/bin/clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-I', VENDOR / 'include', cpp, lib, '-o', executable], env)
        print(run([executable], env).stdout, end='', flush=True)
        negative = run([executable, '--old-block'], env, check=False)
        assert negative.returncode == 1 and 'CHECK failed: zero-jitter PM-age refusals' in negative.stderr, negative
        assert 'zero-jitter phases: 999/2000 PM-age refusals' in negative.stdout, negative.stdout
        assert 'AddressSanitizer' not in negative.stderr and 'runtime error:' not in negative.stderr, negative.stderr
        print('PASS old-block negative control: same assertion rejected 999/2000 polling phases', flush=True)
        assert before == fingerprints(), 'Production inputs changed during testing; rerun'


if __name__ == '__main__':
    main()
