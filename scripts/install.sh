#!/bin/bash
# MtkWireless installer / updater for OpenCore.
#
#   curl -fsSL https://raw.githubusercontent.com/Dino-VN/MtkWireless/main/scripts/install.sh -o /tmp/mtkw-install.sh && sudo bash /tmp/mtkw-install.sh
#
# Detects the MediaTek Wi-Fi card and USB Bluetooth controller, finds the
# EFI partition OpenCore booted from, removes conflicting Wi-Fi/Bluetooth
# kexts (Intel itlwm/IntelBluetooth, Broadcom BrcmPatchRAM, SimpleMtk*),
# installs or updates MtkWirelessWifi/MtkWirelessBT (+ Lilu, BlueToolFixup
# when missing), updates config.plist and prints what will work on this
# macOS build. Everything it changes is backed up first.
#
# Options:
#   --dry-run       show what would change, change nothing
#   --yes           do not ask for confirmation
#   --force         reinstall even if the EFI already has the latest version
#   --efi DIR       EFI root to use (the folder that contains EFI/OC)
#   --kexts DIR     use MtkWireless*.kext from DIR instead of downloading
#   --version TAG   install this release (e.g. v1.0.7) instead of the latest
set -uo pipefail

REPO="Dino-VN/MtkWireless"
DRY=0 YES=0 FORCE=0 EFI_ROOT="" KEXT_SRC="" TAG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1 ;;
        --yes|-y) YES=1 ;;
        --force) FORCE=1 ;;
        --efi) EFI_ROOT="${2:?}"; shift ;;
        --kexts) KEXT_SRC="${2:?}"; shift ;;
        --version) TAG="${2:?}"; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ -t 1 ]; then B=$'\e[1m' R=$'\e[31m' G=$'\e[32m' Y=$'\e[33m' N=$'\e[0m'; else B="" R="" G="" Y="" N=""; fi
info() { echo "${B}==>${N} $*"; }
ok()   { echo "  ${G}✓${N} $*"; }
warn() { echo "  ${Y}!${N} $*"; WARNINGS+=("$*"); }
die()  { echo "${R}error:${N} $*" >&2; exit 1; }
ask()  {
    [ $YES = 1 ] && return 0
    local a; printf '%s [y/N] ' "$1"; read -r a </dev/tty || return 1
    [[ "$a" =~ ^[YyCc] ]]
}
WARNINGS=()
PB=/usr/libexec/PlistBuddy

if [ "$(id -u)" != 0 ] && [ $DRY = 0 ] && { [ -z "$EFI_ROOT" ] || [ ! -w "$EFI_ROOT/EFI/OC" ]; }; then
    [ -f "$0" ] || die "run with sudo: sudo bash /tmp/mtkw-install.sh"
    exec sudo /bin/bash "$0" "$@"
fi

WORK="$(mktemp -d /tmp/mtkwireless.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- system
info "System"
BUILD="$(sw_vers -buildVersion)"; PRODUCT="$(sw_vers -productVersion)"
KMAJOR="$(uname -r | cut -d. -f1)"
ok "macOS $PRODUCT ($BUILD), Darwin $(uname -r), $(sysctl -n hw.model)"
[ "$(uname -m)" = x86_64 ] || die "MtkWireless is x86_64 only"

# ---------------------------------------------------------------- hardware
info "Hardware"
WIFI_ID="" WIFI_NAME=""
while read -r dev; do
    case "$dev" in
        7961) WIFI_ID="14c3:7961"; WIFI_NAME="MT7921" ;;
        0608) WIFI_ID="14c3:0608"; WIFI_NAME="MT7921K (RZ608)" ;;
        7922) WIFI_ID="14c3:7922"; WIFI_NAME="MT7922" ;;
        0616) WIFI_ID="14c3:0616"; WIFI_NAME="MT7922 (RZ616)" ;;
    esac
done < <(ioreg -r -c IOPCIDevice -l -w0 | awk '
    /"vendor-id" = </ { v = substr($0, index($0, "<") + 1, 4) }
    /"device-id" = </ { d = substr($0, index($0, "<") + 1, 4) }
    /^[ |]*}$/ { if (v == "c314") print substr(d, 3, 2) substr(d, 1, 2); v = ""; d = "" }')
