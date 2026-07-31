#!/bin/sh
# Board smoke for M7 www + key APIs (run on device).
# Usage: sh /mnt/sdcard/smoke_m7_web.sh [base_url] [user] [pass]
set -e
BASE="${1:-http://127.0.0.1:8080}"
USER="${2:-admin}"
PASS="${3:-admin}"

echo "== index =="
wget -qO- "$BASE/" | head -c 120
echo

JS=$(wget -qO- "$BASE/" | sed -n 's/.*src="\([^"]*index-[^"]*\.js\)".*/\1/p' | head -1)
echo "js=$JS"
[ -n "$JS" ] || { echo "FAIL: no js in index"; exit 1; }
wget -qO- "$BASE$JS" >/tmp/smoke_js.bin
JSZ=$(wc -c </tmp/smoke_js.bin)
echo "js_bytes=$JSZ"
[ "$JSZ" -gt 10000 ] || { echo "FAIL: js too small"; exit 1; }

echo "== unauthorized gate =="
UA=$(wget -qO- "$BASE/api/v1/system/info")
echo "$UA" | head -c 120
echo
echo "$UA" | grep -q '"code":1001' || { echo "FAIL: expected 1001"; exit 1; }

echo "== login =="
RESP=$(wget -qO- --post-data="{\"username\":\"$USER\",\"password\":\"$PASS\"}" \
  --header='Content-Type: application/json' "$BASE/api/v1/auth/login" || true)
echo "$RESP" | head -c 160
echo
TOKEN=$(echo "$RESP" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
if [ -z "$TOKEN" ]; then
  echo "SMOKE_OK_STATIC (login skipped: password not default; pass user/pass as args)"
  exit 0
fi

auth() {
  wget -qO- --header="Authorization: Bearer $TOKEN" "$1"
}

echo "== record =="
auth "$BASE/api/v1/record" | head -c 240
echo
echo "== motion =="
auth "$BASE/api/v1/alarm/motion" | head -c 240
echo
echo "== storage =="
auth "$BASE/api/v1/storage/status" | head -c 200
echo
echo "== system =="
auth "$BASE/api/v1/system/info" | head -c 200
echo
echo "SMOKE_OK"
