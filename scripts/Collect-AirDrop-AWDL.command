#!/bin/zsh
# Capture one AirDrop attempt without restarting Bluetooth or changing settings.
set -u
repo="${0:A:h:h}"
out="$repo/diagnostics/airdrop-awdl-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out" || exit 1
exec > >(tee -a "$out/collector.log") 2>&1
print "Output: $out"
print 'Enter your Mac password to capture AWDL packets for 90 seconds.'
sudo -v || exit 1
snapshot() {
  ioreg -a -r -c AirportMtkWlan > "$out/wlan-$1.plist"
  ioreg -a -r -c AirportMtkAwdlInterface > "$out/awdl-$1.plist"
  ioreg -a -r -n IOResources > "$out/resources-$1.plist"
  ifconfig awdl0 > "$out/interface-$1.txt"
  ndp -an > "$out/neighbors-$1.txt"
}
snapshot before
kmutil showloaded --list-only > "$out/kexts.txt" 2>&1
started=$(date '+%Y-%m-%d %H:%M:%S')
# The root shell owns and reaps its capture child, including on interruption.
sudo -n /bin/sh -c '
  # Newer AirDrop sessions also use UDP (QUIC), not only TCP and mDNS.
  /usr/sbin/tcpdump -i awdl0 -n -s 0 -U -w "$1/awdl.pcap" "ip6" > "$1/tcpdump.log" 2>&1 &
  capture=$!
  stop_capture() { kill -INT "$capture" 2>/dev/null; wait "$capture" 2>/dev/null; }
  trap stop_capture EXIT
  trap "exit 130" INT TERM
  sleep 90 &
  wait $!
' awdl-capture "$out" &
worker=$!
trap 'kill -TERM "$worker" 2>/dev/null' INT TERM
print '\nCapture started. Now send one photo from iPhone to Mac.'
print 'After it fails, try sending one small file from Mac to iPhone.'
print 'Leave this window open; capture stops automatically after 90 seconds.'
while kill -0 "$worker" 2>/dev/null; do
  ioreg -a -r -c AirportMtkAwdlInterface > "$out/sample-$(date +%H%M%S).plist"
  sleep 1
done
wait "$worker"
ended=$(date '+%Y-%m-%d %H:%M:%S')
snapshot after
for process_name in sharingd rapportd mDNSResponder bluetoothd; do
  /usr/bin/log show --start "$started" --end "$ended" --style compact --info --debug \
    --predicate "process == \"$process_name\"" > "$out/$process_name.log" 2>&1
done
print "started=$started\nended=$ended" > "$out/window.txt"
print "\nDone. Tell Codex: đã chạy xong AWDL collector.\n$out"
