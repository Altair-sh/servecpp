#!/usr/bin/env bash
# invalid configs: servecpp exits with an error instead of starting
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir "$TMP/d"

# run_servecpp <args...>: sets $out and $code
run_servecpp() {
    code=0
    out=$(cd "$TMP" && timeout 5 "$SERVECPP" "$@" 2>&1) || code=$?
    if [ $code -eq 124 ]; then
        fail "servecpp started: $*"
    fi
}

# expect_config_error <expected message regex> <toml>
expect_config_error() {
    printf '%s\n' "$2" > "$TMP/bad.toml"
    run_servecpp -c bad.toml
    if [ $code -eq 0 ]; then
        fail "servecpp accepted config: $2"
    fi
    if ! grep -qiE -- "$1" <<< "$out"; then
        fail "expected error matching '$1', got: $out"
    fi
}

run_servecpp -c nonexistent.toml
[ $code -ne 0 ] || fail "missing config file accepted"
grep -q "config file 'nonexistent.toml' not found" <<< "$out" || fail "missing config: $out"

# default path ./servecpp.toml
run_servecpp
[ $code -ne 0 ] || fail "missing default config file accepted"
grep -q "config file './servecpp.toml' not found" <<< "$out" || fail "missing default config: $out"

share='[[share]]
url = "/d"
path = "d"'

expect_config_error 'no \[\[share\]\]' 'port = 8080'
expect_config_error "share without 'path'" '[[share]]
url = "/d"'
expect_config_error "share without 'url'" '[[share]]
path = "d"'
expect_config_error 'not a directory' '[[share]]
url = "/d"
path = "nonexistent"'
expect_config_error "duplicate url '/d'" "$share
$share"
expect_config_error 'invalid url' '[[share]]
url = "d"
path = "d"'
expect_config_error 'invalid url' '[[share]]
url = "/d/"
path = "d"'
# TOML has no "no", a missing key means no password
expect_config_error 'bad\.toml:4' "$share
read_password = no"
expect_config_error "'read_password' is empty" "$share
read_password = \"\""
expect_config_error "'write_password' must be a string" "$share
write_password = 123"
expect_config_error "unknown key 'read_pasword'" "$share
read_pasword = \"x\""
expect_config_error 'port' "port = 70000
$share"
expect_config_error 'port' "port = 0
$share"
expect_config_error "invalid address '999.1.1.1'" "address = \"999.1.1.1\"
$share"
expect_config_error 'max_upload' "max_upload = 0
$share"
