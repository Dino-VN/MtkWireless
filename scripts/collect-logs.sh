#!/bin/bash
# Self-contained, read-only MtkWireless support bundle collector (macOS Bash 3.2).
# Can run from a downloaded file or via bash -c, like scripts/install.sh.
# Never restarts daemons, enables tracing, scans, mounts EFI, or changes settings.
set -uo pipefail
umask 077
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C

LAST=1h
ARCHIVE=1
OUTPUT_BASE=""
NOTES=""
OUT=""
ZIP_FILE=""
ACTIVE_PID=""
ROOT_OK=0
OWNER_UID=""
OWNER_GID=""
STARTED=$(/bin/date '+%Y-%m-%d %H:%M:%S%z')

usage() {
    printf '%s\n' \
        'Usage: collect-logs.sh [--last 1h] [--no-logarchive] [--output DIR] [--notes TEXT]' \
        '  --last WINDOW      Recent daemon/kernel logs: 1m through 7d (default 1h).' \
        '  --no-logarchive    Skip the native .logarchive (otherwise capped at 512 MB).' \
        '  --output DIR       Create a new bundle in DIR (default: your Desktop).' \
        '  --notes TEXT       Describe the failure, time, devices and transfer direction.' \
        'Kernel/kext loading logs cover the current boot; crash/HCI/Lilu files cover 3 days.' \
        'No uploads, no mtkctl, no Xcode/Python dependencies, no system-setting changes.'
}
while [ $# -gt 0 ]; do
    case "$1" in
        --last|--output|--notes)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            case "$1" in
                --last) LAST=$2 ;;
                --output) OUTPUT_BASE=$2 ;;
                --notes) NOTES=$2 ;;
            esac
            shift 2 ;;
        --no-logarchive) ARCHIVE=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
if [[ ! "$LAST" =~ ^([1-9][0-9]{0,4})(m|h|d)$ ]]; then
    usage >&2; exit 2
fi
amount=$((10#${BASH_REMATCH[1]}))
case "${BASH_REMATCH[2]}" in
    m) minutes=$amount ;; h) minutes=$((amount * 60)) ;; d) minutes=$((amount * 1440)) ;;
esac
[ "$minutes" -le 10080 ] || { usage >&2; exit 2; }
[ "$(/usr/bin/uname -s)" = Darwin ] || { printf 'This collector requires macOS.\n' >&2; exit 1; }

