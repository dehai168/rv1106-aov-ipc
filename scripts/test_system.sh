#!/bin/sh
# Smoke test T4.8 system API (no reboot/reset).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/lib_login.sh"
BASE="${BASE:-http://127.0.0.1:8080}"

ipc_login "$BASE"
AUTH="$IPC_AUTH"

echo "=== GET system/info ==="
wget -qO- --header="$AUTH" "$BASE/api/v1/system/info"
echo

echo "=== GET system/time ==="
wget -qO- --header="$AUTH" "$BASE/api/v1/system/time"
echo

echo "=== POST system/time (timezone only) ==="
printf '%s' '{"timezone":"Asia/Shanghai","ntp_enabled":false}' > /tmp/time.json
wget -qO- --post-file=/tmp/time.json --header='Content-Type: application/json' \
  --header="$AUTH" "$BASE/api/v1/system/time"
echo

echo "=== GET system/log head ==="
wget -qO- --header="$AUTH" "$BASE/api/v1/system/log" | head -c 120
echo

echo OK
