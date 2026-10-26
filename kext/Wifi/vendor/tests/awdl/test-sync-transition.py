"""Exercise production sync transitions with gate release and concurrent callers."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
def fn(sig):
 a=s.index(sig); return s[a:s.index('\n}\n',a)+3]
code=r'''
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>
#include <AWDL/AwdlTimeline.hpp>
#define XYLog(...) ((void)0)
using IOReturn=unsigned;using AbsoluteTime=uint64_t;using IOThread=void*;
constexpr unsigned kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBusy=2,kIOReturnTimeout=3,kMillisecondScale=1,THREAD_UNINT=0;
constexpr unsigned THREAD_AWAKENED=0,THREAD_TIMED_OUT=1;
std::atomic<uint64_t> offset{0};
uint64_t awdlNow(){return offset+std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void clock_interval_to_deadline(unsigned n,unsigned,AbsoluteTime*p){*p=n;}
IOThread IOThreadSelf(){thread_local int identity;return &identity;}
struct OSObject{};
struct Gate{
 std::recursive_mutex mutex;std::condition_variable_any cv;
 static thread_local unsigned depth;
 IOReturn runAction(IOReturn(*fn)(OSObject*,void*,void*,void*,void*),void*a,void*b=nullptr){std::lock_guard<std::recursive_mutex> lock(mutex);++depth;auto result=fn(nullptr,a,b,nullptr,nullptr);--depth;return result;}
 IOReturn commandSleep(void*,AbsoluteTime ms,unsigned){
  // IOWorkLoop sleepGate releases/restores the full recursive gate depth.
  for(unsigned i=1;i<depth;++i)mutex.unlock();
  auto result=cv.wait_for(mutex,std::chrono::milliseconds(ms));
  for(unsigned i=1;i<depth;++i)mutex.lock();
  return result==std::cv_status::timeout?THREAD_TIMED_OUT:THREAD_AWAKENED;
 }
 void commandWakeup(void*,bool){cv.notify_all();}
} gate;
thread_local unsigned Gate::depth=0;
Gate*_fCommandGate=&gate;
using mbuf_t=int*;void mbuf_freem(mbuf_t m){delete m;}
struct Timer{bool active=false;void setTimeoutMS(unsigned){active=true;}void cancelTimeout(){active=false;}};
struct Hal{
 bool on=false;unsigned calls=0,status=0,inFlight=0;std::vector<bool> history;
 mtkawdl::Reservation grant{};
 bool awdlGrantSnapshot(mtkawdl::Reservation &out){out=grant;return on;}
 std::function<void()> wait;
 IOReturn awdlEnable(const uint8_t*,bool value){assert(!inFlight++);++calls;history.push_back(value);if(wait)wait();--inFlight;if(!status)on=value;return status;}
};
struct AirportMtkAwdlInterface:OSObject{
 Hal radio;Hal*hal=&radio;Timer clock;Timer*timer=&clock;
 struct{void reset(){}}dataPeers;bool trafficActive=false,trafficPeerInstalled=false;uint8_t installedTrafficPeer[6]{};uint8_t discoveryTxPeers[3][6]{};struct AddbaDecline{uint8_t request[24+9];bool valid;}addbaPending[4]{};uint64_t addbaRequests=0,addbaQueued=0,addbaSubmitted=0,addbaFailed=0,addbaExpired=0,addbaDeferred=0,addbaDropped=0,delbaReceived=0;bool stopping=false,enabled=false;IOThread syncOwner=nullptr;
 uint64_t syncRequests=0,syncWaits=0,syncFailures=0;
 struct{uint8_t mac[6]={2,1,2,3,4,5};void holdClock(uint64_t){}void discardControls(){}}state;
 uint64_t nextAction=0,requestDeadline=0;unsigned requestedChannel=0,grantedChannel=0,queueCount=0,queueHead=0;
 static constexpr unsigned kDataQueueSize=256;mbuf_t queue[kDataQueueSize]{};std::atomic<unsigned> refs{1};
 void retain(){++refs;}void release(){--refs;}void drainCompletions(){}
 uint64_t endedAt=0;unsigned ends=0;
 void notifyAvailabilityWindowEnd(uint64_t now){endedAt=now>awTimeline.radioEnd?awTimeline.radioEnd:now;++ends;awTimeline.close();}
 mtkawdl::Timeline awTimeline;
 void setProperty(const char*,const char*){}void setProperty(const char*,bool){}void setProperty(const char*,uint64_t,unsigned){}
 IOReturn setSync(bool);IOReturn setSyncInGate(bool);
 void finishAvailabilityWindowForDisable();
 static IOReturn syncGated(OSObject*,void*,void*,void*,void*);
};
'''+fn('IOReturn AirportMtkAwdlInterface::setSync(bool')+fn('IOReturn AirportMtkAwdlInterface::syncGated(')+fn('void AirportMtkAwdlInterface::finishAvailabilityWindowForDisable(')+fn('IOReturn AirportMtkAwdlInterface::setSyncInGate(')+r'''
int main(){
 AirportMtkAwdlInterface d;
 // The firmware waiter releases the gate. A second native request enters
 // while the first is waiting, then must wait for the owner's actual result.
 for(bool second:{true,false}){
  uint64_t beforeWaits=d.syncWaits;
  std::mutex m;std::condition_variable cv;bool inside=false,finish=false;
  d.radio.wait=[&]{gate.mutex.unlock();{std::unique_lock<std::mutex> l(m);inside=true;cv.notify_all();cv.wait(l,[&]{return finish;});}gate.mutex.lock();};
  unsigned firstResult=99,secondResult=99;
  std::thread first([&]{firstResult=d.setSync(true);});
  {std::unique_lock<std::mutex> l(m);cv.wait(l,[&]{return inside;});}
  std::thread next([&]{secondResult=d.setSync(second);});
  // Observe actual waiter entry under the production gate.
  bool waited=false;
  for(unsigned i=0;i<200&&!waited;++i){ {std::lock_guard<std::recursive_mutex> l(gate.mutex);waited=d.syncWaits>beforeWaits;}if(!waited)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  {std::lock_guard<std::mutex> l(m);finish=true;}cv.notify_all();
  first.join();next.join();
  assert(waited);
  assert(firstResult==0&&secondResult==0&&d.enabled==second&&d.radio.on==second);
  assert(!d.syncOwner&&d.refs==1);d.radio.wait=nullptr;
 }
 assert(d.syncWaits>0);
 // Native disable at 41TU must finish a real minimum 54TU AW before
 // tearing down firmware. Traffic is disabled throughout the gate sleeps.
 auto startAW=[&](unsigned ageTU){
  assert(!d.setSync(true));uint64_t now=awdlNow();
  d.awTimeline.active={now-ageTU*1024,now+(64-ageTU)*1024,0,6,4};
  d.awTimeline.open=true;d.awTimeline.lastStart=d.awTimeline.active.start;
  d.awTimeline.radioEnd=d.awTimeline.active.end;d.awTimeline.token=7;
  d.awTimeline.grantObservedAt=now-100;
  d.radio.grant.request(7,6,now-100);assert(d.radio.grant.grant(7,6,100,now-100));
 };
 startAW(41);uint64_t awStart=d.awTimeline.lastStart;
 auto realBefore=std::chrono::steady_clock::now();unsigned oldEnds=d.ends;
 std::atomic<bool> disabledTraffic{false};
 std::thread traffic([&]{std::this_thread::sleep_for(std::chrono::milliseconds(2));
  std::lock_guard<std::recursive_mutex> lock(gate.mutex);disabledTraffic=!d.enabled;});
 d.radio.wait=[&]{assert(!d.enabled&&!d.awTimeline.open&&d.ends==oldEnds+1);};
 assert(!d.setSync(false));traffic.join();d.radio.wait=nullptr;
 assert(disabledTraffic&&d.endedAt-awStart>=54*1024&&d.endedAt-awStart<=74*1024);
 assert(std::chrono::steady_clock::now()-realBefore>=std::chrono::milliseconds(10));
 assert(d.endedAt<=awdlNow()&&!d.enabled&&!d.radio.on&&!d.syncOwner);
 // Reset/grant revocation during the real wait cannot fabricate a full AW.
 startAW(1);awStart=d.awTimeline.lastStart;oldEnds=d.ends;
 std::thread revoke([&]{std::this_thread::sleep_for(std::chrono::milliseconds(3));
  std::lock_guard<std::recursive_mutex> lock(gate.mutex);d.radio.grant.cancel();});
 assert(!d.setSync(false));revoke.join();
 assert(d.ends==oldEnds+1&&d.endedAt-awStart<54*1024);
 // Native can synchronously disable from an AW-start callback on a gated
 // stack. Sleep must release the entire gate so reset can revoke the grant.
 startAW(0);awStart=d.awTimeline.lastStart;oldEnds=d.ends;
 std::thread nestedRevoke([&]{std::this_thread::sleep_for(std::chrono::milliseconds(3));
  std::lock_guard<std::recursive_mutex> lock(gate.mutex);d.radio.grant.cancel();});
 assert(!gate.runAction([](OSObject*,void *self,void*,void*,void*){
  return static_cast<AirportMtkAwdlInterface*>(self)->setSync(false);},&d));
 nestedRevoke.join();assert(d.ends==oldEnds+1&&d.endedAt-awStart<54*1024);
 // Detach can finish while the disable waiter has released the gate. Do
 // not enter HAL again after teardown resumes the sleeping native caller.
 startAW(1);unsigned callsBeforeDetach=d.radio.calls;
 std::thread detach([&]{std::this_thread::sleep_for(std::chrono::milliseconds(3));
  std::lock_guard<std::recursive_mutex> lock(gate.mutex);d.stopping=true;});
 assert(d.setSync(false)==kIOReturnNotReady);detach.join();
 assert(d.radio.calls==callsBeforeDetach&&!d.syncOwner&&!d.awTimeline.open);
 d.stopping=false;d.radio.on=false;
 // Actual firmware failure must propagate; no synthetic successful sync.
 d.setSync(false);unsigned failuresBefore=d.syncFailures;d.radio.status=17;
 assert(d.setSync(true)==17&&!d.enabled&&!d.clock.active&&d.syncFailures==failuresBefore+1);d.radio.status=0;
 // Synchronous recursion cannot wait for itself.
 d.radio.wait=[&]{assert(d.setSync(false)==kIOReturnBusy);};assert(!d.setSync(true)&&d.enabled);d.radio.wait=nullptr;
 // Teardown during the firmware wait must not revive timer/queued packets.
 d.queue[0]=new int(3);d.queueCount=1;
 d.radio.wait=[&]{d.stopping=true;d.enabled=false;};assert(d.setSync(true)==kIOReturnNotReady);
 assert(!d.enabled&&!d.clock.active&&!d.queueCount&&!d.syncOwner&&d.refs==1);
 d.radio.wait=nullptr;d.stopping=false;
 // A stuck owner has a bounded wait, releases the caller's retain, and
 // cannot be overwritten by the timed-out request.
 int foreign;d.syncOwner=&foreign;
 offset+=3000001; // deadline is relative, advance it while waiting
 std::thread advance([&]{std::this_thread::sleep_for(std::chrono::milliseconds(20));offset+=3000001;gate.cv.notify_all();});
 assert(d.setSync(true)==kIOReturnTimeout);advance.join();assert(d.syncOwner==&foreign&&d.refs==1);d.syncOwner=nullptr;
 puts("PASS production sync: concurrent enable/enable and enable/disable across firmware gate sleep, real failure propagation, recursion rejection, teardown cancellation, bounded owner wait and retain balance.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-sync-transition-') as tmp:
 p=Path(tmp)/'test.cpp';p.write_text(code);b=Path(tmp)/'test'
 subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-fsanitize=address,undefined','-I'+str(root/'include'),str(p),'-o',str(b)],check=True)
 subprocess.run([str(b)],check=True,timeout=15)
