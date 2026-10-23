#!/bin/bash
# Runs SimpleMtkWlan's AWDL tests (kext/Wifi/vendor/tests/awdl) against the
# Rust AWDL core: the C++ headers forward to it, so every test binary is
# linked with the host build of mtk-ffi. Also checks the shared struct layout.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
if [ -z "${DEVELOPER_DIR:-}" ] && xcode-select -p | grep -q CommandLineTools; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
CARGO="$(command -v cargo || echo "$HOME/.cargo/bin/cargo")"
(cd rust && "$CARGO" build -q -p mtk-ffi --features wifi --target-dir "$ROOT/build/rust-host")
LIB="$ROOT/build/rust-host/debug/libmtk_ffi.a"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# The Python tests call clang++ themselves; this wrapper adds the Rust core.
mkdir -p "$work/bin"
REAL=/usr/bin/clang++  # xcrun shim: adds the SDK sysroot
cat > "$work/bin/clang++" <<WRAP
#!/bin/sh
case " \$* " in *" -c "*) exec "$REAL" "\$@" ;; esac
exec "$REAL" "\$@" "$LIB"
WRAP
chmod +x "$work/bin/clang++"
export PATH="$work/bin:$PATH"

cd kext/Wifi/vendor
cat > "$work/layout.cpp" <<'CPP'
#include <AWDL/AwdlLayout.hpp>
#include <stdio.h>
int main() {
    long bad = mtkawdl::layoutMismatch();
    if (bad >= 0) { printf("layout mismatch at %ld: C++ %zu, Rust %zu\n", bad,
        bad < long(sizeof(mtkawdl::kLayout) / sizeof(size_t)) ? mtkawdl::kLayout[bad] : 0, mtk_awdl_layout(bad)); return 1; }
    printf("layout: %zu entries match\n", sizeof(mtkawdl::kLayout) / sizeof(size_t));
}
CPP
clang++ -std=c++14 -Wall -Wextra -Werror -Iinclude "$work/layout.cpp" -o "$work/layout" && "$work/layout"
clang++ -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iinclude tests/awdl/protocol-test.cpp -o "$work/protocol-test"
"$work/protocol-test" > /dev/null && echo "protocol-test: PASS"
status=0
for t in tests/awdl/test-*.py; do
    if python3 "$t" > "$work/log" 2>&1; then
        echo "$(basename "$t"): PASS"
    else
        echo "$(basename "$t"): FAIL"
        tail -20 "$work/log"
        status=1
    fi
done
exit $status
