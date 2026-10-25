#!/usr/bin/env bash
# requests must never reach files outside the share
set -eo pipefail
source "$(dirname "$0")/common.sh"

mkdir "$TMP/public"
echo ok > "$TMP/public/ok.txt"
echo secret > "$TMP/secret.txt"
start_server '
[[share]]
url = "/public"
path = "public"
write_password = "w"
'

expect_status 200 "$URL/public/ok.txt"

for p in \
    '/public/../secret.txt' \
    '/public/./../secret.txt' \
    '/public/%2e%2e/secret.txt' \
    '/public/%2e%2e%2fsecret.txt' \
    '/public/..%5csecret.txt' \
    '/public/%5c..%5csecret.txt' \
    '/public/%00' \
    '/public/ok.txt%00' \
    '/public/C:/secret.txt' \
    '/public/ok.txt::$DATA' \
    '/public/ok.txt.' \
    '/public/NUL' \
    '/public/%zz' \
    '/../secret.txt' \
    '/public/%C0%AE%C0%AE/secret.txt'
do
    code=$(curl -s --path-as-is -o "$TMP/out" -w '%{http_code}' "$URL$p") || true
    case $code in
        400|404) ;;
        *) fail "GET $p returned $code" ;;
    esac
    if grep -qx secret "$TMP/out"; then
        fail "GET $p returned the secret file"
    fi
done

# uploads outside the share
echo evil > "$TMP/evil"
for p in '/public/../evil.txt' '/public/%2e%2e%2fevil.txt' '/public/..%5cevil.txt'; do
    code=$(curl -s --path-as-is -o /dev/null -w '%{http_code}' -u :w -T "$TMP/evil" "$URL$p") || true
    case $code in
        400|404) ;;
        *) fail "PUT $p returned $code" ;;
    esac
done
if [ -e "$TMP/evil.txt" ]; then
    fail "upload escaped the share"
fi
