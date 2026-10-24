#!/usr/bin/env python3
"""Check the WCL roaming wire formats (WCL.hpp) against the 26.7.1 layouts."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include <cassert>
#include <cstdio>
#include "WCL.hpp"
int main() {
    using namespace AirportWCL;
    uint8_t buf[0xbc] = {};
    buf[0] = 36; buf[1] = 0xd0; buf[2] = 6; buf[3] = 0x10;   // chanspecs 36, 6
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(buf + 0x64, mac, 6);
    buf[0x90] = 1; buf[0x94] = 2; buf[0xbb] = 4; memcpy(buf + 0x9b, "home", 4);
    ReassocRequest r;
    assert(decodeReassoc(buf, sizeof(buf), r));
    assert(r.bssidCount == 1 && r.channelCount == 2 && r.ssidLength == 4);
    assert(!memcmp(r.bssid, mac, 6) && !memcmp(r.ssid, "home", 4));
    assert(r.channels[36 / 8] & (1 << (36 % 8)) && r.channels[0] & (1 << 6));
    assert(!decodeReassoc(buf, 0xbb, r));                    // short buffer
    buf[0x64] = 1; assert(!decodeReassoc(buf, sizeof(buf), r));   // multicast
    buf[0x64] = 2; buf[0x94] = 51; assert(!decodeReassoc(buf, sizeof(buf), r));
    buf[0x94] = 0; buf[0x90] = 8; assert(!decodeReassoc(buf, sizeof(buf), r));
    buf[0x90] = 0; buf[0xbb] = 33; assert(!decodeReassoc(buf, sizeof(buf), r));
    buf[0xbb] = 0; memset(buf + 0x64, 0, 6);
    assert(decodeReassoc(buf, sizeof(buf), r) && r.bssidCount == 0);
    RoamPeer from = {}, to = {};
    from.rssi = -72; from.channel = 6; from.bssid[0] = 1;
    to.rssi = -55; to.channel = 149; to.bssid[0] = 6; to.phyMode = 0x100;
    RoamStatus st;
    makeRoamStatus(0, RoamReasonLowRssi, 100, 90, from, to, st);
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&st);
    assert(st.endMs == 100 && p[0x20] == uint8_t(-72) && p[0x2c] == 149);
    assert(p[0x58] == 1 && p[0x5e] == 6 && p[0x55] == 1 && p[4] == 0x01 && p[7] == 0xe0);
    puts("PASS: WCL_REASSOC decode, ROAM_START/PREP/REASSOC/ROAM_DONE layouts");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-wifi-roam-') as tmp:
    src = Path(tmp) / 'test.cpp'
    src.write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-fsanitize=address,undefined',
                    '-I', str(root / 'kext/include'), '-I', str(root / 'kext/Wifi'),
                    '-I', str(root / 'kext/Wifi/vendor/AirportMtkWlan/WCL'),
                    '-Wl,-U,_mtk_wlan_scan_ssids_parse',
                    str(src), '-o', str(Path(tmp) / 'test')], check=True)
    subprocess.run([str(Path(tmp) / 'test')], check=True)