if [ -n "$WIFI_ID" ]; then ok "Wi-Fi: $WIFI_NAME ($WIFI_ID)"; else warn "no supported MediaTek PCIe Wi-Fi card found (14c3:7961/0608/7922/0616)"; fi

USB_IDS="$(ioreg -p IOUSB -l -w0 | awk '
    /"idVendor" = / { v = $NF }
    /"idProduct" = / { p = $NF }
    /^[ |]*}$/ { if (v != "" && p != "") printf "%04x:%04x\n", v, p; v = ""; p = "" }' | sort -u)"

# ---------------------------------------------------------------- release
info "MtkWireless release"
if [ -n "$KEXT_SRC" ]; then
    SRC="$KEXT_SRC"
else
    api="https://api.github.com/repos/$REPO/releases/${TAG:+tags/$TAG}"
    [ -n "$TAG" ] || api="${api}latest"
    json="$(curl -fsSL -H 'Accept: application/vnd.github+json' "$api")" || die "cannot reach GitHub ($api)"
    url="$(printf '%s' "$json" | grep -o '"browser_download_url": *"[^"]*-RELEASE\.zip"' | head -1 | sed 's/.*"\(https[^"]*\)"/\1/')"
    [ -n "$url" ] || die "no RELEASE zip in $api"
    curl -fsSL "$url" -o "$WORK/mtk.zip" || die "download failed: $url"
    unzip -q "$WORK/mtk.zip" -d "$WORK/mtk" || die "bad zip"
    SRC="$(dirname "$(find "$WORK/mtk" -name MtkWirelessWifi.kext -maxdepth 4 | head -1)")"
fi
[ -d "$SRC/MtkWirelessWifi.kext" ] && [ -d "$SRC/MtkWirelessBT.kext" ] || die "MtkWireless*.kext not found in $SRC"
NEWVER="$($PB -c 'Print CFBundleVersion' "$SRC/MtkWirelessWifi.kext/Contents/Info.plist")"
ok "version $NEWVER"

# Bluetooth controllers MtkWirelessBT matches, read from its Info.plist.
BT_MATCH="$(osascript -l JavaScript - "$SRC/MtkWirelessBT.kext/Contents/Info.plist" <<'EOF' 2>/dev/null
ObjC.import('Foundation');
function run(argv) {
  const d = ObjC.deepUnwrap($.NSDictionary.dictionaryWithContentsOfFile(argv[0]));
  const out = [];
  for (const k in d.IOKitPersonalities || {}) {
    const p = d.IOKitPersonalities[k];
    if (p.idVendor !== undefined && p.idProduct !== undefined)
      out.push(('000' + p.idVendor.toString(16)).slice(-4) + ':' + ('000' + p.idProduct.toString(16)).slice(-4));
  }
  return out.join('\n');
}
EOF
)"
BT_ID=""
for id in $USB_IDS; do
    if printf '%s\n' "$BT_MATCH" | grep -qx "$id"; then BT_ID="$id"; break; fi
done
if [ -n "$BT_ID" ]; then ok "Bluetooth: MediaTek USB $BT_ID"; else warn "no supported MediaTek USB Bluetooth controller found"; fi
[ -n "$WIFI_ID" ] || [ -n "$BT_ID" ] || die "no MediaTek MT7921/MT7922 hardware found, nothing to install"

INSTALL_WIFI=0 INSTALL_BT=0
[ -n "$WIFI_ID" ] && INSTALL_WIFI=1
[ -n "$BT_ID" ] && INSTALL_BT=1
if [ $INSTALL_WIFI = 1 ] && [ "$KMAJOR" != 25 ]; then
    warn "MtkWirelessWifi needs macOS 26 Tahoe (Darwin 25); this is Darwin $KMAJOR, Wi-Fi kext not installed"
    INSTALL_WIFI=0
fi

