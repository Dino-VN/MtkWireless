#!/bin/zsh
# One-shot AirDrop / Bluetooth / AWDL evidence collector.
# Asks for the admin password once, records an HCI trace, awdl0 mDNS packets,
# AWDL driver counters and daemon logs around one manual AirDrop attempt in
# each direction, then turns HCI tracing off again. It does not install or
# unload kexts, change network settings or reboot.

repo="${0:A:h:h}"
stamp=$(date +%Y%m%d-%H%M%S)
out="$repo/diagnostics/airdrop-bt-$stamp"
mkdir -p "$out" || exit 1
exec > >(tee -a "$out/collector.log") 2>&1

hci_pref=/Library/Preferences/com.apple.MobileBluetooth.debug
hci_dir=/private/var/root/Library/Logs/Bluetooth
max_wait=300

print "Output: $out"
print 'Enter your Mac password (needed for the Bluetooth HCI trace and packet capture).'
sudo -v || { print 'sudo failed; nothing was changed.'; exit 1; }
# Keep the sudo timestamp fresh while the collector runs.
( while true; do sudo -n true 2>/dev/null; sleep 30; done ) &
keepalive=$!

cleanup() {
  [[ -n $sampler ]] && kill $sampler 2>/dev/null
  [[ -n $capture ]] && sudo kill -INT $capture 2>/dev/null
  sudo defaults delete $hci_pref HCITraces 2>/dev/null
  kill $keepalive 2>/dev/null
}
trap cleanup EXIT INT TERM

snapshot() {
  local label=$1
  { ioreg -a -r -c AirportMtkWlan; ioreg -a -r -c MtkWirelessWifi; } > "$out/wlan-$label.plist" 2>&1
  ioreg -l -w0 -r -n IOResources | grep -A12 '"MtkWirelessBT"' > "$out/bt-patcher-$label.txt" 2>&1
  ioreg -l -w0 | grep -B2 -A40 'MtkBtLog' > "$out/bt-loader-$label.txt" 2>&1
  system_profiler SPBluetoothDataType > "$out/bluetooth-$label.txt" 2>&1
  ifconfig awdl0 > "$out/awdl0-$label.txt" 2>&1
  netstat -ibn -I awdl0 >> "$out/awdl0-$label.txt" 2>&1
  ndp -an > "$out/ndp-$label.txt" 2>&1
}

print '\n[1/6] Static information'
sw_vers > "$out/system.txt"; uname -a >> "$out/system.txt"
kmutil showloaded --list-only 2>/dev/null | grep -i -E 'mtk|lilu|bluetool|airport|bt' > "$out/kexts.txt"
if ! grep -q com.dinovn.MtkWirelessBT "$out/kexts.txt"; then
  print 'WARNING: MtkWirelessBT.kext is not loaded; the BLE feature-mask shim cannot run.'
  print '         Check Kernel -> Add in config.plist (Lilu first) and reboot.'
fi
nvram -p 2>/dev/null | grep -i bluetooth > "$out/nvram-bluetooth.txt"
defaults read com.apple.sharingd > "$out/sharingd-prefs.txt" 2>&1
snapshot before

print '[2/6] Enabling HCI trace and restarting bluetoothd'
sudo defaults write $hci_pref HCITraces -dict HCIFileTraces -bool true
sudo pkill bluetoothd
for i in {1..30}; do
  sleep 1
  pgrep -x bluetoothd >/dev/null && system_profiler SPBluetoothDataType 2>/dev/null | grep -q 'State: On' && break
done
sleep 5
print "bluetoothd is $(pgrep -x bluetoothd >/dev/null && print running || print NOT running)"

print '[3/6] Starting awdl0 mDNS capture and AWDL counter sampler'
sudo /usr/sbin/tcpdump -i awdl0 -n -s 0 -U -w "$out/awdl0-mdns.pcap" 'udp port 5353 or icmp6' > "$out/tcpdump.log" 2>&1 &
sleep 1
capture=$(pgrep -n -x tcpdump)
( zmodload zsh/datetime; while true; do
    print "@ $EPOCHREALTIME $(strftime %H:%M:%S $EPOCHSECONDS)"
    ioreg -r -c AirportMtkWlan -l -w0 | grep '"MTKAWDL' | grep -v -E 'Payload|RejectedAction|Template' | sed -E 's/^[ |]+//'
    sleep 0.5
  done ) > "$out/awdl-counters.txt" 2>&1 &
sampler=$!
started=$(date '+%Y-%m-%d %H:%M:%S')

print '\n================  YOUR TURN  ================'
print '  A. On this Mac: AirDrop a small file to the iPhone.'
print '     Wait until it fails (about 15-60 s).'
print '  B. On the iPhone: open the AirDrop share sheet and'
print '     keep it open about 30 s. Note whether the Mac appears.'
print "  Then press Enter here (auto-continues after $max_wait s)."
print '============================================='
read -t $max_wait 'reply?> ' || print '(timed out, continuing)'
print -n 'Did the Mac appear in the iPhone share sheet? [y/n/skip] '
read -t 60 seen || seen=skip
print "mac_seen_on_iphone=$seen" > "$out/user-observation.txt"
ended=$(date '+%Y-%m-%d %H:%M:%S')

print '\n[4/6] Stopping capture'
kill $sampler 2>/dev/null; sampler=
sudo kill -INT $capture 2>/dev/null; capture=
sleep 2
snapshot after

print '[5/6] Collecting HCI trace and logs'
sudo cp "$hci_dir/bluetoothd-hci-latest.pklg" "$out/hci.pklg" 2>"$out/hci-copy.err" ||
  sudo sh -c "ls -t $hci_dir/*.pklg | head -1 | xargs -I{} cp {} '$out/hci.pklg'"
/usr/bin/log show --start "$started" --end "$ended" --style compact --info --debug \
  --predicate 'process == "bluetoothd"' > "$out/log-bluetoothd.txt" 2>&1
/usr/bin/log show --start "$started" --end "$ended" --style compact --info --debug \
  --predicate 'process == "sharingd" OR process == "rapportd"' > "$out/log-sharingd-rapportd.txt" 2>&1
/usr/bin/log show --start "$started" --end "$ended" --style compact --info --debug \
  --predicate 'process == "mDNSResponder"' > "$out/log-mdnsresponder.txt" 2>&1
/usr/bin/log show --start "$started" --end "$ended" --style compact --info --debug \
  --predicate 'process == "kernel" AND (eventMessage CONTAINS "awdl" OR eventMessage CONTAINS "AWDL" OR eventMessage CONTAINS[c] "mtk" OR eventMessage CONTAINS[c] "bluetooth")' \
  > "$out/log-kernel.txt" 2>&1
print "started=$started\nended=$ended" > "$out/window.txt"

print '[6/6] Turning HCI tracing off'
sudo defaults delete $hci_pref HCITraces 2>/dev/null
sudo chown -R "$USER" "$out"

print "\nDone. Folder: $out"
ls -la "$out"
print '\nTell Claude: "đã chạy xong collector". You can close this window.'
