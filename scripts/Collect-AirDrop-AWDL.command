#!/bin/zsh
# Capture one bulk AirDrop attempt without restarting services or changing settings.
set -u
umask 077
repo="${0:A:h:h}"
duration="${1:-180}"
if [[ "$duration" != <-> || "$duration" -lt 30 || "$duration" -gt 600 ]]; then
  print -u2 'Usage: Collect-AirDrop-AWDL.command [seconds: 30-600; default 180]'
  exit 2
fi
mkdir -p "$repo/diagnostics" || exit 1
out=$(mktemp -d "$repo/diagnostics/airdrop-awdl-$(date +%Y%m%d-%H%M%S).XXXXXX") || exit 1
exec > >(tee -a "$out/collector.log") 2>&1
print "Output: $out"
print 'Captures contain private network traffic and device information; keep them local.'
print "Enter your Mac password to capture AWDL packets for $duration seconds."
sudo -v || exit 1
# Driver traces use clock_get_uptime / mach_absolute_time, not wall time or
# the AW counter. Keep bracketed wall/monotonic observations around each
# sample. The recorded implementation identifies the host clock; do not infer
# wall time by simply adding driver timestamps to the machine's boot date.
clock_sample() {
  python3 -I - "$1" >> "$out/clocks.jsonl" <<'PY'
import json
import sys
import time
wall_before = time.time_ns()
monotonic = time.monotonic_ns()
wall_after = time.time_ns()
info = time.get_clock_info("monotonic")
print(json.dumps({"sample": sys.argv[1], "wall_before_ns": wall_before,
                  "monotonic_ns": monotonic, "wall_after_ns": wall_after,
                  "monotonic_implementation": info.implementation,
                  "monotonic_resolution_s": info.resolution}))
PY
}
snapshot() {
  clock_sample "$1-before"
  ioreg -a -r -c AirportMtkWlan > "$out/wlan-$1.plist"
  ioreg -a -r -c AirportMtkAwdlInterface > "$out/awdl-$1.plist"
  ioreg -a -r -n IOResources > "$out/resources-$1.plist"
  # Keep interface names and MTUs: an AirDrop candidate can be Ethernet,
  # not AWDL. This records state without capturing traffic on other interfaces.
  ifconfig -a > "$out/interfaces-$1.txt"
  ifconfig awdl0 > "$out/interface-$1.txt"
  ndp -an > "$out/neighbors-$1.txt"
  clock_sample "$1-after"
}
snapshot before
kmutil showloaded --list-only > "$out/kexts.txt" 2>&1
started=$(date '+%Y-%m-%d %H:%M:%S')
# Pre-create as the invoking user. Root opens these existing files without
# changing ownership, so the private (0600) capture remains readable afterwards.
: > "$out/awdl.pcap"
: > "$out/tcpdump.log"
: > "$out/capture-exit-status.txt"
# The root shell owns and reaps its capture child, including on interruption.
sudo -n /bin/sh -c '
  status_file="$1/capture-exit-status.txt"
  /usr/sbin/tcpdump -i awdl0 -n -s 0 -U -w "$1/awdl.pcap" "ip6" > "$1/tcpdump.log" 2>&1 &
  capture=$!
  timer=
  stop_capture() {
    if [ -n "$timer" ]; then
      kill "$timer" 2>/dev/null
      wait "$timer" 2>/dev/null
    fi
    kill -INT "$capture" 2>/dev/null
    wait "$capture" 2>/dev/null
    printf "%s\n" "$?" > "$status_file"
  }
  trap stop_capture EXIT
  trap "exit 130" INT TERM
  sleep "$2" &
  timer=$!
  wait "$timer"
' awdl-capture "$out" "$duration" &
worker=$!
trap 'kill -TERM "$worker" 2>/dev/null' INT TERM
print '\nCapture started. Send ONE large file from Mac to iPhone now.'
print 'If progress stalls, leave that attempt running until its error or capture end.'
print 'Do not start another transfer, change settings, or restart a service during capture.'
print "Leave this window open; capture stops automatically after $duration seconds."
sample=0
while kill -0 "$worker" 2>/dev/null; do
  label=$(printf 'sample-%04d' "$sample")
  clock_sample "$label-before"
  ioreg -a -r -c AirportMtkAwdlInterface > "$out/$label.plist"
  ioreg -a -r -c AirportMtkWlan > "$out/wlan-$label.plist"
  clock_sample "$label-after"
  sample=$((sample+1))
  sleep 1
done
wait "$worker"
capture_status=$?
ended=$(date '+%Y-%m-%d %H:%M:%S')
snapshot after
for process_name in sharingd rapportd mDNSResponder bluetoothd; do
  /usr/bin/log show --start "$started" --end "$ended" --style compact --info --debug \
    --predicate "process == \"$process_name\"" > "$out/$process_name.log" 2>&1
done
capture_exit=unknown
if [[ -s "$out/capture-exit-status.txt" ]]; then
  read -r capture_exit < "$out/capture-exit-status.txt"
fi
print "started=$started\nended=$ended\nduration=$duration\nwrapper_status=$capture_status\ncapture_status=$capture_exit" > "$out/window.txt"
if [[ "$capture_status" != 0 || "$capture_exit" != 0 ]]; then
  print "WARNING: capture did not finish cleanly (wrapper=$capture_status, tcpdump=$capture_exit). See tcpdump.log."
fi
print "\nDone. Report the filename, approximate size, direction, and where progress stopped.\n$out"
