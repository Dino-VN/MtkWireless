/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MtkWireless: 802.11d Country element bookkeeping, header-only so the host
 * tests (scripts/test-wifi-country.py) compile it without net80211.
 */
#ifndef _NET80211_IEEE80211_11D_H_
#define _NET80211_IEEE80211_11D_H_

#include <stdint.h>

#define IEEE80211_11D_SLOTS 8

/* BSSes per announced country; each BSS counted once (moves on change). */
struct ieee80211_11d_seen {
    uint8_t cc[2];
    uint16_t n;
};

/* Accept a Country element (ie points at the element ID) whose country
 * string starts with two ISO letters; "X?" and "ZZ" name no country. */
static inline int
ieee80211_11d_country(const uint8_t *ie, uint8_t cc[2])
{
    uint8_t a, b;
    if (ie == 0 || ie[1] < 2)
        return 0;
    a = ie[2]; b = ie[3];
    if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 'a' + 'A');
    if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z' || a == 'X' ||
        (a == 'Z' && b == 'Z'))
        return 0;
    cc[0] = a; cc[1] = b;
    return 1;
}

/* node_cc is the BSS's last announced country (0 = none yet). */
static inline void
ieee80211_11d_note(struct ieee80211_11d_seen *seen, uint8_t node_cc[2],
                   const uint8_t cc[2])
{
    int i, slot = -1, low = 0;
    if (node_cc[0] == cc[0] && node_cc[1] == cc[1])
        return;
    for (i = 0; i < IEEE80211_11D_SLOTS; i++) {
        if (node_cc[0] && seen[i].n && seen[i].cc[0] == node_cc[0] &&
            seen[i].cc[1] == node_cc[1])
            seen[i].n--;
    }
    for (i = 0; i < IEEE80211_11D_SLOTS; i++) {
        if (seen[i].cc[0] == cc[0] && seen[i].cc[1] == cc[1]) {
            slot = i;
            break;
        }
        if (seen[i].n < seen[low].n)
            low = i;
    }
    if (slot < 0) {
        slot = low;
        seen[slot].cc[0] = cc[0];
        seen[slot].cc[1] = cc[1];
        seen[slot].n = 0;
    }
    if (seen[slot].n < 0xffff)
        seen[slot].n++;
    node_cc[0] = cc[0];
    node_cc[1] = cc[1];
}

#endif
