#!/bin/sh
# Smoke test T5.1 time/rtc (read-only; no settimeofday).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/lib_login.sh"
BASE="${BASE:-http://127.0.0.1:8080}"

ipc_login "$BASE"

echo "=== board date ==="
date
hwclock 2>/dev/null || echo "hwclock unavailable"
echo

echo "=== GET system/time ==="
wget -qO- --header="$IPC_AUTH" "$BASE/api/v1/system/time"
echo

echo "=== log rtc/ntp lines ==="
grep -E 'rtc|ntp|hwclock' /userdata/log/ipc_app.log 2>/dev/null | tail -5 || true
echo OK
