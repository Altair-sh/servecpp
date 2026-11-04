#!/usr/bin/bash
set -eo pipefail

if [ ! -d tmp/vcpkg ]; then
    mkdir -p tmp
    cd tmp
    git clone https://github.com/microsoft/vcpkg.git
    echo "[+] downloaded vcpkg source code in tmp/vcpkg"
    cd ..
else
    echo "[=] vcpkg source code is already installed in tmp/vcpkg"
fi
if [ ! -f tmp/vcpkg/vcpkg ] && [ ! -f tmp/vcpkg/vcpkg.exe ]; then
    cd tmp/vcpkg
    echo
    ./bootstrap-vcpkg.sh
    echo "[+] downloaded vcpkg binary in tmp/vcpkg"
else
    echo "[=] vcpkg binary is already installed in tmp/vcpkg"
fi

echo VCPKG_DISABLE_METRICS
export VCPKG_DISABLE_METRICS=1
echo VCPKG_ROOT=tmp/vcpkg
export VCPKG_ROOT=tmp/vcpkg
