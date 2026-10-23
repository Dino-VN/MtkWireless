#!/bin/bash
# Build MtkWirelessWifi.kext (macOS 26 Tahoe) and MtkWirelessBT.kext into
# build/<Conf>/.
#
#   scripts/build.sh [Debug|Release]
#
# MODULE_VERSION (numeric, e.g. 1.2.3) defaults to the VERSION file.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

CONF="${1:-Release}"
case "$CONF" in
    Debug)   PROFILE=dev;     PROFILE_DIR=debug;   OPT=(-O0 -DDEBUG=1) ;;
    Release) PROFILE=release; PROFILE_DIR=release; OPT=(-Os) ;;
    *) echo "usage: $0 [Debug|Release]" >&2; exit 2 ;;
esac

VERSION="${MODULE_VERSION:-$(cat VERSION)}"
if ! [[ "$VERSION" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]]; then
    echo "MODULE_VERSION must be numeric (e.g. 1.2.3), got '$VERSION'" >&2
    exit 1
fi

# The kext toolchain needs full Xcode, not the Command Line Tools.
if [ -z "${DEVELOPER_DIR:-}" ] && xcode-select -p | grep -q CommandLineTools; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
SDK="$(xcrun --sdk macosx --show-sdk-path)"
CC="$(xcrun -f clang)"
CXX="$(xcrun -f clang++)"
CARGO="$(command -v cargo || echo "$HOME/.cargo/bin/cargo")"

KSDK="$ROOT/third_party/MacKernelSDK"
LILU="$ROOT/third_party/Lilu.kext/Contents/Resources"
[ -d "$KSDK/Headers" ] && [ -d "$LILU/Headers" ] || { echo "run scripts/bootstrap.sh first" >&2; exit 1; }

OUT="$ROOT/build/$CONF"
OBJ="$ROOT/build/obj/$CONF"
mkdir -p "$OUT" "$OBJ"

# Deployment target per kext, as in its Xcode project: MacKernelSDK picks
# class layouts by it. Set before compile/link_kext.
TARGET=(-target x86_64-apple-macos10.12 -isysroot "$SDK")
# Mirrors Xcode's kernel-extension settings (-mkernel: static relocation,
# no red zone, no implicit float).
CFLAGS=(-nostdinc -mkernel -fno-builtin -fno-common -fstrict-aliasing -g "${OPT[@]}"
        -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT "-DMODULE_VERSION=$VERSION"
        -Wall -Wno-unused-parameter -I "$ROOT/kext/include")
# Header order decides which IOKit class layouts (vtables) the code is built
# against, so each kext uses exactly the order of its Xcode project.
PRIVATE_HEADERS="$SDK/System/Library/Frameworks/Kernel.framework/PrivateHeaders"
BT_INCLUDES=(-I "$PRIVATE_HEADERS" -I "$KSDK/Headers")
CXXFLAGS=(-std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions)

# rust_lib <feature>: build the Rust core for one kext, print the archive path.
rust_lib() {
    local dir="$ROOT/build/rust-$1"
    (cd rust && MACOSX_DEPLOYMENT_TARGET=10.12 "$CARGO" build -p mtk-ffi --features "$1" --profile "$PROFILE" \
        --target targets/x86_64-apple-kext.json -Z build-std=core,alloc \
        -Z json-target-spec --target-dir "$dir") >&2 || exit 1  # fails the $(...) caller
    echo "$dir/x86_64-apple-kext/$PROFILE_DIR/libmtk_ffi.a"
}

