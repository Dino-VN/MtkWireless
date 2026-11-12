#!/usr/bin/env python3
"""Country code: 802.11d Country element bookkeeping (ieee80211_11d.h),
selection and Apple80211 locale mapping (CountryLocale.hpp)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "ieee80211_11d.h"
#include "CountryLocale.hpp"

static int count(const ieee80211_11d_seen *seen, const char *cc)
{
    for (int i = 0; i < IEEE80211_11D_SLOTS; ++i)
        if (seen[i].cc[0] == cc[0] && seen[i].cc[1] == cc[1])
            return seen[i].n;
    return 0;
}

int main()
{
    using namespace mtkcc;
    Source s = kFirmware;

    // --- Country element parsing (element ID 7, length, "IT ", triplets).
    uint8_t cc[2];
    const uint8_t it[] = {7, 6, 'I', 'T', ' ', 36, 4, 23};
    assert(ieee80211_11d_country(it, cc) && cc[0] == 'I' && cc[1] == 'T');
    const uint8_t lower[] = {7, 3, 'd', 'e', 'O'};
    assert(ieee80211_11d_country(lower, cc) && cc[0] == 'D' && cc[1] == 'E');
    const uint8_t xx[] = {7, 3, 'X', 'X', ' '}, zz[] = {7, 3, 'Z', 'Z', ' '};
    const uint8_t digits[] = {7, 3, '0', '0', ' '}, shortie[] = {7, 1, 'I'};
    assert(!ieee80211_11d_country(xx, cc) && !ieee80211_11d_country(zz, cc));
    assert(!ieee80211_11d_country(digits, cc) && !ieee80211_11d_country(shortie, cc));
    assert(!ieee80211_11d_country(nullptr, cc));

    // --- Tally: each BSS counted once, moves when its country changes.
    ieee80211_11d_seen seen[IEEE80211_11D_SLOTS] = {};
    uint8_t bss[7][2] = {};
    const uint8_t IT[2] = {'I', 'T'}, FR[2] = {'F', 'R'};
    for (int i = 0; i < 7; ++i) {
        ieee80211_11d_note(seen, bss[i], IT);
        ieee80211_11d_note(seen, bss[i], IT);   // repeated beacons
    }
    ieee80211_11d_note(seen, bss[6], FR);       // one BSS changes country
    assert(count(seen, "IT") == 6 && count(seen, "FR") == 1);
    // A full table evicts the least seen country.
    uint8_t other[9][2] = {};
    for (int i = 0; i < 9; ++i) {
        const uint8_t c[2] = {'A', uint8_t('A' + i)};
        ieee80211_11d_note(seen, other[i], c);
    }
    assert(count(seen, "IT") == 6);

    char seenCC[IEEE80211_11D_SLOTS][2];
    unsigned seenN[IEEE80211_11D_SLOTS];
    for (int i = 0; i < IEEE80211_11D_SLOTS; ++i) {
        memcpy(seenCC[i], seen[i].cc, 2);
        seenN[i] = seen[i].n;
    }
    char major[3];
    majority(seenCC, seenN, IEEE80211_11D_SLOTS, major);
    assert(!strcmp(major, "IT"));
    unsigned none[2] = {0, 0};
    majority(seenCC, none, 2, major);
    assert(!major[0]);

    // --- Selection: override > named firmware > geolocation > 802.11d AP
    // > 802.11d majority > firmware.
    // Hardware 2026-11-10: firmware "00", no geolocation, APs announce IT.
    assert(!strcmp(select("", "00", "", "IT", "IT", &s), "IT") && s == kBss11d);
    assert(!strcmp(select("", "00", "", "", "IT", &s), "IT") && s == kScan11d);
    assert(!strcmp(select("", "00", "", "FR", "IT", &s), "FR") && s == kBss11d);
    assert(!strcmp(select("", "00", "DE", "IT", "IT", &s), "DE") && s == kGeo);
    assert(!strcmp(select("", "ZZ", "IT", "", "", &s), "IT") && s == kGeo);
    assert(!strcmp(select("", "DE", "IT", "IT", "IT", &s), "DE") && s == kFirmware);
    assert(!strcmp(select("US", "DE", "IT", "IT", "IT", &s), "US") && s == kOverride);
    assert(!strcmp(select("IT", "00", "", "", "", &s), "IT") && s == kOverride);
    // Nothing usable: keep the firmware value (unchanged behaviour).
    assert(!strcmp(select("", "00", "", "", "", &s), "00") && s == kFirmware);
    assert(!strcmp(select("", "00", "ZZ", "XX", "", &s), "00") && s == kFirmware);
    assert(!strcmp(select("", "00", "I", "", "", &s), "00") && s == kFirmware);
    assert(select("", "00", "IT", "", "", nullptr));
    assert(!strcmp(sourceName(kBss11d), "802.11d-ap") && !strcmp(sourceName(kScan11d), "802.11d-scan"));

    // --- Locale: ETSI for the EU/EEA, CH and GB, case-insensitive.
    for (const char *c : {"IT", "it", "DE", "FR", "ES", "NL", "SE", "NO", "CH", "GB", "EU"})
        assert(locale(c) == 2);
    assert(locale("JP") == 3);
    assert(locale("KR") == 4);
    for (const char *c : {"US", "CA", "00", "ZZ", "", "X1", "CN", "AU", "BR"})
        assert(locale(c) == 1);
    assert(unknown(nullptr) && unknown("") && unknown("0") && !unknown("IT"));

    puts("PASS: 802.11d Country element and per-BSS tally, override > firmware > geo > AP > scan > firmware, ETSI/JAPAN/KOREA locale.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-wifi-country-') as tmp:
    src = Path(tmp) / 'test.cpp'
    src.write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined',
                    '-I', str(root / 'kext/Wifi/vendor/AirportMtkWlan'),
                    '-I', str(root / 'kext/Wifi/vendor/mtk80211/openbsd/net80211'),
                    str(src), '-o', str(Path(tmp) / 'test')], check=True)
    subprocess.run([str(Path(tmp) / 'test')], check=True)
