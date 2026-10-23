// SPDX-License-Identifier: GPL-2.0-or-later
#include <AWDL/AwdlProtocol.hpp>
#include <assert.h>
#include <stdio.h>
#include <vector>
using namespace mtkawdl;
static const uint8_t local[6]={2,1,2,3,4,5}, remote[6]={2,1,2,3,4,6};
static void aggregateTests()
{
    for(size_t size=54;size<=1514;++size) {
        uint8_t eth[1514]={},wire[1542]={};memcpy(eth,local,6);memcpy(eth+6,remote,6);
        eth[12]=0x86;eth[13]=0xdd;eth[14]=0x60;eth[18]=(size-54)>>8;eth[19]=size-54;
        size_t wn=encodeData(wire,sizeof(wire),eth,size,65535);
        std::vector<uint8_t> f(wire,wire+24);f[0]=0x88;f.push_back(0x80);f.push_back(0);
        size_t second=0;
        for(unsigned i=0;i<2;++i) {
            if(i) second=f.size();
            f.insert(f.end(),eth,eth+12);f.push_back((wn-24)>>8);f.push_back(wn-24);
            size_t p=f.size();f.insert(f.end(),wire+24,wire+wn);put16(f.data()+p+10,65535+i);
            if(!i) while((f.size()-26)%4) f.push_back(0);
        }
        assert(validDataFrame(f.data(),f.size()));
        size_t at=26;uint8_t out[1514];
        for(unsigned i=0;i<2;++i) {
            DataView v={};size_t used=dataView(f.data(),f.size(),at,v);assert(used);
            assert(v.sequence==uint16_t(65535+i));assert(decodeDataView(out,sizeof(out),v)==size);
            assert(!memcmp(out,eth,size));assert(!decodeDataView(out,size-1,v));at+=used;
        }
        assert(at==f.size());
        auto bad=f;bad[second+12]=0xff;assert(!validDataFrame(bad.data(),bad.size()));
        bad=f;bad[second+6]^=4;assert(!validDataFrame(bad.data(),bad.size()));
        bad=f;bad.pop_back();assert(!validDataFrame(bad.data(),bad.size()));
        bad=f;bad[second+14+14]=0;assert(!validDataFrame(bad.data(),bad.size()));
        // Hardware supplies an individual subframe with two bytes following
        // its QoS header. Relocation plus pull must recover the original bytes.
        std::vector<uint8_t> sub(f.begin(),f.begin()+26);
        sub.insert(sub.end(),{0xcc,0xdd});sub.insert(sub.end(),f.begin()+second,f.end());
        assert(relocateDeaggregatedHeader(sub.data(),sub.size()));sub.erase(sub.begin(),sub.begin()+2);
        assert(validDataFrame(sub.data(),sub.size()));
        DataView v={};assert(dataView(sub.data(),sub.size(),26,v));
        assert(decodeDataView(out,sizeof(out),v)==size&&!memcmp(out,eth,size));
        if(size==1514) {
            for(size_t len=0;len<f.size();++len) {
                std::vector<uint8_t> shortFrame(f.begin(),f.begin()+len);
                validDataFrame(shortFrame.data(),shortFrame.size());
                relocateDeaggregatedHeader(shortFrame.data(),shortFrame.size());
            }
            uint32_t rng=7921;
            for(unsigned i=0;i<100000;++i) {
                auto mutated=f;rng=rng*1664525+1013904223;mutated[rng%mutated.size()]^=rng>>24;
                validDataFrame(mutated.data(),mutated.size());
            }
        }
    }
}
static void scheduleTests()
{
    for(unsigned presence=1;presence<=64;presence*=2) {
        State tx,rx; const uint64_t epoch=10000000;
        tx.init(remote,epoch); rx.init(local,epoch); tx.metric=10;
        assert(tx.setPresence(presence,epoch));
        tx.baseCounter=65536-presence;
        for(unsigned i=0;i<slots;++i) tx.sequence.channels[i]=(i&1) ? 44 : 6;
        tx.ownSequence=tx.sequence; tx.elect(epoch);
        const uint64_t period=presence*awUs;
        const uint64_t offsets[]={0,1,1023,1024,awUs-1,awUs,period-1,period,period+1,period*17+1023};
        for(uint64_t offset:offsets) {
            uint64_t now=epoch+offset; uint8_t f[256]; Advertisement a={};
            size_t n=buildAction(tx,f,sizeof(f),now);
            assert(n && parseAction(f,n,a));
            assert(a.presence==presence && a.sequence.step+1==presence);
            assert(rx.receiveAction(f,n,now)>=0 && rx.synchronized);
            assert(rx.counter(now)==tx.counter(now));
            assert(rx.channel(now)==tx.channel(now));
            assert(rx.peers[rx.find(remote)].channel(now)==tx.channel(now));
            assert(rx.untilNext(now)>=tx.untilNext(now));
            assert(rx.untilNext(now)-tx.untilNext(now)<1024);
        }
    }
    State s; s.init(local,10000000);
    s.baseCounter=5;
    assert(s.setPresence(8,10000123));
    assert(s.counter(10000123)==5 && s.untilNext(10000123)==3*awUs-123);
    State before=s;
    for(unsigned bad: {0U,3U,5U,65U,255U}) {
        assert(!s.setPresence(bad,10000123)); assert(!memcmp(&before,&s,sizeof(s)));
    }
}
static void roleTests()
{
    State tx,rx; uint8_t f[256]; tx.init(remote,1000000); rx.init(local,1000000);
    tx.metric=10; tx.elect(1000000);
    size_t n=buildAction(tx,f,sizeof(f),1000000);
    assert(rx.setSyncRole(3,local));
    assert(rx.receiveAction(f,n,1000000)>=0 && !rx.synchronized);
    assert(!memcmp(rx.master,local,6));
    assert(rx.setSyncRole(2,remote)); assert(!rx.channel(1000000));
    assert(!buildAction(rx,f,0,1000000));
    rx.elect(1000000); assert(rx.synchronized && !memcmp(rx.sync,remote,6));
    rx.elect(5000000); assert(!rx.synchronized && !rx.channel(5000000));
    assert(rx.setSyncRole(1,local)); assert(rx.channel(5000000));
    State before=rx;
    assert(!rx.setSyncRole(2,local) && !rx.setSyncRole(3,remote) && !rx.setSyncRole(0,local));
    assert(!memcmp(&before,&rx,sizeof(rx)));
    // Never follow a node which names us as its own synchronization source.
    tx.epoch=5000000; memcpy(tx.sync,local,6);
    n=buildAction(tx,f,sizeof(f),5000000);
    assert(rx.receiveAction(f,n,5000000)>=0 && !rx.synchronized);
}
static void nativeClockTests()
{
    for(int drift:{-3000,3000}) for(uint16_t initial:{uint16_t(12),uint16_t(65532)}) {
        State s;s.init(local,1000000);s.nativeElectionManaged=s.nativeParentSelected=true;
        memcpy(s.nativeParent,remote,6);
        auto &p=s.peers[0];p.used=true;p.seen=p.epoch=1000000;p.baseCounter=initial;
        memcpy(p.mac,remote,6);memcpy(p.advertisement.master,remote,6);
        memcpy(p.advertisement.sync,remote,6);p.advertisement.presence=4;
        p.advertisement.sequence=s.sequence;
        s.elect(1000000);assert(s.synchronized&&s.baseCounter==initial);
        uint64_t now=1100000;
        p.seen=now;p.epoch=uint64_t(int64_t(p.epoch)-drift);
        uint32_t before=s.untilNext(now);s.elect(now);
        int32_t change=int32_t(before)-int32_t(s.untilNext(now));
        assert(change==(drift>0?512:-512)&&s.clockCorrections==1);
        uint64_t epoch=s.epoch;uint16_t base=s.baseCounter;
        for(unsigned i=0;i<100;++i)s.elect(now+i);
        assert(s.epoch==epoch&&s.baseCounter==base&&s.clockCorrections==1);
        // A valid but discontinuous counter sample cannot jump this selected
        // native clock. Another fresh observation counts one rejection.
        p.seen=now+1000;p.baseCounter+=64;
        s.elect(p.seen);assert(s.clockRejected==1&&s.epoch==epoch&&s.baseCounter==base);
        assert(!s.synchronized&&s.nativeClockValidated&&s.nativeClockHoldover);
        s.elect(p.seen+1);assert(s.clockRejected==1);
        assert(!s.synchronized&&s.nativeClockHoldover&&s.nativeHoldoverTransitions==1);
        p.seen=now-1;s.elect(now+2000);
        assert(s.clockRejected==1&&s.epoch==epoch&&s.baseCounter==base);
        assert(!s.synchronized&&s.nativeClockHoldover);
        // A later aligned observation resumes discipline without another
        // acquisition or a jump to the rejected counter namespace.
        p.baseCounter=base;p.epoch=epoch;p.seen=now+3000;s.elect(p.seen);
        assert(s.synchronized&&!s.nativeClockHoldover&&s.nativeClockAcquisitions==1);
    }
    // Replace deferred observations instead of accumulating half-TU slews.
    // The hold has an absolute bound even if no grant arrives.
    State s;s.init(local,1000000);s.nativeElectionManaged=s.nativeParentSelected=true;
    memcpy(s.nativeParent,remote,6);
    auto &p=s.peers[0];p.used=true;p.seen=p.epoch=1000000;
    memcpy(p.mac,remote,6);memcpy(p.advertisement.master,remote,6);
    s.elect(1000000);s.holdClock(1200000);
    for(unsigned i=1;i<=20;++i) {
        p.seen=1000000+i*5000;p.epoch=1000000-(i<20?3000:100);
        s.elect(p.seen);s.elect(p.seen+1);
    }
    assert(s.epoch==1000000&&s.clockDeferred==20&&s.clockCorrections==0);
    s.elect(1200000);assert(s.clockCorrections==1&&s.clockSampleAt==1100000);
    auto epoch=s.epoch;auto base=s.baseCounter;
    s.elect(1200001);assert(s.epoch==epoch&&s.baseCounter==base);
    s.holdClock(1500000);assert(s.setSyncRole(3,local)&&!s.clockHoldUntil);
    // Native refreshes roles and parent controls while following the same
    // peer. Role toggles must not readopt a discontinuous AW counter.
    for(uint16_t initial:{uint16_t(18080),uint16_t(65532)}) {
        State clock;clock.init(local,1000000);
        clock.nativeElectionManaged=clock.nativeParentSelected=true;
        memcpy(clock.nativeParent,remote,6);
        auto &parent=clock.peers[0];parent.used=true;parent.seen=parent.epoch=1000000;
        parent.baseCounter=initial;memcpy(parent.mac,remote,6);
        memcpy(parent.advertisement.master,remote,6);clock.elect(1000000);
        uint64_t now=1100000;uint16_t before=clock.counter(now);
        parent.baseCounter=224;parent.epoch=now;parent.seen=now;
        assert(clock.setSyncRole(1,remote));clock.elect(now);
        assert(clock.counter(now)==before);
        assert(clock.setSyncRole(2,remote));clock.elect(now+1);
        assert(clock.counter(now+1)==before&&clock.clockRejected==1);
        // An explicit new native selection supplies another AW namespace.
        // Acquire it at the safe edge instead of rejecting it forever.
        uint8_t other[6]={2,9,8,7,6,5};
        memcpy(parent.mac,other,6);memcpy(parent.advertisement.master,other,6);
        parent.seen=now+2;clock.nativeTimelineStarted=true;clock.selectNativeParent(other);
        clock.elect(now+2);assert(clock.counter(now+2)==224&&clock.clockRejected==1);
        assert(clock.nativeClockValidated&&clock.nativeClockAcquisitions==2);
    }
    // Local warmup AWs must not prevent the first real native clock selection.
    for(uint16_t peerBase:{uint16_t(18080),uint16_t(65532)}) {
        State warm;warm.init(local,1000000);warm.nativeTimelineStarted=true;
        warm.nativeElectionManaged=true;warm.holdClock(1065536);
        auto &p=warm.peers[0];p.used=true;p.seen=p.epoch=1012000;p.baseCounter=peerBase;
        memcpy(p.mac,remote,6);memcpy(p.advertisement.master,remote,6);
        warm.selectNativeParent(remote,remote);warm.elect(1012000);
        assert(warm.counter(1012000)==0&&!warm.nativeClockValidated);
        assert(warm.nativeClockAcquisitionPending&&warm.clockDeferred==1);
        warm.holdClock(0);warm.elect(1065536);
        assert(warm.counter(1065536)==uint16_t(peerBase+3));
        assert(warm.nativeClockValidated&&warm.nativeClockAcquisitions==1&&!warm.clockRejected);
        uint64_t acquiredEpoch=warm.epoch;
        p.baseCounter+=64;p.seen=1065537;
        warm.elect(p.seen);assert(warm.clockRejected==1&&warm.epoch==acquiredEpoch);
        warm.selectNativeParent(remote,remote);warm.elect(p.seen+1);
        assert(warm.nativeClockAcquisitions==1&&warm.clockRejected==1);
        uint8_t newRoot[6]={2,9,8,7,6,4};memcpy(p.advertisement.master,newRoot,6);
        // RX can change the peer's root before native selects it. Rejecting
        // that observation must retain the committed clock identity, so the
        // later native selection still authorizes its real acquisition.
        warm.elect(p.seen+2);
        assert(!memcmp(warm.clockRoot,remote,6)&&!memcmp(warm.master,remote,6));
        assert(warm.nativeClockValidated&&warm.nativeClockAcquisitions==1);
        assert(!warm.synchronized&&warm.nativeClockHoldover);
        warm.selectNativeParent(remote,newRoot);warm.elect(p.seen+2);
        assert(warm.nativeClockAcquisitions==2&&warm.nativeClockValidated);
        assert(warm.synchronized&&!warm.nativeClockHoldover);
        // A different root arriving during deferred acquisition is not the
        // clock selected by native. Wait for its selected tree, or cancellation.
        uint8_t thirdRoot[6]={2,9,8,7,6,3};
        warm.holdClock(1200000);warm.selectNativeParent(remote,thirdRoot);
        uint64_t heldEpoch=warm.epoch;memcpy(p.advertisement.master,thirdRoot,6);
        warm.elect(1100000);assert(warm.epoch==heldEpoch&&warm.nativeClockAcquisitionPending);
        memcpy(p.advertisement.master,newRoot,6);warm.elect(1200000);
        assert(warm.epoch==heldEpoch&&warm.nativeClockAcquisitions==2);
        warm.selectNativeParent(remote,newRoot);warm.elect(1200001);
        assert(!warm.nativeClockAcquisitionPending&&warm.nativeClockAcquisitions==2);
    }
}
static void templateTests()
{
    State tx; tx.init(remote,1000000); uint8_t f[512]; Advertisement a={};
    for(unsigned encoding: {0U,1U,3U}) {
        size_t n=buildAction(tx,f,sizeof(f),1024000);
        std::vector<uint8_t> frame(f,f+n);
        size_t syncLength=le16(frame.data()+41);
        if(encoding) {
            frame.insert(frame.begin()+43+syncLength,16,0);
            put16(frame.data()+41,syncLength+16);
            uint8_t *p=frame.data()+43+33; p[1]=encoding;
            for(unsigned i=0;i<16;++i) {
                p[6+2*i]=encoding==1 ? 0 : 6;
                p[7+2*i]=encoding==1 ? 6 : 81;
            }
            p[38]=p[39]=0;
        }
        assert(parseAction(frame.data(),frame.size(),a));
        std::vector<uint8_t> malformed=frame;
        malformed.push_back(250); // invalid trailing TLV, no mutation allowed
        const std::vector<uint8_t> before=malformed; State stateBefore=tx;
        assert(!refreshAction(tx,malformed.data(),malformed.size(),1065536));
        assert(malformed==before && !memcmp(&stateBefore,&tx,sizeof(tx)));
        // A band/class mismatch must not alias a 6 GHz channel onto 2 GHz.
        if(encoding==3) {
            auto bad=frame; bad[43+33+7]=131;
            assert(!parseAction(bad.data(),bad.size(),a));
        }
        tx.sequence.channels[1]=44; tx.ownSequence=tx.sequence;
        size_t refreshed=refreshAction(tx,frame.data(),frame.size(),1065536);
        assert(refreshed && refreshed<=frame.size());
        assert(parseAction(frame.data(),refreshed,a));
        assert(a.sequence.channels[1]==44 && a.counter==tx.counter(1065536));
        assert(frame[43+34]==0);
        // A changed channel count is encoded exactly, without expanding a
        // three-slot schedule into an inconsistent sixteen-slot repetition.
        tx.sequence.count=3;
        refreshed=refreshAction(tx,frame.data(),refreshed,1080000);
        assert(refreshed && parseAction(frame.data(),refreshed,a));
        assert(a.sequence.count==3);
        tx.sequence.count=16; tx.sequence.channels[1]=6;
    }
}
static void nativeTemplateTests()
{
    State tx,rx; tx.init(remote,1000000); rx.init(local,1000000);
    uint8_t original[256];size_t originalSize=buildAction(tx,original,sizeof(original),1000000);
    // Native builds service/election/data-path TLVs separately; it omits sync.
    size_t electionOffset=43+le16(original+41);
    std::vector<uint8_t> native(original+electionOffset,original+originalSize);
    native.insert(native.end(),{250,2,0,0x12,0x34});
    assert(validTemplateTLVs(native.data(),native.size()));
    uint8_t output[2304];memset(output,0xaa,sizeof(output));State before=tx;
    assert(!buildActionWithTemplate(tx,output,1,1065536,native.data(),native.size()));
    assert(!memcmp(&before,&tx,sizeof(tx))&&output[0]==0xaa);
    size_t n=buildActionWithTemplate(tx,output,sizeof(output),1065536,native.data(),native.size());
    Advertisement a={};assert(n&&parseAction(output,n,a));
    assert(rx.receiveAction(output,n,1065536)>=0&&rx.synchronized);
    assert(!memcmp(output+n-native.size(),native.data(),native.size()));
    // Native template election can become stale while the host AW clock
    // changes masters. Preserve service/vendor TLVs, align the election, and
    // remove the old sync-tree path rather than advertise two masters.
    auto stale=native;
    stale[3+5]=0x99; // first TLV is Election Parameters v2
    stale.insert(stale.end(),{20,6,0,2,9,8,7,6,5});
    State elected=tx; elected.masterMetric=123; elected.selfCounter=17;
    n=buildActionWithTemplate(elected,output,sizeof(output),1080000,stale.data(),stale.size());
    assert(n&&parseAction(output,n,a)&&a.metric==123&&a.selfCounter==17);
    bool unknown=false;
    for(size_t at=40;at<n;) {
        assert(output[at]!=20);
        if(output[at]==250) {unknown=true;assert(output[at+3]==0x12&&output[at+4]==0x34);}
        at+=3+le16(output+at+1);
    }
    assert(unknown);
    for(size_t cut=0;cut<native.size();++cut) {
        std::vector<uint8_t> truncated(native.begin(),native.begin()+cut);
        // Complete TLV prefixes are permitted, partial TLVs must reject.
        if(validTemplateTLVs(truncated.data(),truncated.size())) {
            assert(buildActionWithTemplate(tx,output,sizeof(output),1080000,truncated.data(),truncated.size()));
        }
    }
    auto bad=native;bad.push_back(1);assert(!validTemplateTLVs(bad.data(),bad.size()));
    bad=native;bad[1]=bad[2]=0xff;assert(!validTemplateTLVs(bad.data(),bad.size()));
    std::vector<uint8_t> sync(original+40,original+electionOffset);
    assert(!validTemplateTLVs(sync.data(),sync.size()));
    bad=native;bad.insert(bad.end(),native.begin(),native.begin()+43);
    assert(!validTemplateTLVs(bad.data(),bad.size()));
    uint32_t random=0x7921;
    for(unsigned i=0;i<10000;++i) {
        random=random*1664525+1013904223;bad=native;bad[random%bad.size()]^=uint8_t(random>>24);
        validTemplateTLVs(bad.data(),bad.size());
    }
}
static void nativePeriodTests()
{
    State s; s.init(local,1000000); uint8_t f[256];
    for(unsigned period : {110U,440U}) {
        s.psfPeriod=period;
        size_t n=buildAction(s,f,sizeof(f),1065536);
        Advertisement a{}; assert(n && parseAction(f,n,a));
        assert(le16(f+43+7)==period);
        assert(a.period==16 && a.counter==s.counter(1065536));
    }
}
static void capturedTransitionTests()
{
    FILE *file=fopen("tests/awdl/fixtures/iphone-election-transition.bin","rb"); assert(file);
    uint8_t raw[2048]; size_t n=fread(raw,1,sizeof(raw),file); fclose(file); assert(n==203);
    Advertisement a{};
    assert(validActionEnvelope(raw,n) && parseAction(raw,n,a));
    assert(!memcmp(raw+43+21,remote,6)); // Sync TLV still names the peer.
    assert(!memcmp(a.master,local,6) && !memcmp(a.sync,local,6));
    State rx; rx.init(local,10000000);
    int peer=rx.receiveAction(raw,n,10000000);
    assert(peer>=0 && rx.peers[peer].used && !rx.synchronized);
    assert(rx.peers[peer].advertisement.sequence.channels[8]==6);
    assert(rx.peers[peer].advertisement.sequence.channels[10]==149);
    // A follower naming us cannot make us follow it, but remains a data peer.
    assert(!memcmp(rx.master,local,6));
    auto bad=std::vector<uint8_t>(raw,raw+n);bad[bad.size()-40]=1;
    assert(!parseAction(bad.data(),bad.size(),a)); // Multicast election root rejected.
    puts("PASS captured iPhone election transition: retain channel schedule without creating an election loop.");
}
static void capturedIphoneTests()
{
    capturedTransitionTests();
    FILE *file=fopen("tests/awdl/fixtures/iphone-v9-psf.bin","rb"); assert(file);
    uint8_t raw[2048]; size_t n=fread(raw,1,sizeof(raw),file); fclose(file); assert(n==612);
    Advertisement a{}; assert(validActionEnvelope(raw,n) && parseAction(raw,n,a));
    assert(a.metric==531 && a.counter==10626 && a.down==29 && a.presence==4);
    const uint8_t channels[16]={11,0,149,0,0,0,0,0,6,0,149,0,0,0,0,0};
    assert(!memcmp(a.sequence.channels,channels,16));
    State rx; rx.init(local,10000000);
    assert(rx.receiveAction(raw,n,10000000)>=0 && rx.synchronized && rx.find(raw+10)>=0);
    assert(rx.channel(10000000)==11);
    // Bound and validate the observed three-byte padding, opclass, shape,
    // duplicate schedule and truncated TLVs. Reject atomically.
    for(size_t offset:{size_t(181),size_t(150),size_t(145)}) {
        auto bad=std::vector<uint8_t>(raw,raw+n);bad[offset]=255;
        State before=rx;assert(rx.receiveAction(bad.data(),bad.size(),10000001)<0);
        assert(!memcmp(&rx,&before,sizeof(rx)));
    }
    auto duplicate=std::vector<uint8_t>(raw,raw+n);duplicate.insert(duplicate.end(),raw+140,raw+184);
    assert(!parseAction(duplicate.data(),duplicate.size(),a));
    auto conflict=std::vector<uint8_t>(raw,raw+n);
    conflict[77]=3; // Same opclass encoding in both TLVs must agree.
    for(unsigned i=0;i<16;++i) {
        conflict[82+2*i]=channels[i]; conflict[83+2*i]=channels[i] ? (channels[i]<=14 ? 81 : 128) : 0;
    }
    conflict[86]=44;assert(!parseAction(conflict.data(),conflict.size(),a));
    for(size_t length=0;length<n;++length) parseAction(raw,length,a);
    uint32_t random=0x9010;
    for(unsigned i=0;i<100000;++i) {
        random=random*1664525+1013904223;auto bad=std::vector<uint8_t>(raw,raw+n);
        bad[random%n]^=uint8_t(random>>24);parseAction(bad.data(),bad.size(),a);
    }
    puts("PASS captured iPhone PSF: standalone opclass schedule, zero padding, peer election, malformed input and 100000 mutations.");
}
static void applePaddingElectionTests()
{
    State tx,rx; tx.init(remote,1000000); rx.init(local,1000000);
    tx.metric=522;tx.masterMetric=522;tx.selfCounter=393;tx.masterCounter=393;
    rx.metric=510;rx.selfCounter=3214;
    uint8_t frame[256];size_t n=buildAction(tx,frame,sizeof(frame),1000000);
    for(unsigned padding:{1U,2U,8U}) {
        std::vector<uint8_t> f(frame,frame+n);f.resize(n+padding,0);
        Advertisement a{};assert(validActionEnvelope(f.data(),f.size()));
        assert(parseAction(f.data(),f.size(),a));
        assert(rx.receiveAction(f.data(),f.size(),1000000)>=0&&rx.synchronized);
        assert(rx.masterMetric==522); // higher metric wins despite older counter
        f.back()=1;assert(!parseAction(f.data(),f.size(),a));
        assert(!validActionEnvelope(f.data(),f.size()));
    }
    rx.metric=600;rx.elect(1000000);assert(!rx.synchronized);
}
// The iPhone's ADDBA Request captured as MTKAWDLRejectedAction on hardware:
// TID 0, immediate policy, A-MSDU, 64-frame buffer. It must be declined with
// the same token, parameters and timeout, addressed back from our AWDL MAC.
static void blockAckTests()
{
    const uint8_t mac[6]={0xda,0xbb,0xfc,0x42,0x0e,0x2c},phone[6]={0xd2,0xa6,0x5b,0x84,0x1b,0xf5};
    const uint8_t request[33]={0xd0,0,0x30,0,0xda,0xbb,0xfc,0x42,0x0e,0x2c,0xd2,0xa6,0x5b,0x84,0x1b,0xf5,
        0,0x25,0,0xff,0x94,0x73,0x10,0x3c,3,0,0x3f,0x03,0x10,0,0,0x30,0};
    State s; s.init(mac,1000000); s.frameCounter=7;
    uint8_t out[64]; assert(blockAckAction(request,sizeof(request),mac));
    assert(buildAddbaDecline(s,request,sizeof(request),out,sizeof(out))==33&&s.frameCounter==8);
    const uint8_t expected[33]={0xd0,0,0,0,0xd2,0xa6,0x5b,0x84,0x1b,0xf5,0xda,0xbb,0xfc,0x42,0x0e,0x2c,
        0,0x25,0,0xff,0x94,0x73,0x70,0,3,1,0x3f,37,0,0x03,0x10,0,0};
    assert(!memcmp(out,expected,sizeof(expected))&&!memcmp(out+4,phone,6));
    // Not ours, protected, truncated, or not a request: no response.
    std::vector<uint8_t> bad(request,request+33);
    bad[4]^=1; assert(!blockAckAction(bad.data(),33,mac)); bad[4]^=1;
    bad[1]=0x40; assert(!blockAckAction(bad.data(),33,mac)); bad[1]=0;
    bad[16]^=1; assert(!blockAckAction(bad.data(),33,mac)); bad[16]^=1;
    memcpy(bad.data()+10,mac,6); assert(!blockAckAction(bad.data(),33,mac)); memcpy(bad.data()+10,phone,6);
    assert(!buildAddbaDecline(s,request,32,out,sizeof(out)));
    assert(!buildAddbaDecline(s,request,33,out,32));
    bad[25]=delba; assert(blockAckAction(bad.data(),33,mac)&&!buildAddbaDecline(s,bad.data(),33,out,sizeof(out)));
    bad[25]=addbaRequest; bad[24]=127; assert(!blockAckAction(bad.data(),33,mac));
    assert(s.frameCounter==8);
}
int main()
{
    blockAckTests();
    aggregateTests(); capturedIphoneTests(); scheduleTests(); roleTests(); nativeClockTests(); templateTests(); nativeTemplateTests(); nativePeriodTests(); applePaddingElectionTests();
    State tx,rx; tx.init(remote,1000000); rx.init(local,1000000); tx.metric=10;
    for(unsigned i=0;i<16;++i) tx.sequence.channels[i]=i%2 ? 44 : 6;
    tx.ownSequence=tx.sequence; tx.elect(1024000);
    uint8_t frame[2048]; size_t n=buildAction(tx,frame,sizeof(frame),1024000);
    Advertisement a={}; assert(n && parseAction(frame,n,a));
    assert(a.period==16 && a.presence==4 && a.metric==10 && a.sequence.channels[1]==44);
    // Truncation at every byte, including complete TLV prefixes, never reads
    // outside the supplied buffer. Invalid frames cannot mutate state.
    for(size_t len=0;len<n;++len) {
        std::vector<uint8_t> shortFrame(frame,frame+len);
        Advertisement parsed={}; parseAction(shortFrame.data(),len,parsed);
    }
    for(unsigned type=0;type<256;++type) {
        std::vector<uint8_t> bad(frame,frame+n); bad[41]=255; bad[42]=255; bad[40]=type;
        State before=rx; assert(rx.receiveAction(bad.data(),bad.size(),1024000)==-1);
        assert(!memcmp(&before,&rx,sizeof(rx)));
    }
    for(unsigned bit: {1U,2U,4U,0x40U,0x80U}) {
        auto bad=std::vector<uint8_t>(frame,frame+n); bad[1]=bit;
        assert(!parseAction(bad.data(),bad.size(),a));
    }
    {
        auto bad=std::vector<uint8_t>(frame,frame+n); bad[22]|=1;
        assert(!parseAction(bad.data(),bad.size(),a));
        bad.assign(frame,frame+n); bad[43+27]=0;
        assert(!parseAction(bad.data(),bad.size(),a));
        bad.assign(frame,frame+n); bad[43+33+2]=1;
        assert(!parseAction(bad.data(),bad.size(),a));
        bad.assign(frame,frame+n); bad[43+33+4]=0;
        assert(!parseAction(bad.data(),bad.size(),a));
    }
    assert(rx.receiveAction(frame,n,1024000)==0 && rx.synchronized);
    assert(rx.counter(1024000)==tx.counter(1024000));
    assert(rx.untilNext(1024000)>=tx.untilNext(1024000) && rx.untilNext(1024000)-tx.untilNext(1024000)<1024);
    assert(rx.channel(1080000)==tx.channel(1080000));
    assert(!rx.peers[0].channel(1024000+peerLifetimeUs+1));
    assert(rx.acceptData(rx.peers[0],123));
    // Expiry resets replay history even if a fresh advertisement arrives
    // before a separate timer tick has purged the old peer entry.
    assert(rx.receiveAction(frame,n,1024000+peerLifetimeUs+1)==0);
    assert(rx.acceptData(rx.peers[0],123));
    rx.elect(1024000+2*peerLifetimeUs+2);
    assert(!rx.synchronized && rx.find(remote)==-1);
    assert(rx.sequence.channels[1]==6 && rx.masterMetric==rx.metric && rx.distance==0);
    assert(tx.setPresence(1,1024000)); size_t shortPresence=buildAction(tx,frame,sizeof(frame),1024000);
    assert(parseAction(frame,shortPresence,a) && a.presence==1);
    assert(tx.setPresence(4,1024000)); n=buildAction(tx,frame,sizeof(frame),1024000);
    // Unknown TLVs are skipped, but a malformed trailing TLV rejects the
    // entire advertisement rather than applying only the synchronization TLV.
    std::vector<uint8_t> extended(frame,frame+n);
    extended.insert(extended.end(),{250,2,0,1,2}); assert(parseAction(extended.data(),extended.size(),a));
    extended.pop_back(); assert(!parseAction(extended.data(),extended.size(),a));
    tx.baseCounter=65532; tx.epoch=1000000;
    assert(tx.counter(1065536)==0 && tx.channel(1065536)==6);
    Reservation grant={}; grant.request(3,44,1000000);
    assert(!grant.canTransmit(44,1001000));
    assert(!grant.grant(2,44,50,1002000)); assert(!grant.grant(3,6,50,1002000));
    assert(grant.grant(3,44,50,1002000)); assert(grant.canTransmit(44,1003000));
    assert(!grant.canTransmit(6,1003000) && !grant.canTransmit(44,1051000));
    grant.cancel(); assert(!grant.canTransmit(44,1003000));
    grant.request(4,6,2000000); assert(!grant.grant(4,6,50,2000000+rocResponseTimeoutUs));
    grant.request(5,6,2000000); assert(!grant.grant(5,6,0,2001000));
    Peer p={}; assert(rx.acceptData(p,65535)); assert(rx.acceptData(p,0));
    assert(!rx.acceptData(p,65535)); assert(rx.acceptData(p,65534)); assert(!rx.acceptData(p,65534));
    assert(rx.acceptData(p,100)); assert(!rx.acceptData(p,0));
    // Round trip actual Ethernet/IPv6 payloads and both 802.11 header sizes.
    for(size_t size=54;size<=1514;++size) {
        uint8_t eth[1514]={},wire[1546]={},decoded[1514]={};
        memcpy(eth,local,6); memcpy(eth+6,remote,6); eth[12]=0x86; eth[13]=0xdd;
        for(size_t i=14;i<size;++i) eth[i]=uint8_t(i*7); eth[14]=0x60;
        eth[18]=uint8_t((size-54)>>8); eth[19]=uint8_t(size-54);
        size_t encoded=encodeData(wire,sizeof(wire),eth,size,42); uint16_t seq=0;
        assert(encoded==size+26);
        assert(decodeData(decoded,sizeof(decoded),wire,encoded,seq)==size && seq==42);
        assert(!memcmp(eth,decoded,size));
        // Unsupported headers must never reach the IPv6 stack as a partial
        // or incorrectly decapsulated datagram, and must leave outputs alone.
        for(unsigned bit: {1U,2U,4U,0x40U,0x80U}) {
            wire[1]=bit; assert(!decodeData(decoded,sizeof(decoded),wire,encoded,seq));
        }
        wire[1]=0; wire[22]|=1;
        assert(!decodeData(decoded,sizeof(decoded),wire,encoded,seq)); wire[22]&=0xf0;
        wire[0]=9; assert(!decodeData(decoded,sizeof(decoded),wire,encoded,seq)); wire[0]=8;
        memmove(wire+26,wire+24,encoded-24); wire[24]=wire[25]=0; wire[0]=0x88;
        assert(decodeData(decoded,sizeof(decoded),wire,encoded+2,seq)==size);
        assert(!memcmp(eth,decoded,size));
        wire[24]=0x80; assert(!decodeData(decoded,sizeof(decoded),wire,encoded+2,seq)); wire[24]=0;
        wire[25]=1; assert(!decodeData(decoded,sizeof(decoded),wire,encoded+2,seq)); wire[25]=0;
        wire[42]=0x40; assert(!decodeData(decoded,sizeof(decoded),wire,encoded+2,seq)); wire[42]=0x60;
        wire[46]^=1; assert(!decodeData(decoded,sizeof(decoded),wire,encoded+2,seq)); wire[46]^=1;
        if(size==1514) for(size_t len=40;len<encoded+2;++len) {
            std::vector<uint8_t> truncated(wire,wire+len);
            assert(!decodeData(decoded,sizeof(decoded),truncated.data(),len,seq));
        }
        for(size_t len=0;len<40;++len) {
            std::vector<uint8_t> shortWire(wire,wire+len);
            assert(!decodeData(decoded,sizeof(decoded),shortWire.data(),len,seq));
        }
        wire[1]=0x40; assert(!decodeData(decoded,sizeof(decoded),wire,encoded+2,seq));
    }
    // Deterministic fuzzing of TLV sizes and header bits under ASan/UBSan.
    uint32_t random=0x7921;
    for(unsigned i=0;i<100000;++i) {
        random=random*1664525+1013904223;
        std::vector<uint8_t> packet(frame,frame+n);
        packet[random%n]^=uint8_t(random>>24);
        parseAction(packet.data(),packet.size(),a);
        uint8_t out[1514]; uint16_t seq;
        decodeData(out,sizeof(out),packet.data(),packet.size(),seq);
    }
    puts("AWDL protocol: bounded templates, presence/wrap, roles, malformed frames, election/expiry, replay, grants, IPv6 and A-MSDU round trips across every MTU size, hardware padding and 200000 mutations passed");
}
