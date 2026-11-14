#!/usr/bin/env python3
"""Production MCU slot ownership under ASan/UBSan and ThreadSanitizer.

Extracts bookkeeping and waiter functions verbatim. Locks, time, mbufs and reset
are host boundaries; does not emulate firmware replies or prove RF behaviour.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HAL = ROOT / 'kext/Wifi/vendor/SimpleMtkWlan/hal_mwx/MtkMwx.cpp'
source = HAL.read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 3]


start = source.index('\tuint32_t\t\tsc_mcu_seq;')
end = source.rfind('\n', start, source.index('sc_scan_seq_num;', start)) + 1
fields = source[start:end]
code = r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define DEVNAME(sc) "mcu-host"
#define MSEC_TO_NSEC(n) (uint64_t(n)*1000000)
std::mutex lock;
void *mwx_txq_lock=nullptr;
thread_local bool locked=false;
std::atomic<unsigned> live{0}, freed{0};
void IOLockLock(void *) { assert(!locked); lock.lock(); locked=true; }
void IOLockUnlock(void *) { assert(locked); locked=false; lock.unlock(); }
struct mbuf { unsigned id; explicit mbuf(unsigned i):id(i) { ++live; } };
void m_freem(mbuf *m) { assert(m&&!locked); --live; ++freed; delete m; }
struct mwx_softc {
''' + fields + r'''
    unsigned sc_mcu_timeouts=0;
    mwx_softc() { std::memset(this,0,sizeof(*this)); }
};
unsigned sleeps=0,resets=0,dumps=0;
std::function<void()> onSleep;
int tsleep_nsec(void *,int,const char *,uint64_t ns) {
    assert(!locked&&ns==MSEC_TO_NSEC(10)); ++sleeps; if(onSleep) onSleep(); return 0;
}
void mwx_reset(mwx_softc *) { assert(!locked); ++resets; }
void mwx_dump_mcu_state(mwx_softc *,uint32_t,int) { assert(!locked); ++dumps; }
'''
for signature in ('static uint8_t\nmwx_mcu_claim_seq(', 'static void\nmwx_mcu_cancel_seq(',
                  'static bool\nmwx_mcu_complete_seq(', 'static int\nmwx_mcu_pop_seq(',
                  'static int\nmwx_mcu_sleep_resp(', 'int\nmwx_mcu_wait_resp_int(',
                  'int\nmwx_mcu_wait_resp_msg('):
    code += function(signature)
code += r'''
static void bounded() {
    mwx_softc sc; uint64_t cookies[16]{};
    for(unsigned i=1;i<16;++i) {
        auto seq=mwx_mcu_claim_seq(&sc,100+i,true);
        assert(seq==i&&sc.sc_mcu_wait[seq].mcu_busy);
        cookies[i]=sc.sc_mcu_wait[seq].mcu_cookie;
    }
    assert(mwx_mcu_claim_seq(&sc,200,true)==0&&mwx_mcu_claim_seq(&sc,200,false)==0);
    for(unsigned i=1;i<16;++i) assert(sc.sc_mcu_wait[i].mcu_cookie==cookies[i]);
    mwx_mcu_cancel_seq(&sc,105,5); assert(mwx_mcu_claim_seq(&sc,205,true)==5);
    for(unsigned i=1;i<16;++i) mwx_mcu_cancel_seq(&sc,sc.sc_mcu_wait[i].mcu_cmd,i);
    sc.sc_mcu_cookie=UINT64_MAX; assert(mwx_mcu_claim_seq(&sc,1,true)==0);
}
static void replies() {
    mwx_softc sc;
    auto seq=mwx_mcu_claim_seq(&sc,10,true); auto cookie=sc.sc_mcu_wait[seq].mcu_cookie;
    auto m=new mbuf(1); assert(mwx_mcu_complete_seq(&sc,seq,10,cookie,m,73));
    auto duplicate=new mbuf(2); assert(!mwx_mcu_complete_seq(&sc,seq,10,cookie,duplicate,0)); m_freem(duplicate);
    uint32_t value=0; assert(mwx_mcu_wait_resp_int(&sc,10,seq,&value)==0&&value==73&&!live);
    auto late=new mbuf(3); assert(!mwx_mcu_complete_seq(&sc,seq,10,cookie,late,0)); m_freem(late);
    sc.sc_mcu_seq=0; auto replacement=mwx_mcu_claim_seq(&sc,10,true); assert(replacement==seq);
    auto old=new mbuf(4); assert(!mwx_mcu_complete_seq(&sc,seq,10,cookie,old,0)); m_freem(old);
    auto current=new mbuf(5); assert(mwx_mcu_complete_seq(&sc,seq,10,sc.sc_mcu_wait[seq].mcu_cookie,current,91));
    mbuf *out=nullptr; assert(mwx_mcu_wait_resp_msg(&sc,10,seq,&out)==0&&out==current&&!sc.sc_mcu_wait[seq].mcu_busy); m_freem(out);
    seq=mwx_mcu_claim_seq(&sc,20,false); auto async=new mbuf(6);
    assert(!mwx_mcu_complete_seq(&sc,seq,20,sc.sc_mcu_wait[seq].mcu_cookie,async,0)); m_freem(async); mwx_mcu_cancel_seq(&sc,20,seq);
    // A residual legacy response must be released before reinitializing a slot.
    sc.sc_mcu_seq=0; sc.sc_mcu_wait[1].mcu_m=new mbuf(7);
    assert(mwx_mcu_claim_seq(&sc,30,true)==1&&!live); mwx_mcu_cancel_seq(&sc,30,1);
}
static void waiters() {
    mwx_softc sc; auto seq=mwx_mcu_claim_seq(&sc,40,true);
    assert(mwx_mcu_wait_resp_int(&sc,41,seq,nullptr)==EINVAL&&sc.sc_mcu_wait[seq].mcu_busy);
    for(int invalid:{-1,0,16,255}) assert(mwx_mcu_wait_resp_int(&sc,40,invalid,nullptr)==EINVAL);
    onSleep=[&]{ if(sleeps==3) { auto m=new mbuf(8); assert(mwx_mcu_complete_seq(&sc,seq,40,sc.sc_mcu_wait[seq].mcu_cookie,m,123)); } };
    uint32_t value=0; assert(mwx_mcu_wait_resp_int(&sc,40,seq,&value)==0&&sleeps==3&&value==123&&!live); onSleep=nullptr;
    seq=mwx_mcu_claim_seq(&sc,50,true); unsigned before=sleeps;
    assert(mwx_mcu_wait_resp_int(&sc,50,seq,nullptr)==EWOULDBLOCK&&sleeps==before+300&&resets==1&&dumps==1);
    assert(!sc.sc_mcu_wait[seq].mcu_busy);
    auto m=new mbuf(9); assert(!mwx_mcu_complete_seq(&sc,seq,50,sc.sc_mcu_wait[seq].mcu_cookie,m,0)); m_freem(m);
}
static void concurrent() {
    mwx_softc sc; std::vector<std::thread> workers;
    for(unsigned worker=0;worker<24;++worker) workers.emplace_back([&,worker]{
        for(unsigned i=0;i<200;++i) {
            uint32_t cmd=1000+worker; uint8_t seq;
            while(!(seq=mwx_mcu_claim_seq(&sc,cmd,true))) std::this_thread::yield();
            // Our busy reservation cannot be changed by another sender.
            uint64_t cookie=sc.sc_mcu_wait[seq].mcu_cookie;
            auto m=new mbuf(worker); assert(mwx_mcu_complete_seq(&sc,seq,cmd,cookie,m,worker));
            uint32_t result=UINT32_MAX; assert(mwx_mcu_pop_seq(&sc,cmd,seq,&result,nullptr)==0&&result==worker);
        }
    });
    for(auto &w:workers) w.join();
    for(unsigned i=1;i<16;++i) assert(!sc.sc_mcu_wait[i].mcu_busy&&!sc.sc_mcu_wait[i].mcu_m);
    assert(sc.sc_mcu_cookie==24*200&&!live);
}
int main() { bounded(); replies(); waiters(); concurrent(); assert(!live&&!locked); std::puts("MCU slots: bounds, saturation, reuse cookies, late/duplicate/async replies, timeout ownership and 24 concurrent senders passed"); }
'''
with tempfile.TemporaryDirectory(prefix='mtk-mcu-slots-') as directory:
    cpp = Path(directory) / 'slots.cpp'
    cpp.write_text(code)
    for sanitizer in ('address,undefined', 'thread'):
        binary = Path(directory) / ('slots-' + sanitizer.split(',')[0])
        subprocess.run(['/usr/bin/clang++', '-std=c++17', '-O1', '-g', '-pthread',
                        '-fno-omit-frame-pointer', '-fno-sanitize-recover=all',
                        '-fsanitize=' + sanitizer, str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
