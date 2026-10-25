#!/usr/bin/env bash
# read and write passwords per share
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir "$TMP/public" "$TMP/private" "$TMP/readonly"
echo x > "$TMP/public/x.txt"
echo p > "$TMP/private/p.txt"
echo r > "$TMP/readonly/r.txt"
echo data > "$TMP/f"
start_server '
[[share]]
url = "/public"
path = "public"
write_password = "w"

[[share]]
url = "/private"
path = "private"
read_password = "r"
write_password = "w2"

[[share]]
url = "/readonly"
path = "readonly"
'

# public: anyone reads, writing needs the write password
expect_status 200 "$URL/public/x.txt"
expect_status 401 -T "$TMP/f" "$URL/public/f"
expect_status 401 -u :wrong -T "$TMP/f" "$URL/public/f"
expect_status 201 -u :w -T "$TMP/f" "$URL/public/f"
# the user name is ignored
expect_status 204 -u anyone:w -T "$TMP/f" "$URL/public/f"

# private: reading needs the read or the write password
expect_status 401 "$URL/private/p.txt"
expect_status 401 "$URL/private/"
expect_status 401 -u :wrong "$URL/private/p.txt"
expect_status 200 -u :r "$URL/private/p.txt"
expect_status 200 -u :w2 "$URL/private/p.txt"
expect_status 200 -u :r "$URL/private/"
# writing needs the write password
expect_status 401 -u :r -T "$TMP/f" "$URL/private/f"
expect_status 201 -u :w2 -T "$TMP/f" "$URL/private/f"
# passwords of other shares don't work
expect_status 401 -u :w -T "$TMP/f" "$URL/private/f"

# readonly: no write_password
expect_status 200 "$URL/readonly/r.txt"
expect_status 405 -T "$TMP/f" "$URL/readonly/f"
expect_status 405 -u :w -T "$TMP/f" "$URL/readonly/f"
expect_header '^allow: GET, HEAD' -T "$TMP/f" "$URL/readonly/f"

expect_header '^www-authenticate: Basic realm="/private"' "$URL/private/p.txt"

# upload check by the upload form: needs the write password, and 401 has no WWW-Authenticate,
# otherwise the browser would show its login dialog over the page
expect_status 401 -X OPTIONS -u :wrong -H 'X-Upload-Size: 5' "$URL/public/f"
expect_status 401 -X OPTIONS -u :r -H 'X-Upload-Size: 5' "$URL/private/f"
expect_status 200 -X OPTIONS -u :w2 -H 'X-Upload-Size: 5' "$URL/private/f"
expect_status 405 -X OPTIONS -H 'X-Upload-Size: 5' "$URL/readonly/f"
headers=$(curl -s -D - -o /dev/null -X OPTIONS -H 'X-Upload-Size: 5' "$URL/public/f" | tr -d '\r') || true
if grep -qi '^www-authenticate' <<< "$headers"; then
    fail "upload check 401 has WWW-Authenticate"
fi

# errors are explained in the body
expect_body 'wrong password' -X OPTIONS -u :wrong -H 'X-Upload-Size: 5' "$URL/public/f"
expect_body 'password required' "$URL/private/p.txt"
expect_body 'uploads are disabled for this share' -T "$TMP/f" "$URL/readonly/f"

# upload form only where writing is possible, above the file list
listing=$(curl -sf "$URL/public/")
[[ $listing == *'<form id="upload"'*'<table id="listing">'* ]] || fail "writable share has no upload form above the file list"
listing=$(curl -sf "$URL/readonly/")
if [[ $listing == *'<form id="upload"'* ]]; then
    fail "read-only share has an upload form"
fi
