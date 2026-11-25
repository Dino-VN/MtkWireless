#!/bin/bash
# Read-only AirDrop/Skywalk diagnostics. No sudo, network changes or identifiers.
set -u
printf 'AirDrop network-path diagnostics (read-only)\n'
filter_state=$(sysctl -n net.filter_state 2>/dev/null) || filter_state=unavailable
netagent=$(sysctl -n net.link.generic.system.enable_netagent 2>/dev/null) || netagent=unavailable
printf 'net.filter_state=%s\nnet.link.generic.system.enable_netagent=%s\n' "$filter_state" "$netagent"
if [ "$netagent" = 0 ]; then
    printf 'WARNING: userspace network path is disabled; this may affect AirDrop QUIC performance.\n'
    if [[ "$filter_state" =~ (^|[^[:alnum:]_])pf([^[:alnum:]_]|$) ]]; then
        printf 'pf is listed in filter state. Check firewall rules and whether OpenCore PowerTimeoutKernelPanic is required.\n'
        printf 'This observation alone does not establish which setting caused it. Do not disable protection automatically.\n'
    fi
elif [ "$netagent" = unavailable ]; then
    printf 'Network-path sysctl unavailable on this OS; no conclusion.\n'
fi
# Only interface names and numeric link quality, never IP/MAC/account data.
for interface in $(ifconfig -l 2>/dev/null); do
    case "$interface" in
        en[0-9]*|awdl[0-9]*)
            printf '\n%s LinkQuality:\n' "$interface"
            printf 'show State:/Network/Interface/%s/LinkQuality\n' "$interface" | scutil
            ;;
    esac
done
printf '\nTX pool allocation failures before the driver callback are not driver counters.\n'
printf 'Correlate local Skywalk allocation/ENOMEM logs with MTKAWDLNativeTx* pressure counters.\n'
