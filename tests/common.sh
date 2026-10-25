#!/usr/bin/env bash
# Sourced by tests/test_*.sh. Starts servecpp with a temporary config and provides checks.
# $TMP: temporary directory, removed on exit. Share directories are created in it.
set -eo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
SERVECPP=${SERVECPP:-build/debug/servecpp}
if [ ! -x "$SERVECPP" ]; then
    echo "servecpp binary not found: $SERVECPP (build it or set SERVECPP)" >&2
    exit 1
fi
# absolute, because the server runs in $TMP
SERVECPP="$(cd "$(dirname "$SERVECPP")" && pwd)/$(basename "$SERVECPP")"

TMP=$(mktemp -d)
SERVER_PID=

cleanup() {
    local code=$?
    if [ -n "$SERVER_PID" ]; then
        # KILL: on Windows a native process doesn't receive SIGTERM from bash, and bash waits 10 s
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ $code -ne 0 ] && [ -f "$TMP/server.log" ]; then
        echo "--- server log:" >&2
        cat "$TMP/server.log" >&2
    fi
    rm -rf "$TMP"
    exit $code
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# start_server <toml>
# Writes $TMP/servecpp.toml with a random port + <toml>, starts servecpp in $TMP, waits until it answers.
# Sets $URL. Share paths in <toml> are relative to $TMP.
start_server() {
    PORT=$((20000 + RANDOM % 10000))
    URL="http://127.0.0.1:$PORT"
    printf 'port = %s\n%s\n' "$PORT" "$1" > "$TMP/servecpp.toml"
    (cd "$TMP" && exec "$SERVECPP" -c servecpp.toml) > "$TMP/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 50); do
        if curl -s -o /dev/null "$URL/"; then
            return 0
        fi
        kill -0 "$SERVER_PID" 2>/dev/null || fail "servecpp exited on startup"
        sleep 0.1
    done
    fail "servecpp didn't start on port $PORT"
}

# expect_status <code> <curl args...>
expect_status() {
    local expected=$1
    shift
    local actual
    actual=$(curl -s -o /dev/null -w '%{http_code}' "$@") || true
    if [ "$actual" != "$expected" ]; then
        fail "expected $expected, got $actual: curl $*"
    fi
}

# expect_header <regex> <curl args...>: response headers (case-insensitive) must match
expect_header() {
    local regex=$1
    shift
    local headers
    headers=$(curl -s -D - -o /dev/null "$@" | tr -d '\r') || true
    if ! grep -qiE "$regex" <<< "$headers"; then
        fail "no header matching '$regex': curl $*"$'\n'"$headers"
    fi
}

# expect_body <text> <curl args...>: the response body (an error message) must contain <text>
expect_body() {
    local expected=$1
    shift
    local body
    body=$(curl -s "$@") || true
    if [[ $body != *"$expected"* ]]; then
        fail "expected body with '$expected', got '$body': curl $*"
    fi
}

# no_temp_files <dir>: no unfinished uploads (*.uploading) are left
no_temp_files() {
    local files
    files=$(ls -A "$1")
    if grep -q '\.uploading$' <<< "$files"; then
        fail "temp files left in $1: $files"
    fi
}