# ---------------------------------------------------------------- EFI
info "OpenCore EFI"
if [ -z "$EFI_ROOT" ]; then
    uuid="$(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path 2>/dev/null |
        grep -oE 'GPT,[0-9A-Fa-f-]{36}' | head -1 | cut -d, -f2)"
    [ -n "$uuid" ] || die "not booted by OpenCore (no boot-path in NVRAM); use --efi /Volumes/EFI"
    part=""
    for d in $(diskutil list | awk '$2 == "EFI" { print $NF }'); do
        if diskutil info "$d" | grep -qi "Partition UUID: *$uuid"; then part="$d"; break; fi
    done
    [ -n "$part" ] || die "boot EFI partition $uuid not found"
    mp="$(diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
    if [ -z "$mp" ]; then
        diskutil mount "$part" >/dev/null || die "cannot mount $part"
        mp="$(diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
    fi
    EFI_ROOT="$mp"
    ok "booted from $part, mounted at $EFI_ROOT"
fi
OC="$EFI_ROOT/EFI/OC"; CFG="$OC/config.plist"; KX="$OC/Kexts"
[ -f "$CFG" ] && [ -d "$KX" ] || die "$OC has no config.plist/Kexts"
vault="$($PB -c 'Print :Misc:Security:Vault' "$CFG" 2>/dev/null || echo Optional)"
[ "$vault" = Optional ] || die "Misc > Security > Vault is $vault: editing config.plist would break boot. Set it to Optional first."
plutil -lint "$CFG" >/dev/null || die "config.plist is not a valid plist"

kver() { [ -f "$1/Contents/Info.plist" ] && $PB -c 'Print CFBundleVersion' "$1/Contents/Info.plist" 2>/dev/null; }
vlt() { # $1 < $2 ?
    [ "$1" = "$2" ] && return 1
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | head -1)" = "$1" ]
}
OLDWIFI="$(kver "$KX/MtkWirelessWifi.kext")"; OLDBT="$(kver "$KX/MtkWirelessBT.kext")"
UPDATE=0
for pair in "$INSTALL_WIFI:$OLDWIFI" "$INSTALL_BT:$OLDBT"; do
    want="${pair%%:*}"; old="${pair#*:}"
    [ "$want" = 1 ] || continue
    if [ -z "$old" ]; then UPDATE=1; elif vlt "$old" "$NEWVER"; then UPDATE=1; fi
done
[ $FORCE = 1 ] && UPDATE=1
ok "installed in EFI: Wifi ${OLDWIFI:-none}, BT ${OLDBT:-none}; available: $NEWVER"
[ $UPDATE = 1 ] || ok "already up to date (use --force to reinstall); checking config only"

# ---------------------------------------------------------------- config plan
# Conflicting kexts: other drivers for the same Wi-Fi/Bluetooth hardware
# role, or older MediaTek ports this project replaces.
CONFLICTS="itlwm AirportItlwm IntelBluetoothFirmware IntelBTPatcher IntelBluetoothInjector \
BrcmPatchRAM BrcmPatchRAM2 BrcmPatchRAM3 BrcmFirmwareData BrcmFirmwareRepo BrcmBluetoothInjector \
BrcmBluetoothInjectorLegacy BrcmNonPatchRAM BrcmNonPatchRAM2 SimpleMtkWlan AirportMtkWlan \
SimpleMtkBtFw SimpleMtkBtPatcher MtkBtFw"