# sudo must not put the bundle on /var/root/Desktop. Resolve the invoking user,
# or the console user for a direct root invocation; never guess /Users/$name.
OWNER_USER=$(/usr/bin/id -un)
OWNER_HOME=${HOME:-}
if [ "$EUID" -eq 0 ]; then
    OWNER_USER=${SUDO_USER:-$(/usr/bin/stat -f '%Su' /dev/console 2>/dev/null)}
    case "$OWNER_USER" in
        ''|root|loginwindow|_mbsetupuser)
            [ -n "$OUTPUT_BASE" ] || { printf 'No logged-in user; specify --output DIR.\n' >&2; exit 1; }
            OWNER_USER=root; OWNER_HOME=${HOME:-/var/root} ;;
        *)
            home_record=$(/usr/bin/dscl /Search -read "/Users/$OWNER_USER" NFSHomeDirectory 2>/dev/null) || home_record=""
            OWNER_HOME=${home_record#NFSHomeDirectory: }
            [ -n "$OWNER_HOME" ] && [ -d "$OWNER_HOME" ] || {
                printf 'Cannot resolve the home directory for %s.\n' "$OWNER_USER" >&2; exit 1;
            } ;;
    esac
fi
OWNER_UID=$(/usr/bin/id -u "$OWNER_USER") || exit 1
OWNER_GID=$(/usr/bin/id -g "$OWNER_USER") || exit 1
[ -n "$OUTPUT_BASE" ] || OUTPUT_BASE="$OWNER_HOME/Desktop"
/bin/mkdir -p "$OUTPUT_BASE" || exit 1
OUT=$(/usr/bin/mktemp -d "$OUTPUT_BASE/MtkWireless-Logs-$(/bin/date +%Y%m%d-%H%M%S).XXXXXX") || exit 1
# Canonical paths also work when --output is relative and when bash -c has no file.
OUT=$(cd "$OUT" && pwd -P) || exit 1
ZIP_FILE="$OUT.zip"
/bin/mkdir -p "$OUT"/{system,hardware,ioreg,network,services,preferences,logs,bluetooth,crashes,kexts,efi} || exit 1
printf 'started\tseconds\texit_status\toutput\tcommand\n' > "$OUT/commands.tsv"

info() {
    printf '%s\n' "$*"
    printf '%s\n' "$*" >> "$OUT/collector.log"
}
as_root() {
    if [ "$EUID" -eq 0 ]; then
        "$@"
    elif [ "$ROOT_OK" -eq 1 ]; then
        /usr/bin/sudo -n -v && /usr/bin/sudo -n "$@"
    else
        # A declined password still yields a useful, explicitly reduced bundle.
        "$@"
    fi
}
process_tree() {
    local parent=$1 child
    while IFS= read -r child; do
        process_tree "$child"
    done < <(/usr/bin/pgrep -P "$parent" 2>/dev/null)
    printf '%s\n' "$parent"
}
stop_active() {
    local targets target
    # Save descendants before TERM: killing a wrapper can reparent children,
    # making a second pgrep of the original PID miss a hung sudo/tool process.
    targets=$(process_tree "$ACTIVE_PID")
    for target in $targets; do
        kill -TERM "$target" 2>/dev/null || as_root /bin/kill -TERM "$target" 2>/dev/null || true
    done
    /bin/sleep 1
    for target in $targets; do
        kill -KILL "$target" 2>/dev/null || as_root /bin/kill -KILL "$target" 2>/dev/null || true
    done
}
permissions() {
    [ -n "$OUT" ] && [ -d "$OUT" ] || return 0
    /bin/chmod -R go-rwx "$OUT" 2>/dev/null || true
    if [ "$EUID" -eq 0 ]; then
        /usr/sbin/chown -R "$OWNER_UID:$OWNER_GID" "$OUT" 2>/dev/null || true
        [ ! -f "$ZIP_FILE" ] || /usr/sbin/chown "$OWNER_UID:$OWNER_GID" "$ZIP_FILE" 2>/dev/null || true
    fi
}
interrupted() {
    trap '' INT TERM HUP
    if [ -n "$ACTIVE_PID" ]; then
        stop_active
        wait "$ACTIVE_PID" 2>/dev/null || true
    fi
    info "Interrupted. Partial files are preserved in: $OUT"
    exit 130
}
trap permissions EXIT
trap interrupted INT TERM HUP

# Every external collection is bounded, and failures do not discard other evidence.
# Keep stderr separate so .plist/.spx and binary files remain parseable.
capture() {
    local relative=$1 budget=$2; shift 2
    local began=$SECONDS timestamp result timed_out=0 quoted
    timestamp=$(/bin/date '+%Y-%m-%dT%H:%M:%S%z')
    printf -v quoted '%q ' "$@"
    /bin/mkdir -p "$(/usr/bin/dirname "$OUT/$relative")"
    info "  $relative"
    "$@" > "$OUT/$relative" 2> "$OUT/$relative.stderr.txt" &
    ACTIVE_PID=$!
    while kill -0 "$ACTIVE_PID" 2>/dev/null; do
        if [ "$((SECONDS - began))" -ge "$budget" ]; then
            timed_out=1
            stop_active
            break
        fi
        /bin/sleep 0.2
    done
    wait "$ACTIVE_PID"; result=$?
    ACTIVE_PID=""
    if [ "$timed_out" -eq 1 ]; then
        result=124
        printf '\n[collector] Timed out after %s seconds; output may be partial.\n' "$budget" >> "$OUT/$relative.stderr.txt"
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$timestamp" "$((SECONDS - began))" "$result" "$relative" "$quoted" >> "$OUT/commands.tsv"
    [ "$result" -eq 0 ] || info "    NOTE: exit $result; see $relative.stderr.txt (collection continues)."
    return 0
}

info "Output: $OUT"
info 'This bundle contains SSIDs, MAC/IP addresses, serial numbers, device names and private logs. Share only with someone you trust.'
info 'Read-only: no daemon restarts, no Wi-Fi/BT/AWDL changes, no EFI mounts, no uploads.'
info 'Run this right after a failure. If the problem is still occurring, keep the current state during collection.'
if [ "$EUID" -eq 0 ]; then
    ROOT_OK=1
else
    info 'Enter your Mac password for full kernel, Bluetooth and kernel collection diagnostics.'
    if [ -t 0 ]; then
        /usr/bin/sudo -v && ROOT_OK=1
    else
        /usr/bin/sudo -n -v && ROOT_OK=1
    fi
fi
[ "$ROOT_OK" -eq 1 ] || info 'WARNING: No administrator access; some diagnostics will be missing or denied.'
printf 'started=%s\nrecent_window=%s\nadmin_access=%s\ninvoking_user=%s\nnotes=%s\n' \
    "$STARTED" "$LAST" "$ROOT_OK" "$OWNER_USER" "$NOTES" > "$OUT/session.txt"

snapshot() {
    local label=$1 class
    for class in AirportMtkWlan MtkPCIEDeviceWrapper AirportMtkAwdlInterface MtkWirelessBTLoader; do
        capture "ioreg/$class-$label.plist" 30 /usr/sbin/ioreg -a -r -c "$class"
    done
    # IOResources holds the BT patcher, mSBC, RPA, LE readiness and verifier counters.
    capture "ioreg/resources-$label.plist" 30 /usr/sbin/ioreg -a -r -n IOResources -d 1
    capture "network/interfaces-$label.txt" 20 /sbin/ifconfig -a
    capture "network/interface-counters-$label.txt" 20 /usr/sbin/netstat -ibn
    capture "network/awdl0-$label.txt" 20 /sbin/ifconfig awdl0
    capture "network/awdl0-counters-$label.txt" 20 /usr/sbin/netstat -ibn -I awdl0
}
info '[1/8] Driver state before other collection'
snapshot before

info '[2/8] macOS, card hardware, USB/PCI and power state'
capture system/sw-vers.txt 15 /usr/bin/sw_vers
capture system/uname.txt 15 /usr/bin/uname -a
capture system/uptime.txt 15 /usr/bin/uptime
capture system/sysctl.txt 20 /usr/sbin/sysctl kern.osversion kern.version kern.boottime kern.bootargs hw.model hw.machine hw.memsize machdep.cpu.brand_string
capture system/sip.txt 20 /usr/bin/csrutil status
capture system/authenticated-root.txt 20 /usr/bin/csrutil authenticated-root status
capture system/extensions.txt 30 /usr/bin/systemextensionsctl list
capture system/disks.txt 30 /usr/sbin/diskutil list
capture system/mounts.txt 20 /sbin/mount
capture system/power.txt 30 /usr/bin/pmset -g
capture system/power-assertions.txt 30 /usr/bin/pmset -g assertions
capture system/sleep-wake.log 60 /usr/bin/pmset -g log
capture hardware/wifi.txt 75 /usr/sbin/system_profiler -detailLevel full -timeout 60 SPAirPortDataType
capture hardware/bluetooth.txt 75 /usr/sbin/system_profiler -detailLevel full -timeout 60 SPBluetoothDataType
capture hardware/system-profiler.spx 150 /usr/sbin/system_profiler -xml -detailLevel full -timeout 120 \
    SPHardwareDataType SPSoftwareDataType SPPCIDataType SPUSBDataType SPAirPortDataType \
    SPBluetoothDataType SPNetworkDataType SPAudioDataType SPPowerDataType
# The full service hierarchy is important after wake: USB device clients can be
# present while interface 0 has no user client. Driver-only snapshots miss that.
capture ioreg/ioservice.plist 60 /usr/sbin/ioreg -a -p IOService
capture ioreg/usb.plist 45 /usr/sbin/ioreg -a -p IOUSB
capture ioreg/pci.plist 30 /usr/sbin/ioreg -a -r -c IOPCIDevice
capture ioreg/device-tree.plist 45 /usr/sbin/ioreg -a -p IODeviceTree
for key in boot-args csr-active-config bluetoothInternalControllerInfo bluetoothExternalDongleFailed \
    4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path \
    4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version; do
    capture "system/nvram-${key//:/_}.txt" 15 as_root /usr/sbin/nvram "$key"
done

info '[3/8] Loaded kexts, dependencies, kernel collection and OpenCore metadata'
capture kexts/loaded.txt 45 as_root /usr/bin/kmutil showloaded --show-kernel --arch-info
capture kexts/loaded-list.txt 45 as_root /usr/bin/kmutil showloaded --list-only
capture kexts/kextstat.txt 30 as_root /usr/sbin/kextstat
capture kexts/kernelmanager-state.txt 45 as_root /usr/bin/kmutil dumpstate
capture kexts/kernel-collections.txt 90 as_root /usr/bin/kmutil inspect --allow-missing-collections \
    --show-collection-metadata --show-kext-uuids --show-kext-load-addresses
for identifier in com.dinovn.MtkWirelessWifi com.dinovn.MtkWirelessBT as.vit9696.Lilu as.acidanthera.BlueToolFixup; do
    capture "kexts/path-$identifier.txt" 30 as_root /usr/bin/kmutil path-for-identifier --bundle-id "$identifier"
done
# Only already-mounted EFIs are read. Never collect the whole config.plist:
# PlatformInfo/NVRAM can contain identifiers or secrets unrelated to diagnosis.
bundle_index=0
collect_bundles() {
    local directory=$1 scope=$2 bundle name
    [ -d "$directory" ] || return 0
    for bundle in "$directory"/*.kext; do
        [ -d "$bundle" ] || continue
        name=${bundle##*/}
        case "$name" in
            *Mtk*|*MTK*|Lilu.kext|BlueToolFixup.kext|*itlwm*|*Itlwm*|IntelBluetooth*|Brcm*) ;;
            *) continue ;;
        esac
        bundle_index=$((bundle_index + 1))
        printf '%s\t%s\n' "$bundle_index" "$bundle" >> "$OUT/kexts/source-paths.tsv"
        capture "kexts/bundle-$bundle_index/$name-info.plist" 20 as_root /usr/bin/plutil -convert xml1 -o - "$bundle/Contents/Info.plist"
        case "$name" in
            MtkWirelessWifi.kext|MtkWirelessBT.kext|Lilu.kext|BlueToolFixup.kext)
                capture "kexts/bundle-$bundle_index/copy.txt" 60 as_root /usr/bin/ditto \
                    "$bundle" "$OUT/kexts/bundle-$bundle_index/$name" ;;
        esac
    done
}
collect_bundles /Library/Extensions library
collect_bundles /Library/StagedExtensions/Library/Extensions staged
collect_bundles /System/Library/Extensions system
efi_index=0
for oc in /Volumes/*/EFI/OC; do
    [ -d "$oc" ] || continue
    efi_index=$((efi_index + 1))
    printf '%s\t%s\n' "$efi_index" "$oc" >> "$OUT/efi/source-paths.tsv"
    for section in Kernel Misc.Security ACPI.Add UEFI.Drivers; do
        capture "efi/efi-$efi_index-$section.plist" 20 as_root /usr/bin/plutil -extract "$section" xml1 -o - "$oc/config.plist"
    done
    collect_bundles "$oc/Kexts" "efi-$efi_index"
done
[ "$efi_index" -ne 0 ] || info '  No mounted OpenCore EFI found; EFI metadata skipped (nothing was mounted).'

info '[4/8] Network, AWDL, DNS, daemon state and relevant preferences'
capture network/hardware-ports.txt 20 /usr/sbin/networksetup -listallhardwareports
capture network/service-order.txt 20 /usr/sbin/networksetup -listnetworkserviceorder
capture network/routes.txt 20 /usr/sbin/netstat -rn
capture network/protocol-counters.txt 20 /usr/sbin/netstat -s
capture network/ipv6-neighbors.txt 20 /usr/sbin/ndp -an
capture network/arp.txt 20 /usr/sbin/arp -an
capture network/nwi.txt 20 /usr/sbin/scutil --nwi
capture network/dns.txt 20 /usr/sbin/scutil --dns
capture network/proxy.txt 20 /usr/sbin/scutil --proxy
[ ! -x /usr/bin/wdutil ] || capture network/wdutil.txt 45 as_root /usr/bin/wdutil info
while IFS= read -r interface; do
    [[ "$interface" =~ ^[a-zA-Z][a-zA-Z0-9]*$ ]] || continue
    capture "network/$interface-power.txt" 20 /usr/sbin/networksetup -getairportpower "$interface"
    capture "network/$interface-network.txt" 20 /usr/sbin/networksetup -getairportnetwork "$interface"
    capture "network/$interface-dhcp.txt" 20 /usr/sbin/ipconfig getpacket "$interface"
    capture "network/$interface-ip-summary.txt" 20 /usr/sbin/ipconfig getsummary "$interface"
done < <(/usr/bin/awk '/^Hardware Port:/ { wifi = /Wi-Fi|AirPort/ } /^Device:/ && wifi { print $2 }' "$OUT/network/hardware-ports.txt")
capture services/processes.txt 20 /bin/ps -axo pid,ppid,uid,stat,lstart,comm
for service in com.apple.bluetoothd com.apple.BlueTool com.apple.airportd com.apple.wifid; do
    capture "services/$service.txt" 20 as_root /bin/launchctl print "system/$service"
done
for service in com.apple.sharingd com.apple.rapportd com.apple.coreservices.useractivityd; do
    capture "services/$service.txt" 20 as_root /bin/launchctl print "gui/$OWNER_UID/$service"
done
# Read selected keys only: Bluetooth's main preference file can hold link keys.
capture preferences/bt-power.txt 15 as_root /usr/bin/defaults read /Library/Preferences/com.apple.Bluetooth ControllerPowerState
capture preferences/hci-tracing.txt 15 as_root /usr/bin/defaults read /Library/Preferences/com.apple.MobileBluetooth.debug HCITraces
capture preferences/airdrop-discovery.txt 15 /usr/bin/defaults read "$OWNER_HOME/Library/Preferences/com.apple.sharingd" DiscoverableMode
capture preferences/airdrop-disabled.txt 15 /usr/bin/defaults read "$OWNER_HOME/Library/Preferences/com.apple.NetworkBrowser" DisableAirDrop
for key in ActivityAdvertisingAllowed ActivityReceivingAllowed; do
    capture "preferences/$key.txt" 15 /usr/bin/defaults read "$OWNER_HOME/Library/Preferences/com.apple.coreservices.useractivityd" "$key"
done

info '[5/8] bluetoothd binary, recent crash/panic reports and existing HCI/Lilu logs'
if [ -f /usr/sbin/bluetoothd ]; then
    capture bluetooth/copy-binary.txt 30 /bin/cp /usr/sbin/bluetoothd "$OUT/bluetooth/bluetoothd"
    capture bluetooth/binary-sha256.txt 30 /usr/bin/shasum -a 256 /usr/sbin/bluetoothd
    capture bluetooth/binary-file.txt 15 /usr/bin/file /usr/sbin/bluetoothd
    capture bluetooth/binary-signature.txt 30 /usr/bin/codesign -d --verbose=4 /usr/sbin/bluetoothd
fi
copy_index=0
copy_list() {
    local list=$1 category=$2 source name
    while IFS= read -r -d '' source; do
        name=${source##*/}
        copy_index=$((copy_index + 1))
        printf '%s\t%s\n' "$copy_index" "$source" >> "$OUT/$category/source-paths.tsv"
        capture "$category/copy-$copy_index.txt" 30 as_root /bin/cp "$source" "$OUT/$category/$copy_index-$name"
    done < "$OUT/$list"
}
crash_index=0
for directory in /Library/Logs/DiagnosticReports "$OWNER_HOME/Library/Logs/DiagnosticReports"; do
    [ -d "$directory" ] || continue
    crash_index=$((crash_index + 1))
    capture "crashes/paths-$crash_index.nul" 30 as_root /usr/bin/find "$directory" -maxdepth 2 -type f -mtime -3 \
        \( -iname 'bluetoothd*' -o -iname 'BlueTool*' -o -iname 'airportd*' -o -iname 'wifid*' \
        -o -iname 'sharingd*' -o -iname 'rapportd*' -o -iname 'mDNSResponder*' -o -iname 'coreaudiod*' \
        -o -iname 'audioaccessoryd*' -o -iname 'Kernel*.ips' -o -iname '*.panic' -o -iname 'Sleep Wake Failure*' \) -print0
    copy_list "crashes/paths-$crash_index.nul" crashes
