#!/usr/bin/env python3
"""Check grant planning across idle gaps without announcing idle windows."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
include = Path(sys.argv[1]) if len(sys.argv)>1 else root/'include'
code = r'''
#include <cassert>
#include <cstdio>
#include <AWDL/AwdlTimeline.hpp>
int main(){
 using namespace mtkawdl;
 const uint8_t mac[]={2,1,2,3,4,5};State s;s.init(mac,1000000);
 memset(s.sequence.channels,0,16);
 s.sequence.channels[0]=s.sequence.channels[2]=6;s.sequence.channels[4]=44;
 s.ownSequence=s.sequence;
 RadioPlan p{};assert(planRadio(s,1000000,p));
 assert(p.window.counter==0&&p.window.channel==6&&p.durationMs==201);
 Reservation grant{};grant.request(1,6,1000000);assert(grant.grant(1,6,p.durationMs,1000000));
 Window idle{},second{},different{};
 assert(currentWindow(s,1000000+4*awUs,idle)&&idle.channel==0);
 assert(currentWindow(s,1000000+8*awUs,second)&&grantCoversWindow(second,grant,second.start));
 assert(!grantCoversWindow(idle,grant,idle.start));
 assert(currentWindow(s,1000000+16*awUs,different)&&different.channel==44);
 assert(!grantCoversWindow(different,grant,different.start));
 // Trailing idle does not enlarge a grant; neither does a different channel.
 s.sequence.channels[2]=0;assert(planRadio(s,1000000,p));assert(p.durationMs==70);
 s.sequence.channels[1]=44;s.sequence.channels[2]=6;
 assert(planRadio(s,1000000,p)&&p.durationMs==70);
 // A long idle horizon must never exceed the existing firmware dwell cap.
 s.sequence.channels[1]=0;s.presence=s.ownPresence=64;
 s.sequence.step=s.ownSequence.step=63;
 assert(planRadio(s,1000000,p)&&p.durationMs<=maxRocMs);
 puts("PASS radio plan: one grant spans same-channel windows across idle, never grants idle/different-channel AWs, preserves dwell cap.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-awdl-idle-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(code)
    subprocess.run(['clang++','-std=c++14','-fsanitize=address,undefined',
                    '-I'+str(include),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
