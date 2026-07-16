#!/bin/sh
# Smoke test T4.5 video APIs (no resolution change).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/lib_login.sh"
BASE="${BASE:-http://127.0.0.1:8080}"

ipc_login "$BASE"
AUTH="$IPC_AUTH"

echo "=== GET video/image ==="
wget -qO- --header="$AUTH" "$BASE/api/v1/video/image"
echo

echo "=== POST video/image apply=false ==="
printf '%s' '{"brightness":55,"apply":false}' > /tmp/img.json
wget -qO- --post-file=/tmp/img.json --header='Content-Type: application/json' \
  --header="$AUTH" "$BASE/api/v1/video/image"
echo

echo "=== GET video/encode ==="
wget -qO- --header="$AUTH" "$BASE/api/v1/video/encode"
echo

echo "=== POST video/encode bitrate only apply=false ==="
printf '%s' '{"main":{"bitrate_kbps":1800},"apply":false}' > /tmp/enc.json
wget -qO- --post-file=/tmp/enc.json --header='Content-Type: application/json' \
  --header="$AUTH" "$BASE/api/v1/video/encode"
echo

echo OK