done
hci_index=0
for directory in /private/var/root/Library/Logs/Bluetooth "$OWNER_HOME/Library/Logs/Bluetooth"; do
    # Root's log directory can be inaccessible before sudo; let find report it.
    hci_index=$((hci_index + 1))
    capture "bluetooth/hci-paths-$hci_index.nul" 30 as_root /usr/bin/find "$directory" -maxdepth 2 -type f -mtime -3 -name '*.pklg' -print0
    copy_list "bluetooth/hci-paths-$hci_index.nul" bluetooth
done
capture logs/file-paths.nul 30 as_root /usr/bin/find /private/var/log -maxdepth 1 -type f -mtime -3 \
    \( -name 'Lilu_*.txt' -o -iname '*bluetooth*.log' -o -name 'wifi.log' -o -name 'system.log' -o -name 'install.log' -o -name 'kernel.log' \) -print0
copy_list logs/file-paths.nul logs

info '[6/8] Unified logs, including kext/kernel loading since boot'
log_show() {
    local relative=$1 window=$2 predicate=$3
    capture "$relative" 180 as_root /usr/bin/log show --last "$window" --style compact --info --debug --predicate "$predicate"
}
log_show logs/bluetoothd.log "$LAST" 'process == "bluetoothd" OR process == "BlueTool"'
log_show logs/wifi.log "$LAST" 'process == "airportd" OR process == "wifid" OR process == "WiFiAgent"'
log_show logs/airdrop-continuity.log "$LAST" 'process == "sharingd" OR process == "rapportd" OR process == "nearbyd" OR process == "useractivityd" OR process == "AirPlayXPCHelper" OR process == "ContinuityCaptureAgent"'
log_show logs/mdns.log "$LAST" 'process == "mDNSResponder"'
log_show logs/bluetooth-audio.log "$LAST" 'process == "coreaudiod" OR process == "audioaccessoryd"'
log_show logs/kernel.log "$LAST" 'process == "kernel"'
log_show logs/driver.log "$LAST" 'process == "kernel" AND (eventMessage CONTAINS[c] "mtk" OR eventMessage CONTAINS[c] "awdl" OR eventMessage CONTAINS[c] "bluetooth" OR eventMessage CONTAINS[c] "lilu" OR eventMessage CONTAINS[c] "bluetool")'
log_show logs/kernel-kext-loading-boot.log boot 'subsystem CONTAINS[c] "KernelManagement" OR process == "kernelmanagerd" OR process == "kextd" OR (process == "kernel" AND (eventMessage CONTAINS[c] "mtk" OR eventMessage CONTAINS[c] "lilu" OR eventMessage CONTAINS[c] "bluetool" OR eventMessage CONTAINS[c] "kext" OR eventMessage CONTAINS[c] "kernel collection"))'
log_show logs/daemon-launches.log "$LAST" 'process == "launchd" AND (eventMessage CONTAINS[c] "bluetooth" OR eventMessage CONTAINS[c] "sharingd" OR eventMessage CONTAINS[c] "rapportd" OR eventMessage CONTAINS[c] "airportd" OR eventMessage CONTAINS[c] "wifid")'
capture logs/kmutil-boot.log 180 as_root /usr/bin/kmutil log show --last boot --style compact --info --debug
if [ "$ARCHIVE" -eq 1 ]; then
    info '  Native .logarchive: up to 512 MB, all processes in the recent window.'
    capture logs/logarchive-collection.txt 300 as_root /usr/bin/log collect --last "$LAST" --size 512m --output "$OUT/logs/system.logarchive"