cat > "$WORK/config.js" <<'EOF'
ObjC.import('Foundation');
function load(p) {
  const d = $.NSData.dataWithContentsOfFile(p);
  return $.NSPropertyListSerialization.propertyListWithDataOptionsFormatError(d, 2, null, null);
}
function str(o) { return o && !o.isNil() ? ObjC.unwrap(o) : ''; }
function bundleBase(bp) { return bp.split('/')[0].replace(/\.kext$/i, ''); }
function entry(bundle, exe, comment, minK, maxK) {
  const e = $.NSMutableDictionary.dictionary;
  e.setObjectForKey($('x86_64'), 'Arch');
  e.setObjectForKey($(bundle + '.kext'), 'BundlePath');
  e.setObjectForKey($(comment), 'Comment');
  e.setObjectForKey($.NSNumber.numberWithBool(true), 'Enabled');
  e.setObjectForKey($('Contents/MacOS/' + exe), 'ExecutablePath');
  e.setObjectForKey($(maxK), 'MaxKernel');
  e.setObjectForKey($(minK), 'MinKernel');
  e.setObjectForKey($('Contents/Info.plist'), 'PlistPath');
  return e;
}
function run(argv) {
  const [path, mode, conflictList, wifi, bt] = argv;
  const conflicts = conflictList.split(' ').filter(x => x).map(x => x.toLowerCase());
  const pl = load(path);
  if (!pl || pl.isNil()) throw 'cannot read ' + path;
  const add = pl.objectForKey('Kernel').objectForKey('Add');
  const out = [];
  const names = [];
  for (let i = 0; i < add.count; i++) names.push(bundleBase(str(add.objectAtIndex(i).objectForKey('BundlePath'))));
  const enabled = i => { const e = add.objectAtIndex(i).objectForKey('Enabled'); return e && !e.isNil() && e.boolValue; };
  // Remove conflicts and our own entries (re-added below in the right place).
  for (let i = names.length - 1; i >= 0; i--) {
    const n = names[i].toLowerCase();
    if (conflicts.includes(n)) { out.push('remove ' + names[i] + (enabled(i) ? '' : ' (disabled)')); add.removeObjectAtIndex(i); names.splice(i, 1); }
    else if (n === 'mtkwirelesswifi' || n === 'mtkwirelessbt') { add.removeObjectAtIndex(i); names.splice(i, 1); }
  }
  let lilu = names.findIndex(n => n.toLowerCase() === 'lilu');
  if (bt === '1') {
    if (lilu < 0) { add.insertObjectAtIndex(entry('Lilu', 'Lilu', 'Lilu (needed by MtkWirelessBT)', '', ''), 0); names.unshift('Lilu'); lilu = 0; out.push('add Lilu'); }
    else if (!enabled(lilu)) { add.objectAtIndex(lilu).setObjectForKey($.NSNumber.numberWithBool(true), 'Enabled'); out.push('enable Lilu'); }
    let btf = names.findIndex(n => n.toLowerCase() === 'bluetoolfixup');
    if (btf < 0) { add.insertObjectAtIndex(entry('BlueToolFixup', 'BlueToolFixup', 'BrcmPatchRAM BlueToolFixup', '21.0.0', ''), lilu + 1); names.splice(lilu + 1, 0, 'BlueToolFixup'); out.push('add BlueToolFixup'); }
    else {
      if (!enabled(btf)) { add.objectAtIndex(btf).setObjectForKey($.NSNumber.numberWithBool(true), 'Enabled'); out.push('enable BlueToolFixup'); }
      if (btf < lilu) out.push('WARN BlueToolFixup is listed before Lilu in Kernel > Add');
    }
  }
  if (wifi === '1') { add.addObject(entry('MtkWirelessWifi', 'MtkWirelessWifi', 'MediaTek MT7921/MT7922 Wi-Fi (macOS 26)', '25.0.0', '25.99.99')); out.push('add MtkWirelessWifi'); }
  if (bt === '1') {
    add.addObject(entry('MtkWirelessBT', 'MtkWirelessBT', 'MediaTek MT7921/MT7922 Bluetooth', '', '')); out.push('add MtkWirelessBT');
    const guid = '7C436110-AB2A-4BBB-A880-FE41995C9F82';
    let nv = pl.objectForKey('NVRAM');
    if (!nv || nv.isNil()) { nv = $.NSMutableDictionary.dictionary; pl.setObjectForKey(nv, 'NVRAM'); }
    for (const sect of ['Add', 'Delete']) if (nv.objectForKey(sect).isNil()) nv.setObjectForKey($.NSMutableDictionary.dictionary, sect);
    let a = nv.objectForKey('Add').objectForKey(guid);
    if (a.isNil()) { a = $.NSMutableDictionary.dictionary; nv.objectForKey('Add').setObjectForKey(a, guid); }
    const want = { bluetoothInternalControllerInfo: 'AAAAAAAAAAAAAAAAAAA=', bluetoothExternalDongleFailed: 'AA==' };
    for (const k in want) {
      const v = $.NSData.alloc.initWithBase64EncodedStringOptions($(want[k]), 0);
      const cur = a.objectForKey(k);
      if (cur.isNil() || !cur.isEqual(v)) { a.setObjectForKey(v, k); out.push('set NVRAM ' + k); }
    }
    let del = nv.objectForKey('Delete').objectForKey(guid);
    if (del.isNil()) { del = $.NSMutableArray.array; nv.objectForKey('Delete').setObjectForKey(del, guid); }
    for (const k in want) if (!del.containsObject($(k))) { del.addObject($(k)); out.push('NVRAM Delete ' + k); }
  }
  if (mode === 'write') {
    const data = $.NSPropertyListSerialization.dataWithPropertyListFormatOptionsError(pl, 100, 0, null);
    if (!data.writeToFileAtomically(path, true)) throw 'cannot write ' + path;
  }
  return out.join('\n');
}
EOF
PLAN="$(osascript -l JavaScript "$WORK/config.js" "$CFG" plan "$CONFLICTS" "$INSTALL_WIFI" "$INSTALL_BT" 2>&1)" || die "config.plist: $PLAN"

