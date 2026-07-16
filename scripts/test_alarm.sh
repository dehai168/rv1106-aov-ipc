#!/bin/sh
# Smoke test T4.7 alarm APIs (no live motion; checks config + events list).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/lib_login.sh"
BASE="${BASE:-http://127.0.0.1:8080}"

ipc_login "$BASE"

echo "=== GET alarm/motion ==="
wget -qO- --header="$IPC_AUTH" "$BASE/api/v1/alarm/motion"
echo

echo "=== POST alarm/motion apply=false ==="
printf '%s' '{"sensitivity":3,"apply":false}' > /tmp/alarm.json
wget -qO- --post-file=/tmp/alarm.json --header='Content-Type: application/json' \
  --header="$IPC_AUTH" "$BASE/api/v1/alarm/motion"
echo

echo "=== GET alarm/events ==="
wget -qO- --header="$IPC_AUTH" "$BASE/api/v1/alarm/events?limit=10"
echo

echo OK
