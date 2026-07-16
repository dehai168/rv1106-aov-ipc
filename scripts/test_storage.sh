#!/bin/sh
# Smoke test T4.6 storage APIs.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/lib_login.sh"
BASE="${BASE:-http://127.0.0.1:8080}"

ipc_login "$BASE"

echo "=== GET storage/status ==="
wget -qO- --header="$IPC_AUTH" "$BASE/api/v1/storage/status"
echo

echo "=== GET storage/records ==="
wget -qO /tmp/records.json --header="$IPC_AUTH" "$BASE/api/v1/storage/records?limit=5"
cat /tmp/records.json
echo

PATH1=$(sed -n 's/.*"path":"\([^"]*\)".*/\1/p' /tmp/records.json | head -1)
if [ -n "$PATH1" ]; then
  echo "=== GET storage/download head ($PATH1) ==="
  wget -qO- --header="$IPC_AUTH" \
    "$BASE/api/v1/storage/download?path=$(printf '%s' "$PATH1" | sed 's|/|%2F|g')" \
    | head -c 32 | od -An -tx1 | head -3
  echo
else
  echo "WARN: no mp4 on card; skip download probe"
fi

echo OK