REMOVE_FILES=()
for k in $CONFLICTS; do [ -d "$KX/$k.kext" ] && REMOVE_FILES+=("$k.kext"); done
NEED_LILU=0 NEED_BTF=0
if [ $INSTALL_BT = 1 ]; then
    [ -d "$KX/Lilu.kext" ] || NEED_LILU=1
    [ -d "$KX/BlueToolFixup.kext" ] || NEED_BTF=1
fi
LOADED_CONFLICTS="$(kextstat 2>/dev/null | grep -oiE 'itlwm|AirportItlwm|IntelBluetooth[A-Za-z]*|IntelBTPatcher|BrcmPatchRAM[0-9]*|BrcmFirmware[A-Za-z]*|BrcmBluetoothInjector[A-Za-z]*|SimpleMtk[A-Za-z]*|AirportMtkWlan' | sort -u | tr '\n' ' ')"

info "Plan"
[ $UPDATE = 1 ] && [ $INSTALL_WIFI = 1 ] && echo "  install MtkWirelessWifi $NEWVER (was ${OLDWIFI:-none})"
[ $UPDATE = 1 ] && [ $INSTALL_BT = 1 ] && echo "  install MtkWirelessBT $NEWVER (was ${OLDBT:-none})"
[ $NEED_LILU = 1 ] && echo "  download and install Lilu (latest acidanthera release)"
[ $NEED_BTF = 1 ] && echo "  download and install BlueToolFixup (latest BrcmPatchRAM release)"
for f in "${REMOVE_FILES[@]+"${REMOVE_FILES[@]}"}"; do echo "  remove EFI/OC/Kexts/$f (conflicts)"; done
printf '%s\n' "$PLAN" | grep -v '^$' | grep -v '^WARN' | grep -vE '^add (MtkWirelessWifi|MtkWirelessBT)$' | sed 's/^/  config: /'
printf '%s\n' "$PLAN" | grep '^WARN' | sed 's/^WARN //' | while read -r w; do warn "$w"; done
[ -n "$LOADED_CONFLICTS" ] && warn "loaded now (removed after reboot if they come from this EFI): $LOADED_CONFLICTS"

