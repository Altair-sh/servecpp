#!/usr/bin/env bash
# Range requests: partial content, suffix, open end, unsatisfiable
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir "$TMP/public"
head -c 1000 /dev/urandom > "$TMP/public/r.bin"
start_server '
[[share]]
url = "/public"
path = "public"
'

# check_range <curl range> <offset> <length>
check_range() {
    local code
    code=$(curl -s -r "$1" -o "$TMP/part" -w '%{http_code}' "$URL/public/r.bin")
    [ "$code" = 206 ] || fail "range $1: expected 206, got $code"
    tail -c +$(($2 + 1)) "$TMP/public/r.bin" | head -c "$3" > "$TMP/expected"
    cmp "$TMP/expected" "$TMP/part" || fail "range $1: wrong bytes"
}

check_range 100-199 100 100
check_range 900- 900 100
check_range -100 900 100
check_range 990-5000 990 10

expect_header '^content-range: bytes 100-199/1000' -r 100-199 "$URL/public/r.bin"
expect_status 416 -r 2000-3000 "$URL/public/r.bin"
expect_header '^content-range: bytes \*/1000' -r 2000-3000 "$URL/public/r.bin"

# multiple ranges aren't supported: the whole file
expect_status 200 -r 0-1,5-6 "$URL/public/r.bin"