# kmod_info <bundle id> <start fn|0> <stop fn|0> <out.c>: what Xcode puts in <name>_info.c.
kmod_info() {
    local start="$2" stop="$3" decl=""
    if [ "$start" != 0 ]; then
        decl="__private_extern__ kern_return_t $start(kmod_info_t *ki, void *data);
__private_extern__ kern_return_t $stop(kmod_info_t *ki, void *data);"
    fi
    cat > "$4" <<EOF
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
$decl

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($1, "$VERSION", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = $start;
__private_extern__ kmod_stop_func_t *_antimain = $stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
EOF
}

# compile <objdir> <flags...> -- <sources...>: compile in parallel, append the
# objects to OBJS. .s files are assembled; everything else is C++.
OBJS=()
compile() {
    local objdir="$1"
    shift
    local flags=()
    while [ "$1" != "--" ]; do flags+=("$1"); shift; done
    shift
    mkdir -p "$objdir"
    local pids=() src o
    for src in "$@"; do
        o="$objdir/$(basename "${src%.*}").o"
        case "$src" in
            *.s) "$CC" "${TARGET[@]}" -c "$src" -o "$o" & ;;
            *)   "$CXX" "${TARGET[@]}" "${CFLAGS[@]}" ${flags[@]+"${flags[@]}"} -c "$src" -o "$o" & ;;
        esac
        pids+=($!)
        OBJS+=("$o")
        # keep about as many compilers as cores
        if [ ${#pids[@]} -ge "$(sysctl -n hw.ncpu)" ]; then
            wait "${pids[0]}" || exit 1
            pids=("${pids[@]:1}")
        fi
    done
    for pid in ${pids[@]+"${pids[@]}"}; do wait "$pid" || exit 1; done
}

# link_kext <name> <bundle id> <out dir> <Info.plist> <rust lib> <start|0> <stop|0>: link OBJS.
link_kext() {
    local name="$1" id="$2" out="$3" plist="$4" lib="$5" start="$6" stop="$7"
    local objdir="$OBJ/$(basename "$out")-$name"
    mkdir -p "$objdir"
    kmod_info "$id" "$start" "$stop" "$objdir/${name}_info.c"
    "$CC" "${TARGET[@]}" "${CFLAGS[@]}" -I "$KSDK/Headers" -std=gnu11 -c "$objdir/${name}_info.c" -o "$objdir/${name}_info.o"

    local kext="$out/$name.kext"
    rm -rf "$kext" "$kext.dSYM"
    mkdir -p "$kext/Contents/MacOS"
    sed "s/@MODULE_VERSION@/$VERSION/g" "$plist" > "$kext/Contents/Info.plist"
    plutil -lint -s "$kext/Contents/Info.plist"
    # -dead_strip drops the unused libm-style functions that compiler_builtins
    # (not part of LTO) brings in; exported symbols and C++ init sections stay.
    "$CXX" "${TARGET[@]}" -nostdlib -Xlinker -kext -Xlinker -no_deduplicate -Xlinker -dead_strip \
        -L "$KSDK/Library/x86_64" "$objdir/${name}_info.o" "${OBJS[@]}" "$lib" \
        -lkmodc++ -lkmod -lcc_kext -o "$kext/Contents/MacOS/$name"
    xcrun dsymutil "$kext/Contents/MacOS/$name" -o "$kext.dSYM"
    OBJS=()
    echo "built $kext"
}

# --- MtkWirelessBT ---------------------------------------------------------
BT_LIB="$(rust_lib bt)"
BT_GEN="$ROOT/build/gen/bt"
python3 scripts/embed_firmware.py mtkbt "$BT_GEN" firmware/bt/*.bin
compile "$OBJ/bt" "${CXXFLAGS[@]}" -DPRODUCT_NAME=MtkWirelessBT -isystem "$LILU" "${BT_INCLUDES[@]}" \
    -I "$ROOT/kext/BT" -I "$ROOT/kext/common" -I "$BT_GEN" \
    -- kext/BT/MtkWirelessBT.cpp kext/BT/BtLoader.cpp kext/BT/USBDeviceController.cpp kext/BT/Patcher.cpp \
       kext/common/EmbeddedFirmware.cpp "$LILU/Library/plugin_start.cpp" "$BT_GEN/firmware.s"
link_kext MtkWirelessBT com.dinovn.MtkWirelessBT "$OUT" kext/BT/Info.plist "$BT_LIB" \
    MtkWirelessBT_kern_start MtkWirelessBT_kern_stop

# --- MtkWirelessWifi (macOS 26 Tahoe) ---------------------------------------
# Sources and flags of SimpleMtkWlan's AirportMtkWlan target (vendored at
# dfca847 under kext/Wifi/vendor).
V=kext/Wifi/vendor
WIFI_VENDOR_SOURCES=(
    $V/mtk80211/zutil.c $V/SimpleMtkWlan/hal_mwx/MtkMwx.cpp $V/mtk80211/compat.cpp
    $V/mtk80211/openbsd/crypto/poly1305.c $V/mtk80211/openbsd/crypto/sha2.c $V/mtk80211/openbsd/crypto/arc4.c
    $V/mtk80211/openbsd/crypto/hmac.c $V/mtk80211/openbsd/net80211/ieee80211_crypto_ccmp.c
    $V/mtk80211/openbsd/net80211/ieee80211_input.c $V/mtk80211/openbsd/net80211/ieee80211_crypto_wep.c
    $V/mtk80211/openbsd/net80211/CTimeout.cpp $V/mtk80211/openbsd/net80211/ieee80211_crypto.c
    $V/mtk80211/openbsd/net80211/ieee80211_amrr.c $V/mtk80211/openbsd/net80211/ieee80211_regdomain.c
    $V/mtk80211/openbsd/net80211/ieee80211_crypto_bip.c $V/mtk80211/openbsd/net80211/timeout.c
    $V/mtk80211/openbsd/crypto/sha1-pbkdf2.c $V/mtk80211/openbsd/sys/_ifq.cpp $V/include/HAL/MtkHalService.cpp
    $V/mtk80211/openbsd/net80211/ieee80211_node.c $V/mtk80211/openbsd/crypto/cast.c $V/mtk80211/openbsd/net80211/_string.c
    $V/mtk80211/openbsd/crypto/aes.c $V/mtk80211/openbsd/net80211/ieee80211_proto.c
    $V/mtk80211/openbsd/crypto/ecb_enc.c $V/mtk80211/openbsd/crypto/md5.c $V/mtk80211/openbsd/crypto/michael.c
    $V/mtk80211/openbsd/net80211/ieee80211_output.c $V/mtk80211/openbsd/sys/_mbuf.cpp
    $V/mtk80211/openbsd/crypto/sha1.c $V/mtk80211/openbsd/crypto/idgen.c
    $V/mtk80211/openbsd/net80211/ieee80211_ioctl.c $V/mtk80211/openbsd/net80211/ieee80211_ra.c
    $V/mtk80211/openbsd/crypto/rmd160.c $V/mtk80211/openbsd/crypto/cmac.c $V/mtk80211/openbsd/net80211/ieee80211.c
    $V/mtk80211/openbsd/crypto/chachapoly.c $V/mtk80211/openbsd/sys/_task.cpp $V/mtk80211/openbsd/crypto/key_wrap.c
    $V/SimpleMtkWlan/MtkWlanUserClient.cpp $V/mtk80211/openbsd/net80211/ieee80211_pae_output.c
    $V/mtk80211/openbsd/net80211/ieee80211_pae_input.c $V/mtk80211/openbsd/crypto/rijndael.c
    $V/mtk80211/openbsd/crypto/gmac.c $V/mtk80211/openbsd/net80211/ieee80211_mira.c
    $V/mtk80211/openbsd/crypto/ecb3_enc.c $V/mtk80211/openbsd/sys/arp.c
    $V/mtk80211/openbsd/net80211/ieee80211_rssadapt.c $V/mtk80211/openbsd/crypto/set_key.c
    $V/mtk80211/openbsd/net80211/ieee80211_crypto_tkip.c $V/mtk80211/openbsd/crypto/blf.c
    $V/SimpleMtkWlan/MtkPCIe.cpp $V/AirportMtkWlan/AirportMtkWlan.cpp $V/AirportMtkWlan/AirportMtkSkywalkInterface.cpp
    $V/AirportMtkWlan/AirportMtkEthernetInterface.cpp $V/AirportMtkWlan/AirportMtkAwdlInterface.cpp
    $V/AirportMtkWlan/MtkPCIEDeviceWrapper.cpp $V/AirportMtkWlan/WCL/WCL.cpp
)
# Xcode resolves "header.h" through a header map of all project headers;
# these are the directories it covers.
WIFI_QUOTE_DIRS=(mtk80211/openbsd/sys mtk80211/openbsd/crypto mtk80211/linux SimpleMtkWlan
    mtk80211/openbsd/net80211 AirportMtkWlan mtk80211 include/ClientKit include/HAL
    SimpleMtkWlan/hal_mwx AirportMtkWlan/WCL include)
WIFI_LIB="$(rust_lib wifi)"
WIFI_GEN="$ROOT/build/gen/wifi"
TARGET=(-target x86_64-apple-macos15.2 -isysroot "$SDK")
python3 scripts/embed_firmware.py mtkwifi "$WIFI_GEN" firmware/wifi/*
# IO80211 ABI: Tahoe (__MAC_26_0) only for now. Sequoia would be
# __MAC_15_2, as its own kext (the ABIs differ and a Sequoia build never
# attaches on Tahoe).
for variant in Tahoe:__MAC_26_0; do
    name="${variant%%:*}" target="${variant#*:}"
    WIFI_FLAGS=(-x c++ -std=gnu++14 -fapple-kext -fno-rtti -fno-exceptions -fno-sanitize=vptr -O1
        -include "$V/SimpleMtkWlan/PrivateSPI.pch"
        -DAIRPORT -DAIRPORT_WCL -DIO80211FAMILY_V2 "-D__IO80211_TARGET=$target" -D__PRIVATE_SPI__
        "-DSMTW_VERSION=\"MtkWireless $VERSION\"" '-DGIT_COMMIT=""' -DIEEE80211_STA_ONLY
        $(for d in "${WIFI_QUOTE_DIRS[@]}"; do printf -- '-iquote %s ' "$V/$d"; done)
        -I "$KSDK/Headers" -I "$V/include/Airport" -I "$V/AirportMtkWlan" -I "$V/SimpleMtkWlan"
        -I "$V/SimpleMtkWlan/hal_mwx" -I "$ROOT/kext/common" -I "$ROOT/kext/Wifi" -I "$WIFI_GEN"
        -isystem "$V/mtk80211/openbsd" -isystem "$V/mtk80211" -isystem "$V/include"
        -I "$PRIVATE_HEADERS" -I "$KSDK/Headers")
    compile "$OBJ/wifi-$name" "${WIFI_FLAGS[@]}" -w \
        -- "${WIFI_VENDOR_SOURCES[@]}" kext/common/EmbeddedFirmware.cpp
    compile "$OBJ/wifi-$name-own" "${WIFI_FLAGS[@]}" -- kext/Wifi/MtkWirelessWifi.cpp "$WIFI_GEN/firmware.s"
    link_kext MtkWirelessWifi com.dinovn.MtkWirelessWifi "$OUT" kext/Wifi/Info.plist "$WIFI_LIB" 0 0
done

scripts/check.sh "$CONF"
