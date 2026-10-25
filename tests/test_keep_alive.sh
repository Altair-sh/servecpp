#!/usr/bin/env bash
# several requests on one connection
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir "$TMP/public"
echo a > "$TMP/public/a.txt"
echo data > "$TMP/f"
start_server '
[[share]]
url = "/public"
path = "public"
write_password = "w"
'

# check_reuse <curl args...>: curl -v reports a reused connection for the second request
check_reuse() {
    local log
    log=$(curl -sv "$@" 2>&1) || true
    # older curl prints "Re-using", newer "Reusing"
    if ! grep -qiE 're-?using existing' <<< "$log"; then
        fail "connection wasn't reused: curl $*"$'\n'"$log"
    fi
}

check_reuse -o /dev/null "$URL/public/a.txt" -o /dev/null "$URL/public/a.txt"
# after a listing, an error and an upload
check_reuse -o /dev/null "$URL/public/" -o /dev/null "$URL/public/a.txt"
check_reuse -o /dev/null "$URL/public/missing" -o /dev/null "$URL/public/a.txt"
check_reuse -o /dev/null -u :w -T "$TMP/f" "$URL/public/f" --next -s -o /dev/null "$URL/public/a.txt"
