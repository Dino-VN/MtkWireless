// SPDX-License-Identifier: GPL-2.0-or-later
// MtkWireless: country code selection and Apple80211 locale mapping.
//
// Hardware 2026-11-10: the firmware reports "00" (world), so the geolocated
// country airportd sends through setCOUNTRY_CODE was never used (only "ZZ"
// fell back to it), and getLOCALE always said FCC. macOS then planned AWDL on
// the US data channel 149 while the AP was on 44 (Italy): every AWDL window
// needed an off-channel ROC and dense presence was never allowed. With
// amtk_cc=IT, AWDL moved to 44, TX errors fell to 0.5% and AirDrop ran
// ~0.9 MB/s Mac->iPhone and ~5 MB/s iPhone->Mac.
#pragma once

namespace mtkcc {

enum Source { kOverride = 1, kGeo = 2, kFirmware = 3, kBss11d = 4, kScan11d = 5 };

inline const char *sourceName(Source s)
{
    switch (s) {
    case kOverride: return "boot-arg";
    case kGeo: return "geolocation";
    case kBss11d: return "802.11d-ap";
    case kScan11d: return "802.11d-scan";
    default: return "firmware";
    }
}

inline char upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c; }

// "", "00", "ZZ" and "X?" carry no country.
inline bool unknown(const char *cc)
{
    if (!cc || !cc[0] || !cc[1]) return true;
    char a = upper(cc[0]), b = upper(cc[1]);
    return (a == '0' && b == '0') || (a == 'Z' && b == 'Z') || a == 'X';
}

// user override > firmware (when it names a country) > geolocation from
// airportd > 802.11d of the associated AP > 802.11d majority of scanned
// BSSes > firmware. Hardware 2026-11-10: airportd counted "IT" in 7 APs'
// Country elements but never called setCOUNTRY_CODE (IO80211 logs "Dynamic
// country code not supported on this device"), so the driver must read the
// Country element itself, as Broadcom firmware does (EnabledAutoCountry).
inline const char *select(const char *override_cc, const char *firmware_cc,
                          const char *geo_cc, const char *bss11d_cc,
                          const char *scan11d_cc, Source *source)
{
    Source s = kFirmware;
    const char *cc = firmware_cc;
    if (override_cc && override_cc[0]) { s = kOverride; cc = override_cc; }
    else if (!unknown(firmware_cc)) {}
    else if (!unknown(geo_cc)) { s = kGeo; cc = geo_cc; }
    else if (!unknown(bss11d_cc)) { s = kBss11d; cc = bss11d_cc; }
    else if (!unknown(scan11d_cc)) { s = kScan11d; cc = scan11d_cc; }
    if (source) *source = s;
    return cc;
}

// The country announced by the most BSSes (first one on a tie); out gets
// "" when none has a count.
inline void majority(const char (*cc)[2], const unsigned *count, unsigned slots, char out[3])
{
    unsigned best = 0;
    out[0] = out[1] = out[2] = 0;
    for (unsigned i = 0; i < slots; ++i) {
        if (count[i] > best) {
            best = count[i];
            out[0] = cc[i][0];
            out[1] = cc[i][1];
        }
    }
}

inline bool same(const char *cc, const char *code)
{
    return upper(cc[0]) == code[0] && upper(cc[1]) == code[1];
}

// Apple80211 locale values (apple80211_var.h): FCC 1, ETSI 2, JAPAN 3,
// KOREA 4. Anything not listed keeps the previous behaviour (FCC).
inline int locale(const char *cc)
{
    static const char *const etsi[] = {
        "AT", "BE", "BG", "HR", "CY", "CZ", "DK", "EE", "FI", "FR", "DE", "GR",
        "HU", "IE", "IT", "LV", "LT", "LU", "MT", "NL", "PL", "PT", "RO", "SK",
        "SI", "ES", "SE", "IS", "LI", "NO", "CH", "GB", "EU",
    };
    if (unknown(cc)) return 1;
    for (const char *code : etsi)
        if (same(cc, code)) return 2;
    if (same(cc, "JP")) return 3;
    if (same(cc, "KR")) return 4;
    return 1;
}

} // namespace mtkcc
