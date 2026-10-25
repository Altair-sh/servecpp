#!/usr/bin/env bash
# runs every tests/test_*.sh, stops at the first failure
# SERVECPP=build/release/servecpp ./test.sh tests another build
set -eo pipefail
cd "$(dirname "$0")"
for t in tests/test_*.sh; do
    echo "== $t"
    bash "$t"
done
echo "all tests passed"
