#!/bin/sh
# T5.3 soak / long-run monitor (board-side).
# Usage: DURATION_SEC=86400 INTERVAL_SEC=60 sh scripts/soak_monitor.sh
# Logs RSS / storage / process presence every INTERVAL_SEC.
set -e
DURATION_SEC="${DURATION_SEC:-86400}"
INTERVAL_SEC="${INTERVAL_SEC:-60}"
OUT="${OUT:-/userdata/log/soak.csv}"
BASE="${BASE:-http://127.0.0.1:8080}"

mkdir -p "$(dirname "$OUT")"
echo "ts,uptime,rss_kb,vsz_kb,pid,mounted,free_pct,http_ok" >"$OUT"

end=$(( $(date +%s) + DURATION_SEC ))
echo "soak start duration=${DURATION_SEC}s interval=${INTERVAL_SEC}s out=$OUT"

while [ "$(date +%s)" -lt "$end" ]; do
  ts=$(date +%s)
  up=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)
  pid=$(pidof ipc_app 2>/dev/null | awk '{print $1}')
  rss=0
  vsz=0
  if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
    rss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status")
    vsz=$(awk '/^VmSize:/{print $2}' "/proc/$pid/status")
  fi
  mounted=0
  use_pct=-1
  free_pct=-1
  if mountpoint -q /mnt/sdcard 2>/dev/null || [ -d /mnt/sdcard/records ]; then
    mounted=1
    # BusyBox df Use% is used percent; free = 100 - used
    use_pct=$(df /mnt/sdcard 2>/dev/null | awk 'NR==2{gsub(/%/,"",$5); print $5+0}')
    if [ -n "$use_pct" ]; then
      free_pct=$((100 - use_pct))
    fi
  fi
  http_ok=0
  if wget -qO /dev/null "$BASE/" 2>/dev/null; then
    http_ok=1
  fi
  echo "$ts,$up,$rss,$vsz,${pid:-0},$mounted,${free_pct:--1},$http_ok" >>"$OUT"
  echo "[soak] ts=$ts pid=${pid:-0} rss=${rss}k free=${free_pct}% http=$http_ok"
  if [ -z "$pid" ]; then
    echo "[soak] WARN: ipc_app not running" >&2
  fi
  sleep "$INTERVAL_SEC"
done

echo "soak done -> $OUT"