CHANGES="$(printf '%s\n' "$PLAN" | grep -v '^$' | grep -v '^WARN' | grep -cvE '^add (MtkWirelessWifi|MtkWirelessBT)$')"
NOTHING=0
[ $UPDATE = 0 ] && [ $NEED_LILU = 0 ] && [ $NEED_BTF = 0 ] && [ ${#REMOVE_FILES[@]} = 0 ] && [ "$CHANGES" = 0 ] && NOTHING=1
if [ $NOTHING = 1 ]; then info "Nothing to change: MtkWireless $NEWVER and config.plist are already set up"
elif [ $DRY = 1 ]; then info "Dry run: nothing changed"; else
    ask "Apply these changes to $OC?" || die "cancelled"

    BACKUP="${SUDO_USER:+$(eval echo "~$SUDO_USER")}"; BACKUP="${BACKUP:-$HOME}/Desktop/MtkWireless-backup-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$BACKUP/Kexts" && cp "$CFG" "$BACKUP/config.plist" || die "cannot back up to $BACKUP"
    ok "backup: $BACKUP"

    fetch_acid() { # repo kext
        local j u
        j="$(curl -fsSL "https://api.github.com/repos/acidanthera/$1/releases/latest")" || return 1
        u="$(printf '%s' "$j" | grep -o '"browser_download_url": *"[^"]*-RELEASE\.zip"' | head -1 | sed 's/.*"\(https[^"]*\)"/\1/')"
        [ -n "$u" ] && curl -fsSL "$u" -o "$WORK/$1.zip" && unzip -qo "$WORK/$1.zip" -d "$WORK/$1" || return 1
        local k; k="$(find "$WORK/$1" -name "$2.kext" -maxdepth 3 -type d | head -1)"
        [ -n "$k" ] && cp -R "$k" "$KX/" && ok "installed $2 $(kver "$KX/$2.kext")"
    }
    [ $NEED_LILU = 1 ] && { fetch_acid Lilu Lilu || die "Lilu download failed"; }
    [ $NEED_BTF = 1 ] && { fetch_acid BrcmPatchRAM BlueToolFixup || die "BlueToolFixup download failed"; }

    for f in "${REMOVE_FILES[@]+"${REMOVE_FILES[@]}"}"; do
        mv "$KX/$f" "$BACKUP/Kexts/" && ok "removed $f (moved to backup)"
    done
    if [ $UPDATE = 1 ]; then
        for k in MtkWirelessWifi MtkWirelessBT; do
            [ "$k" = MtkWirelessWifi ] && [ $INSTALL_WIFI = 0 ] && continue
            [ "$k" = MtkWirelessBT ] && [ $INSTALL_BT = 0 ] && continue
            [ -d "$KX/$k.kext" ] && mv "$KX/$k.kext" "$BACKUP/Kexts/"
            cp -R "$SRC/$k.kext" "$KX/" && ok "installed $k $(kver "$KX/$k.kext")"
        done
    fi
    out="$(osascript -l JavaScript "$WORK/config.js" "$CFG" write "$CONFLICTS" "$INSTALL_WIFI" "$INSTALL_BT" 2>&1)" ||
        { cp "$BACKUP/config.plist" "$CFG"; die "config.plist update failed, restored: $out"; }
    plutil -lint "$CFG" >/dev/null || { cp "$BACKUP/config.plist" "$CFG"; die "config.plist invalid after edit, restored"; }
    ok "config.plist updated"
fi

# ---------------------------------------------------------------- features
info "What works on this Mac (macOS $PRODUCT $BUILD)"
case "$BUILD" in
    25G332) BLE=yes HOTSPOT=yes ;;
    25G241) BLE=yes HOTSPOT=no ;;
    *) BLE=unknown HOTSPOT=no ;;
esac
if [ $INSTALL_WIFI = 1 ]; then
    ok "Wi-Fi: scan, join, WPA2/WPA3, AWDL, 2.4/5 GHz"
    case "$WIFI_ID" in 14c3:7922|14c3:0616|14c3:0608) echo "    6 GHz: off by default, boot-arg amtk_6ghz=1 (OWE/SAE-H2E networks cannot be joined yet)";; esac
fi
if [ $INSTALL_BT = 1 ]; then
    ok "Bluetooth: pairing, audio, AirPods (AAC-ELD), BLE scanning"
    case "$BT_ID" in
        13d3:3610|0489:e0cd) [ "$BLE" = unknown ] && warn "HFP 16 kHz (mSBC) microphone: untested on $BUILD" || ok "HFP 16 kHz (mSBC) microphone" ;;
        *) warn "HFP microphone: CVSD 8 kHz only ($BT_ID is not one of the mSBC-tested IDs)" ;;
    esac
fi
if [ $INSTALL_BT = 1 ] && [ $INSTALL_WIFI = 1 ]; then
    case "$BLE" in
        yes) ok "AirDrop, Handoff, Universal Clipboard, AirPlay to Mac" ;;
        *) warn "AirDrop/Handoff: bluetoothd patches exist only for 25G241 and 25G332; on $BUILD BLE advertising likely does not work" ;;
    esac
    if [ "$HOTSPOT" = yes ]; then ok "Instant Hotspot"
    else warn "Instant Hotspot: needs macOS 26.7.2 (25G332); not available on $BUILD"; fi
else
    warn "AirDrop, Handoff and Instant Hotspot need both the Wi-Fi card and the Bluetooth controller"
fi
warn "iPhone Mirroring: not possible (needs Apple silicon or T2)"

if [ $DRY = 0 ] && [ $NOTHING = 0 ]; then
    info "Done. Reboot to load MtkWireless $NEWVER."
    echo "  After reboot: kextstat | grep -i mtk   and   ioreg -l -w0 | grep RustSelfTestFailed   (expect 0)"
fi
