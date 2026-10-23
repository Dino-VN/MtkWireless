#!/bin/sh
# Experimental Tahoe driver swap. Never reboots or edits OpenCore/NVRAM.
set -eu
if [ "$(id -u)" != 0 ]; then
    echo 'Run this script with administrator privileges.' >&2
    exit 1
fi
awdl_artifacts='/Volumes/App/SimpleMtkWlan/artifacts/awdl-2026-10-19'
awdl_staging='/Library/Application Support/MtkWlan/AWDL-20261019'
awdl_bundle='com.eiyuumiru.AirportMtkWlan'
mkdir -p "$awdl_staging/candidate" "$awdl_staging/rollback"
ditto "$awdl_artifacts/candidate/AirportMtkWlan.kext" "$awdl_staging/candidate/AirportMtkWlan.kext"
ditto "$awdl_artifacts/rollback/AirportMtkWlan.kext" "$awdl_staging/rollback/AirportMtkWlan.kext"
chown -R root:wheel "$awdl_staging"
chmod -R go-w "$awdl_staging"
exec > "$awdl_staging/hot-reload.log" 2>&1
echo 'Before reload:'
/usr/bin/kmutil showloaded --list-only | /usr/bin/grep "$awdl_bundle" || true
echo 'Attempting hot unload; no reboot will be requested.'
if ! /usr/bin/kmutil unload -b "$awdl_bundle"; then
    echo 'HOT_UNLOAD_REFUSED: current driver retained; no candidate load attempted.'
    exit 2
fi
if /usr/bin/kmutil showloaded --list-only | /usr/bin/grep -q "$awdl_bundle"; then
    echo 'HOT_UNLOAD_INCOMPLETE: current driver still loaded; stopping.'
    exit 3
fi
echo 'Loading experimental AWDL candidate:'
if /usr/bin/kmutil load -p "$awdl_staging/candidate/AirportMtkWlan.kext"; then
    if /usr/bin/kmutil showloaded --list-only | /usr/bin/grep -q '71A7C7C1-E910-35F3-AECA-DDE645A3C915'; then
        echo 'HOT_RELOAD_LOADED: candidate UUID verified. AirDrop remains unverified.'
        /sbin/ifconfig -l
        exit 0
    fi
    echo 'CANDIDATE_UUID_MISSING: attempting rollback.'
else
    echo 'CANDIDATE_LOAD_REFUSED: attempting rollback without a reboot.'
fi
if /usr/bin/kmutil showloaded --list-only | /usr/bin/grep -q "$awdl_bundle"; then
    /usr/bin/kmutil unload -b "$awdl_bundle" || exit 4
fi
if /usr/bin/kmutil load -p "$awdl_staging/rollback/AirportMtkWlan.kext"; then
    echo 'ROLLBACK_LOADED: rebuilt original source; verify normal Wi-Fi.'
else
    echo 'ROLLBACK_LOAD_REFUSED: a restart may be needed to recover the boot-loaded driver.'
fi
exit 5
