#!/bin/zsh
set -eu
exec /usr/bin/env python3 -B -E "${0:A:h}/airdrop-benchmark.py" "$@"
