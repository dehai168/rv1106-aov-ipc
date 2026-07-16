#!/bin/sh
# Smoke test T4.4 network API (no static IP change; apply=false).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/lib_login.sh"
BASE="${BASE:-http://127.0.0.1:8080}"

ipc_login "$BASE"
AUTH="$IPC_AUTH"

echo "=== GET network/config ==="
wget -qO- --header="$AUTH" "$BASE/api/v1/network/config"
echo

echo "=== POST network/config apply=false (dhcp) ==="
printf '%s' '{"mode":"dhcp","apply":false}' > /tmp/net_dhcp.json
wget -qO- --post-file=/tmp/net_dhcp.json --header='Content-Type: application/json' \
  --header="$AUTH" "$BASE/api/v1/network/config"
echo

echo "=== POST invalid iface (expect code 1002) ==="
printf '%s' '{"iface":"usb0","mode":"dhcp","apply":false}' > /tmp/net_bad.json
wget -qO- --post-file=/tmp/net_bad.json --header='Content-Type: application/json' \
  --header="$AUTH" "$BASE/api/v1/network/config" || true
echo

echo OK
