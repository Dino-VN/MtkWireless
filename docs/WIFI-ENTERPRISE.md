# WPA2-Enterprise EAPOL transport

The 2026-10-31 MT7922 campus report on MtkWireless 1.0.6 shows successful
Open System authentication and association on channel 116, followed by four
calls to `ieee80211_eapol_key_input` and a join timeout. An open guest network
on the same channel works. The supplied logs do not contain EAPOL packet
bytes, so the packet type and exact EAP authentication stage are unproven.

## Driver defect and fix

Enterprise RX already duplicates EAPOL Ethernet frames into the BSD input
list before passing the originals to net80211. A call to the key-input
routine therefore does not prove an EAP request was consumed without being
forwarded; that routine logs before checking the packet type.

The BSD transport nevertheless remained closed until RSN port authorization:
`AirportMtkWlan::setLinkStatus` starts the output thread only on link-up,
while net80211 defers link-up until the four-way key handshake succeeds.
Enterprise authentication needs eapolclient to exchange EAP packets first,
then deliver the PMK that lets the key handshake finish. The down-link
path also clears the BSD interface's UP/RUNNING flags.

The fixed driver enables BSD input/output for Enterprise joins once
net80211 enters RUN after the HAL finishes configuring the BSS and peer.
The early Association Response notification is too soon. A committed-state
notification opens this transport and closes it when leaving RUN; explicit
leave, abort and shutdown close it immediately. Normal authorized link-up
continues to own the transport after the key handshake.

This does not authorize the port or complete the WCL join. The existing
RX and TX gates admit only EtherType 0x888e while `ni_port_valid` is clear;
ordinary IPv4 and IPv6 remain blocked. PMK installation and the net80211
key handshake retain their existing behavior. No EAP credential parsing or
payload logging was added. `mtkctl log` records `EAPOL transport=1 state=4`
at setup and `EAPOL transport=0` at teardown.

## Offline validation

`python3 scripts/test-wifi-eapol.py` runs the production transport helper,
link-status handler, RX enqueue function and TX port guard under ASan/UBSan.
It checks pre-authorization EAPOL delivery, ordinary data rejection,
authorization, reauthentication, reset, abort and absent/stopped interfaces.
`scripts/test-wifi-capabilities.py` covers directed join probes, explicit
scan priority, SSID bounds and BSSID binding after the previous join leaves.
The usual scan, RX, roaming and kernel build checks also apply.

## Hardware retest

Use the test build with bundle version 1.0.7, keeping the original boot args.

1. Join the open guest network as a control, then the Enterprise network.
   Confirm EAPOL transport opens after RUN, authentication finishes, keys
   are installed, and ordinary network traffic works.
2. Capture EAPOL on the reported BSD interface during the Enterprise attempt:
   `sudo tcpdump -i en2 -s 0 -w eapol.pcap 'ether proto 0x888e'`.
   Request/response packets and any EAP failure distinguish transport
   problems from later authentication failures. Retain airportd/eapolclient
   logs and `mtkctl log` alongside the capture if the attempt still fails.
3. Join the iPhone hotspot. The join scan must show one directed target,
   discover the BSS and advance to AUTH. Retest the home WPA2-PSK network.
4. Cancel or fail an Enterprise attempt and switch to another network;
   confirm the next join and an AirDrop transfer still work.

The intermittent channel-116 Open System authentication timeout is still
unexplained. A TX ACK alone does not establish why the AP did not send an
authentication response. No retry, PHY or security policy was changed for
that symptom. Successful Enterprise and hotspot joins remain pending
hardware verification; these fixes address defects established in the code.
