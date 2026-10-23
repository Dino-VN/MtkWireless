#!/bin/bash
# Zip both kexts with the OpenCore snippet into dist/.
#
#   scripts/package.sh [Debug|Release]    (run scripts/build.sh first)
set -euo pipefail
cd "$(dirname "$0")/.."
CONF="${1:-Release}"
VERSION="${MODULE_VERSION:-$(cat VERSION)}"
UP="$(echo "$CONF" | tr '[:lower:]' '[:upper:]')"

stage="$(mktemp -d)/MtkWireless-$VERSION-$UP"
mkdir -p "$stage/Kexts" dist
for name in MtkWirelessWifi MtkWirelessBT; do
    test -d "build/$CONF/$name.kext" || { echo "missing build/$CONF/$name.kext" >&2; exit 1; }
    cp -R "build/$CONF/$name.kext" "$stage/Kexts/"
done
cp docs/config-snippet.plist docs/*.md "$stage/"
cp LICENSES/*.txt "$stage/"
(cd "$(dirname "$stage")" && zip -qry "$OLDPWD/dist/$(basename "$stage").zip" "$(basename "$stage")")
rm -rf "$(dirname "$stage")"
echo "dist/$(basename "$stage").zip"
