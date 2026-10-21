#!/bin/sh
# On-board smoke for F2 face REST API.
set -e
BASE="${1:-http://127.0.0.1:8080}"
. /userdata/lib_login.sh
ipc_login "$BASE" || exit 1
echo "=== GET /api/v1/alarm/face ==="
wget -qO- --header="$IPC_AUTH" "$BASE/api/v1/alarm/face"
echo
echo "=== POST /api/v1/alarm/face (enabled=false apply) ==="
printf '%s' '{"enabled":false,"apply":true,"interval_ms":800}' > /tmp/face_post.json
wget -qO- --post-file=/tmp/face_post.json \
  --header='Content-Type: application/json' \
  --header="$IPC_AUTH" \
  "$BASE/api/v1/alarm/face"
echo
echo "=== GET /api/v1/alarm/face/events ==="
wget -qO- --header="$IPC_AUTH" "$BASE/api/v1/alarm/face/events?limit=5"
echo
echo "=== DELETE /api/v1/alarm/face/events (empty date ok path) ==="
printf '%s' '{"date":"19990101"}' > /tmp/face_del.json
# busybox wget may not support DELETE; use ipc_app curl if present, else skip
if command -v curl >/dev/null 2>&1; then
  curl -s -X DELETE -H "$IPC_AUTH" -H 'Content-Type: application/json' \
    --data-binary @/tmp/face_del.json "$BASE/api/v1/alarm/face/events"
  echo
else
  echo "skip DELETE (no curl)"
fi
echo "FACE_API_SMOKE_OK"