fi

info '[7/8] Final counters and bundle summary'
snapshot after
{
    printf 'MtkWireless diagnostic bundle\nStarted: %s\nFinished: %s\nRecent logs: %s\nAdmin access: %s\n\n' \
        "$STARTED" "$(/bin/date '+%Y-%m-%d %H:%M:%S%z')" "$LAST" "$ROOT_OK"
    printf 'Notes: %s\n\nLoaded MtkWireless / related kexts (not a root-cause verdict):\n' "$NOTES"
    /usr/bin/grep -iE 'mtk|lilu|bluetool|itlwm|intelbluetooth|brcm' "$OUT/kexts/loaded-list.txt" || true
    printf '\nFailed/unsupported/timed-out commands (0 = success, 124 = timeout):\n'
    /usr/bin/awk -F '\t' 'NR > 1 && $3 != 0 { print $3 "\t" $4 }' "$OUT/commands.tsv"
    printf '\nStart with hardware/{wifi,bluetooth}.txt, ioreg/*-before.plist,\n'
    printf 'kexts/loaded.txt, logs/kernel-kext-loading-boot.log and the daemon logs.\n'
    printf 'commands.tsv records every command, duration and exit status; stderr is separate.\n'
    printf 'Missing IORegistry services can return an empty array with exit 0; that is NOT proof a driver loaded.\n'
    printf 'No live HCI trace or packet capture was enabled; only existing .pklg files were copied.\n'
    printf 'Disk bluetoothd is the OS binary, not a dump of in-memory page patches.\n'
    printf 'macOS may redact data or may not have persisted debug events; collection cannot recover them.\n'
    printf 'EFI data is included only when already mounted. Full config.plist, keychain and Bluetooth link-key preferences were not copied.\n'
    printf 'WARNING: logs, IORegistry, crashes and .logarchive contain private identifiers and may contain private activity.\n'
    printf 'Share privately with a trusted debugger; this script never uploads anything.\n'
} > "$OUT/README.txt"

