#!/usr/bin/env bash
# directory listing, download, headers, HEAD, non-ASCII names
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir -p "$TMP/public/sub"
head -c 300000 /dev/urandom > "$TMP/public/data.bin"
printf -v cyrillic '\xd0\xba\xd0\xbe\xd1\x82.txt'  # "кот.txt"
echo hello > "$TMP/public/$cyrillic"

start_server '
[[share]]
url = "/public"
path = "public"
'

# list of shares
shares=$(curl -sf "$URL/")
grep -q 'href="/public/"' <<< "$shares" || fail "share list has no /public/: $shares"

# directory listing: files are links with the download attribute
listing=$(curl -sf "$URL/public/")
grep -q 'href="/public/data.bin" download' <<< "$listing" || fail "listing has no download link: $listing"
grep -q 'href="/public/sub/"' <<< "$listing" || fail "listing has no subdirectory: $listing"
grep -q 'href="/public/%D0%BA%D0%BE%D1%82.txt" download' <<< "$listing" || fail "listing has no encoded non-ASCII name: $listing"
# share without write_password: no upload form
if grep -q '<form' <<< "$listing"; then
    fail "read-only share has an upload form"
fi

# download
curl -sf -o "$TMP/out.bin" "$URL/public/data.bin"
cmp "$TMP/public/data.bin" "$TMP/out.bin" || fail "downloaded file differs"

expect_header '^content-type: application/octet-stream' "$URL/public/data.bin"
expect_header '^content-length: 300000' "$URL/public/data.bin"
expect_header '^content-disposition: inline; filename="data.bin"' "$URL/public/data.bin"
expect_header '^x-content-type-options: nosniff' "$URL/public/data.bin"
expect_header '^accept-ranges: bytes' "$URL/public/data.bin"

# non-ASCII name
body=$(curl -sf "$URL/public/%D0%BA%D0%BE%D1%82.txt")
[ "$body" = hello ] || fail "non-ASCII file: got '$body'"
expect_header "^content-disposition: inline; filename=\"_+\\.txt\"; filename\\*=UTF-8''%D0%BA%D0%BE%D1%82\\.txt" "$URL/public/%D0%BA%D0%BE%D1%82.txt"

# HEAD: headers without body. If the server sent a body, the GET on the same connection would get it as its response
expect_header '^content-length: 300000' -I "$URL/public/data.bin"
curl -sf -I -o /dev/null "$URL/public/data.bin" --next -sf -o "$TMP/out2.bin" "$URL/public/data.bin"
cmp "$TMP/public/data.bin" "$TMP/out2.bin" || fail "GET after HEAD returned a different body"

# If-Modified-Since with the received Last-Modified
last_modified=$(curl -sf -I "$URL/public/data.bin" | tr -d '\r' | grep -i '^last-modified:' | cut -d' ' -f2-)
expect_status 304 -H "If-Modified-Since: $last_modified" "$URL/public/data.bin"

expect_status 404 "$URL/public/missing.bin"
expect_status 404 "$URL/nosuchshare/x"
expect_status 405 -X DELETE "$URL/public/data.bin"
