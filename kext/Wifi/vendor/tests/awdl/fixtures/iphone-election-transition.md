# iPhone election transition fixture

Captured from the driver's `MTKAWDLRejectedAction` property on 2026-10-23
while AirDrop endpoint resolution failed. The frame is a real iOS 26 MIF.
Only the 802.11/vendor header and TLVs 4, 18 and 24 are retained; other TLVs,
including service identities, are removed. MACs are replaced consistently
with the protocol tests' local/remote addresses.

The synchronization TLV names the transmitting peer, while election v2 names
this Mac as root and immediate synchronization source. The old parser rejects
the entire frame because these master addresses differ. The test requires
the channel schedule to remain usable while the election loop check keeps
the Mac from following its own follower.

The retained OWL iOS 26 reference parses synchronization parameters and
election-v2 root identity independently in `src/rx.c`.
