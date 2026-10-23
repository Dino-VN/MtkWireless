#!/bin/bash
# Build MtkWirelessWifi.kext and MtkWirelessBT.kext into build/<Conf>/.
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

TARGET=(-target x86_64-apple-macos10.12 -isysroot "$SDK")
# Mirrors Xcode's kernel-extension settings (-mkernel: static relocation,
# no red zone, no implicit float).
CFLAGS=("${TARGET[@]}" -nostdinc -mkernel -fno-builtin -fno-common -fstrict-aliasing -g "${OPT[@]}"
        -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT "-DMODULE_VERSION=$VERSION"
        -Wall -Wno-unused-parameter
        -I "$ROOT/kext/include" -I "$SDK/System/Library/Frameworks/Kernel.framework/PrivateHeaders"
        -I "$KSDK/Headers")
CXXFLAGS=(-std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions)

# rust_lib <feature>: build the Rust core for one kext, print the archive path.
rust_lib() {
    local dir="$ROOT/build/rust-$1"
    (cd rust && MACOSX_DEPLOYMENT_TARGET=10.12 "$CARGO" build -p mtk-ffi --features "$1" --profile "$PROFILE" \
        --target targets/x86_64-apple-kext.json -Z build-std=core,alloc \
        -Z json-target-spec --target-dir "$dir") >&2
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

# build_kext <name> <bundle id> <rust lib> <start|0> <stop|0> <extra cflags...> -- <sources...>
build_kext() {
    local name="$1" id="$2" lib="$3" start="$4" stop="$5"
    shift 5
    local extra=()
    while [ "$1" != "--" ]; do extra+=("$1"); shift; done
    shift
    local objdir="$OBJ/$name" objs=()
    mkdir -p "$objdir"

    kmod_info "$id" "$start" "$stop" "$objdir/${name}_info.c"
    "$CC" "${CFLAGS[@]}" -std=gnu11 -c "$objdir/${name}_info.c" -o "$objdir/${name}_info.o"
    objs+=("$objdir/${name}_info.o")
    for src in "$@"; do
        local o="$objdir/$(basename "${src%.*}").o"
        "$CXX" "${CFLAGS[@]}" "${CXXFLAGS[@]}" ${extra[@]+"${extra[@]}"} -c "$src" -o "$o"
        objs+=("$o")
    done

    local kext="$OUT/$name.kext"
    rm -rf "$kext" "$kext.dSYM"
    mkdir -p "$kext/Contents/MacOS"
    sed "s/@MODULE_VERSION@/$VERSION/g" "kext/${name#MtkWireless}/Info.plist" > "$kext/Contents/Info.plist"
    plutil -lint -s "$kext/Contents/Info.plist"
    "$CXX" "${TARGET[@]}" -nostdlib -Xlinker -kext -Xlinker -no_deduplicate \
        -L "$KSDK/Library/x86_64" "${objs[@]}" "$lib" \
        -lkmodc++ -lkmod -lcc_kext -o "$kext/Contents/MacOS/$name"
    xcrun dsymutil "$kext/Contents/MacOS/$name" -o "$kext.dSYM"
    echo "built $kext"
}

WIFI_LIB="$(rust_lib wifi)"
build_kext MtkWirelessWifi com.dinovn.MtkWirelessWifi "$WIFI_LIB" 0 0 \
    -- kext/Wifi/MtkWirelessWifi.cpp

BT_LIB="$(rust_lib bt)"
build_kext MtkWirelessBT com.dinovn.MtkWirelessBT "$BT_LIB" MtkWirelessBT_kern_start MtkWirelessBT_kern_stop \
    -DPRODUCT_NAME=MtkWirelessBT -isystem "$LILU" \
    -- kext/BT/MtkWirelessBT.cpp "$LILU/Library/plugin_start.cpp"

scripts/check.sh "$CONF"
