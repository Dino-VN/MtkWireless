# 6 GHz (Wi-Fi 6E)

Status: implemented behind boot-arg `amtk_6ghz=1`, off by default, never run
on hardware. MT7922 and MT7921K have 6 GHz; plain MT7921 does not.

## Why off by default

Every 6 GHz BSS requires WPA3-SAE or OWE, with PMF (Wi-Fi Alliance 6 GHz
security rules: no WPA2-PSK, WEP, open or transition modes), and SAE on
6 GHz must use hash-to-element (H2E) only. The SAE here
(`ieee80211_sae.c`, `rust/mtk-wlan/src/sae`) does hunting-and-pecking only,
no OWE. Advertising 6 GHz would let airportd pick 6 GHz BSSes of multi-band
SSIDs and every such join would fail. Turn the default on once H2E (or OWE)
joins work: flip the default in `mwx_6ghz_enabled()` (`MtkMwx.cpp`).

With `amtk_6ghz=1` and a chip that reports 6 GHz, the driver scans 6 GHz,
reports 6 GHz BSSes and channels to macOS, and can associate with a 6 GHz BSS
whose security net80211 supports.

## Capability (Linux mt7921)

`MCU_CE_CMD_GET_NIC_CAPAB` TLV `MT_NIC_CAP_6G`: first byte nonzero = 6 GHz
(`mt7921_mcu_get_nic_capability`). `sc_capa.has_6ghz` also needs 802.11ax
(6 GHz is HE only; `amtk_he=0` turns 6 GHz off too).

Linux additionally loads CLC (country location control) records from the RAM
firmware image (`FW_TYPE_CLC` region, `MCU_CE_CMD_SET_CLC`) to enable 6 GHz
per country and pick the LPI/SP/VLP power table. This driver sends no CLC,
like Linux with `disable_clc=1` and mt7921u, where 6 GHz still works with the
firmware's default power tables. CLC is a follow-up.

## Channels in net80211

net80211 indexed `ic_channels[]` by IEEE channel number; 6 GHz numbers 1-233
collide with 2.4/5 GHz. Now:

- `ic_channels[IEEE80211_CHAN_6GHZ_BASE + n]` (base 256,
  `IEEE80211_CHAN_MAX` 511) is 6 GHz channel n, flag `IEEE80211_CHAN_6GHZ`
  (`0x40000000`, not `CHAN_5GHZ`), `CHAN_OFDM`, `CHAN_PASSIVE` unless PSC.
  2.4/5 GHz indices are unchanged.
- `ieee80211_chan2ieee()` still returns the IEEE number;
  `ieee80211_chan2idx()` returns the index. Every `ic_chan_avail/active/scan`
  bitmap and `ic_wcl_scan_channels` (now 512 bits) uses the index.
- `ieee80211_ieee2mhz/mhz2ieee`: 5955 + 5n MHz (channel 2 = 5935).
- `ieee80211_chan2mode()`: 11A (OFDM rates); 11AX mode includes 6 GHz.
- Beacons/probe responses: `rxi_chan` (now `uint16_t`) is an index; a DS
  Parameter Set on 6 GHz names a 6 GHz channel; the HE 6 GHz Operation
  primary channel wins (duplicate beacons are accepted on the other 20 MHz
  channels). HE 6 GHz Band Capabilities and Operation are kept in the node.
- Assoc request on 6 GHz: no HT/VHT Capabilities; HE Capabilities plus HE
  6 GHz Band Capabilities (`ic_he_6ghz_capa`, from the 5 GHz HT/VHT A-MPDU
  limits as Linux does, SM PS disabled).
- Width: `ieee80211_he_6ghz_negotiate_chw()` from the HE 6 GHz Operation
  (20/40/80; 160 only if both sides advertise it, which we do not).

## Firmware (Linux mt76)

