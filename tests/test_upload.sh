#!/usr/bin/env bash
# upload, replace, chunked upload, interrupted upload
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir -p "$TMP/up/old"
# temp files of uploads interrupted by a crash, removed on start
echo stale > "$TMP/up/crashed.bin.uploading"
echo stale > "$TMP/up/old/crashed.bin.uploading"
start_server '
[[share]]
url = "/up"
path = "up"
write_password = "w"
'
if [ -e "$TMP/up/crashed.bin.uploading" ] || [ -e "$TMP/up/old/crashed.bin.uploading" ]; then
    fail "temp files of interrupted uploads weren't removed on start"
fi
grep -q 'removed unfinished upload' "$TMP/server.log" || fail "removed temp files aren't logged"

# > 1 MB: curl sends "Expect: 100-continue"
head -c 2000000 /dev/urandom > "$TMP/file.bin"
expect_status 201 -u :w -T "$TMP/file.bin" "$URL/up/file.bin"
cmp "$TMP/file.bin" "$TMP/up/file.bin" || fail "uploaded file differs"

# replace
head -c 1000 /dev/urandom > "$TMP/file2.bin"
expect_status 204 -u :w -T "$TMP/file2.bin" "$URL/up/file.bin"
cmp "$TMP/file2.bin" "$TMP/up/file.bin" || fail "replaced file differs"

# empty file
: > "$TMP/empty"
expect_status 201 -u :w -T "$TMP/empty" "$URL/up/empty"
[ -f "$TMP/up/empty" ] && [ ! -s "$TMP/up/empty" ] || fail "empty upload"

# chunked: curl uses chunked encoding when uploading from stdin
expect_status 201 -u :w -T - "$URL/up/chunked.bin" < "$TMP/file.bin"
cmp "$TMP/file.bin" "$TMP/up/chunked.bin" || fail "chunked upload differs"

# non-ASCII name
expect_status 201 -u :w -T "$TMP/file2.bin" "$URL/up/%D0%BA%D0%BE%D1%82.bin"
printf -v cyrillic '\xd0\xba\xd0\xbe\xd1\x82.bin'
cmp "$TMP/file2.bin" "$TMP/up/$cyrillic" || fail "non-ASCII upload differs"

# directory target, missing parent directory
mkdir "$TMP/up/dir"
expect_status 409 -u :w -T "$TMP/file2.bin" "$URL/up/dir"
expect_status 409 -u :w -T "$TMP/file2.bin" "$URL/up/nodir/file.bin"
no_temp_files "$TMP/up"

# upload check, used by the upload form before sending a file: OPTIONS with X-Upload-Size
expect_status 200 -X OPTIONS -u :w -H 'X-Upload-Size: 10' "$URL/up/file.bin"      # exists, would be replaced
expect_status 404 -X OPTIONS -u :w -H 'X-Upload-Size: 10' "$URL/up/new.bin"       # would be created
expect_status 409 -X OPTIONS -u :w -H 'X-Upload-Size: 10' "$URL/up/dir"
expect_status 400 -X OPTIONS -u :w -H 'X-Upload-Size: abc' "$URL/up/new.bin"
expect_status 400 -X OPTIONS -u :w "$URL/up/new.bin"
if [ -e "$TMP/up/new.bin" ]; then
    fail "upload check created a file"
fi
# errors are explained in the body
expect_body 'a directory with this name exists' -X OPTIONS -u :w -H 'X-Upload-Size: 10' "$URL/up/dir"
expect_body 'invalid path' -X OPTIONS -u :w -H 'X-Upload-Size: 10' "$URL/up/a%3Ab.txt"

# names ending with .uploading are reserved for temp files
expect_status 409 -u :w -T "$TMP/file2.bin" "$URL/up/x.uploading"
expect_status 409 -X OPTIONS -u :w -H 'X-Upload-Size: 10' "$URL/up/x.uploading"

# a temp file that appeared while the server runs (not by an upload) doesn't block uploads, it's hidden and overwritten
echo stale > "$TMP/up/stale.bin.uploading"
listing=$(curl -sf "$URL/up/")
if grep -q 'stale' <<< "$listing"; then
    fail "listing shows a temp file"
fi
expect_status 404 "$URL/up/stale.bin.uploading"
expect_status 201 -u :w -T "$TMP/file2.bin" "$URL/up/stale.bin"
cmp "$TMP/file2.bin" "$TMP/up/stale.bin" || fail "upload over a stale temp file differs"
no_temp_files "$TMP/up"

# slow upload in the background
head -c 5000000 /dev/urandom > "$TMP/big.bin"
curl -s -o /dev/null -u :w -T "$TMP/big.bin" --limit-rate 500K "$URL/up/partial.bin" &
curl_pid=$!
sleep 1
[ -f "$TMP/up/partial.bin.uploading" ] || fail "no temp file during upload: $(ls -A "$TMP/up")"

# only one upload per file at a time
expect_status 409 -u :w -T "$TMP/file2.bin" "$URL/up/partial.bin"
expect_body 'the file is being uploaded by another client' -X OPTIONS -u :w -H 'X-Upload-Size: 10' "$URL/up/partial.bin"

# interrupted upload: the temp file is removed, the target isn't created
kill "$curl_pid"
wait "$curl_pid" || true
for _ in $(seq 30); do
    [ -e "$TMP/up/partial.bin.uploading" ] || break
    sleep 0.1
done
no_temp_files "$TMP/up"
if [ -e "$TMP/up/partial.bin" ]; then
    fail "interrupted upload created the target file"
fi

# the name is free again after the interrupted upload
expect_status 201 -u :w -T "$TMP/file2.bin" "$URL/up/partial.bin"
