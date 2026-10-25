#!/usr/bin/env bash
# max_upload: too large uploads are rejected, nothing is written
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir "$TMP/up"
start_server '
max_upload = 1000
[[share]]
url = "/up"
path = "up"
write_password = "w"
'

head -c 1000 /dev/urandom > "$TMP/ok.bin"
head -c 2000 /dev/urandom > "$TMP/big.bin"

expect_status 201 -u :w -T "$TMP/ok.bin" "$URL/up/ok.bin"

# upload check by the upload form
expect_status 404 -X OPTIONS -u :w -H 'X-Upload-Size: 1000' "$URL/up/new.bin"
expect_status 413 -X OPTIONS -u :w -H 'X-Upload-Size: 1001' "$URL/up/new.bin"
expect_body 'the file is larger than the limit of 1000 B' -X OPTIONS -u :w -H 'X-Upload-Size: 1001' "$URL/up/new.bin"
expect_body 'the file is larger than the limit of 1000 B' -u :w -T "$TMP/big.bin" "$URL/up/big.bin"

# Content-Length is known: rejected before the body is read
expect_status 413 -u :w -T "$TMP/big.bin" "$URL/up/big.bin"
if [ -e "$TMP/up/big.bin" ]; then
    fail "too large upload created a file"
fi

# chunked: the size is known only while reading
expect_status 413 -u :w -T - "$URL/up/big-chunked.bin" < "$TMP/big.bin"
if [ -e "$TMP/up/big-chunked.bin" ]; then
    fail "too large chunked upload created a file"
fi
no_temp_files "$TMP/up"
