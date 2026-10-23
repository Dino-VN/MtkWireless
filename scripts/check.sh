#!/bin/bash
# Fail if a built kext references symbols the kernel (or Lilu) does not export,
# or if code touches SSE/x87 state (kernel code must not use the FPU).
#
#   scripts/check.sh [Debug|Release]
set -euo pipefail
cd "$(dirname "$0")/.."
CONF="${1:-Release}"
if [ -z "${DEVELOPER_DIR:-}" ] && xcode-select -p | grep -q CommandLineTools; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi

status=0
for name in MtkWirelessWifi MtkWirelessBT; do
    kext="build/$CONF/$name.kext"
    bin="$kext/Contents/MacOS/$name"
    # kmutil prints one "<symbol> in <collection or kext>" line per symbol it
    # resolves; every undefined symbol of the binary must appear there.
    # kmutil only searches the installed kernel collections, so symbols from
    # Lilu (injected by the bootloader) are matched against its exports.
    resolved="$( {
        kmutil libraries -p "$kext" --undef-symbols 2>&1 | awk '$2 == "in" { print $1 }'
        if /usr/libexec/PlistBuddy -c "Print :OSBundleLibraries:as.vit9696.Lilu" \
            "$kext/Contents/Info.plist" >/dev/null 2>&1; then
            nm -gU third_party/Lilu.kext/Contents/MacOS/Lilu | awk '{ print $3 }'
        fi
    } | sort -u)"
    missing="$(comm -23 <(nm -u "$bin" | sort -u) <(echo "$resolved"))"
    if [ -n "$missing" ]; then
        echo "$name: symbols the kernel does not export:" >&2
        echo "$missing" | sed 's/^/    /' >&2
        status=1
    else
        echo "$name: all $(nm -u "$bin" | wc -l | tr -d ' ') undefined symbols resolve"
    fi
    if xcrun otool -tV "$bin" | grep -qE '%[xy]mm[0-9]|[[:space:]]f(ld|st|add|mul|div)'; then
        echo "$name: SSE/x87 instructions found" >&2
        status=1
    fi
done
exit $status
