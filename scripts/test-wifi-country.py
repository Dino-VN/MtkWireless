#!/usr/bin/env python3
"""Country code selection and Apple80211 locale mapping (CountryLocale.hpp)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "CountryLocale.hpp"

int main()
{
    using namespace mtkcc;
    Source s = kFirmware;

    // Hardware 2026-11-10: firmware "00", airportd geolocation "IT".
    assert(!strcmp(select("", "00", "IT", &s), "IT") && s == kGeo);
    // The old rule only fell back for "ZZ"; that still works.
    assert(!strcmp(select("", "ZZ", "IT", &s), "IT") && s == kGeo);
    // A firmware that names a country wins over geolocation.
    assert(!strcmp(select("", "DE", "IT", &s), "DE") && s == kFirmware);
    // The boot-arg wins over everything.
    assert(!strcmp(select("IT", "00", "", &s), "IT") && s == kOverride);
    assert(!strcmp(select("US", "DE", "IT", &s), "US") && s == kOverride);
    // No usable geolocation: keep the firmware value (unchanged behaviour).
    assert(!strcmp(select("", "00", "", &s), "00") && s == kFirmware);
    assert(!strcmp(select("", "00", "ZZ", &s), "00") && s == kFirmware);
    assert(!strcmp(select("", "00", "X0", &s), "00") && s == kFirmware);
    assert(!strcmp(select("", "00", "I", &s), "00") && s == kFirmware);
    assert(select("", "00", "IT", nullptr));

    // Locale: ETSI for the EU/EEA, CH and GB, case-insensitive.
    for (const char *cc : {"IT", "it", "DE", "FR", "ES", "NL", "SE", "NO", "CH", "GB", "EU"})
        assert(locale(cc) == 2);
    assert(locale("JP") == 3);
    assert(locale("KR") == 4);
    // Unknown and unlisted keep FCC (the previous hard-coded value).
    for (const char *cc : {"US", "CA", "00", "ZZ", "", "X1", "CN", "AU", "BR"})
        assert(locale(cc) == 1);
    assert(unknown(nullptr) && unknown("") && unknown("0") && !unknown("IT"));

    puts("PASS: firmware 00/ZZ falls back to geolocation, boot-arg and named firmware win, ETSI/JAPAN/KOREA mapping, unknown stays FCC.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-wifi-country-') as tmp:
    src = Path(tmp) / 'test.cpp'
    src.write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined',
                    '-I', str(root / 'kext/Wifi/vendor/AirportMtkWlan'),
                    str(src), '-o', str(Path(tmp) / 'test')], check=True)
    subprocess.run([str(Path(tmp) / 'test')], check=True)
