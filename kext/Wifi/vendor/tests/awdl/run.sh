#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
awdl_test_dir=$(mktemp -d /tmp/mtk-awdl-test.XXXXXX)
trap 'rm -rf "$awdl_test_dir"' EXIT
clang++ -std=c++14 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude tests/awdl/protocol-test.cpp -o "$awdl_test_dir/protocol-test"
"$awdl_test_dir/protocol-test"
python3 tests/awdl/test-native-windows.py