| What | 6 GHz value |
|---|---|
| RXD channel | 181 + (n - 1) / 4 (`mt792x_get_status_freq_info`) |
| `START_HW_SCAN` channel band | 3 |
| `SET_CHAN_DOMAIN` | `bw_6g` = 3, `n_6ch`, hw_value = channel number, flags 0 |
| `CHANNEL_SWITCH` `channel_band` | 2 |
| BSS_INFO basic | phymode A/AN/AC/AX_5G, `phymode_ext` = AX_6G |
| BSS_INFO RLM `band` | 3 (`NL80211_BAND_6GHZ`) |
| STA_REC | HE without HT; `STA_REC_HE_6G` (0x17) = peer's HE 6 GHz capa |
| WTBL | HT/VHT TLVs with A-MPDU factor/density from HE 6 GHz capa |
| `SET_RATE_TX_POWER` band 3 | all 6 GHz 20/40/80/160 centers, 12 dBm |

TX power: 12 dBm on every rate, the FCC LPI client limit at 20 MHz
(-1 dBm/MHz), so it holds at every width. LPI is indoor-only; SP and VLP
are not used.

Scanning: the firmware takes at most 64 channels. A full scan covers 2.4
(14) + 5 (27) + the 15 PSC channels of 6 GHz (5, 21, ..., 229). WCL asks
for other 6 GHz channels (RNR-discovered APs) by channel list. No
RNR/short-SSID probing in firmware (`SCAN_FUNC_RNR_SCAN`) yet.

## macOS (IO80211Family / AppleBCMWLANCoreMac 26.7.1)

- `AppleChannelSpec_t`: bits 14-15 band, 0 = 2.4, 3 = 5, **1 = 6 GHz**
  (`ChanSpecConvToApple80211Channel`, `AppleBCMWLANCore::is6GChanSpec`,
  `WCLDeviceConfiguration::fill6gPscChanSpec` builds `0x5000 | ch`); bits
  11-13 width (2 = 20 MHz). Used for scan results (`BeaconMetadata`),
  `WCL_CHANNELS_INFO`, `CHANNELS_INFO`, `WCL_REASSOC`.
- `apple80211_channel.flags`: 6 GHz is `0x2000` (`APPLE80211_C_FLAG_6GHZ`)
  instead of 2GHZ/5GHZ; scan requests use it, so `decodeScan` maps those
  entries to bit 256 + n.
- `WCL_CHANNELS_INFO` byte 0xc85 `supports6GHz`
  (`is6ESupportedInCurrentCountry`): 1 when a 6 GHz channel is active.
- `CHANNELS_INFO` per-channel bitmap = Broadcom `WL_CHAN_*`: 6 GHz sets
  `BAND_6G` (bit 9) and LPI (bit 12; `handleChanInfoTimer` reads LPI power
  for it, VLP for bit 10).
- `CARD_CAPABILITIES`: Broadcom feature 0x5d sets byte 0xf bit 6 of
  `apple80211_capability_data` (= `capabilities[11]` bit 6), which
  `WCLDeviceConfiguration::is6GSupported` reads.

## AWDL stays off 6 GHz

- 6 GHz channels are not in `awdlChannelAllowed` (2.4/5 GHz only), and
  `setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE` rejects entries flagged 6 GHz.
- Associated on 6 GHz, `awdlInfraChannel()` is 0xff: no AWDL channel equals
  it, so every AWDL ROC takes the PM=1 handshake (or, with
  `amtk_awdl_offchan=0`, AWDL does not run). Without this a 6 GHz AP on
  channel 149 would look like the 5 GHz AWDL channel 149.

## Not done

- SAE H2E and OWE, so 6 GHz stays opt-in (SAE hunting-and-pecking exists).
- CLC per-country enablement and SP/VLP power; firmware RNR scan.
- 160 MHz (not offered on 5 GHz either), 6 GHz-only roaming policy.

## Tests

`scripts/test-wifi-capabilities.py` checks the 6 GHz capability bit,
chanspecs, `CHANNELS_INFO` bitmap and 6 GHz scan-request decoding.