info '[8/8] ZIP on Desktop / selected output directory'
# Reserve a new file; never overwrite an existing archive.
if ! (set -o noclobber; : > "$ZIP_FILE") 2>/dev/null; then
    info "ERROR: Archive already exists. Uncompressed files: $OUT"; exit 1
fi
capture zip-result.txt 300 /usr/bin/ditto -c -k --keepParent "$OUT" "$ZIP_FILE"
# A ZIP header alone is not success: test the whole archive before announcing it.
capture zip-check.txt 180 /usr/bin/unzip -tq "$ZIP_FILE"
zip_status=$(/usr/bin/awk -F '\t' '$4 == "zip-result.txt" { print $3 }' "$OUT/commands.tsv")
check_status=$(/usr/bin/awk -F '\t' '$4 == "zip-check.txt" { print $3 }' "$OUT/commands.tsv")
if [ "$zip_status" != 0 ] || [ "$check_status" != 0 ]; then
    info "ERROR: ZIP failed or is incomplete. Keep the original folder: $OUT"; exit 1
fi
/bin/chmod 600 "$ZIP_FILE" || exit 1
permissions
info "Done. Send this ZIP for debugging: $ZIP_FILE"
info 'The uncompressed folder is kept. Do not post the bundle publicly.'
if [ "$EUID" -ne 0 ] && [ -t 1 ]; then
    /usr/bin/open -R "$ZIP_FILE" 2>/dev/null || true
fi
