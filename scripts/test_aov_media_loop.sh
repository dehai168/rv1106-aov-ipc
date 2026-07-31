#!/bin/sh
# T6.5 smoke: lapse + optional debug motion + event record on TF.
# Log to /tmp first (TF mount must be released for mem suspend).
# Usage on board: sh /mnt/sdcard/test_aov_media_loop.sh [cycles]
N=${1:-6}
TMP=/tmp/aov_media_loop.log
OUT=/mnt/sdcard/aov_media_loop.log
APP=/userdata/ipc_app

echo "=== aov media loop N=$N start $(date) ===" >"$TMP"
killall ipc_app 2>/dev/null
sleep 1
if "$APP" --aov loop "$N" --media >>"$TMP" 2>&1; then
  echo "PASS media loop $N $(date)" >>"$TMP"
  RC=0
else
  echo "FAIL media loop $(date)" >>"$TMP"
  RC=1
fi
echo "lapse_jpg=$(ls /mnt/sdcard/aov_lapse 2>/dev/null | wc -l)" >>"$TMP"
echo "records=$(ls /mnt/sdcard/records/*.mp4 2>/dev/null | wc -l)" >>"$TMP"
cp "$TMP" "$OUT"
sync
exit "$RC"
