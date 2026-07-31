#!/bin/sh
export LD_LIBRARY_PATH=/oem/usr/lib

# Patch OSD config via a tiny C-less approach: rewrite keys with awk/python if present
if command -v python3 >/dev/null 2>&1; then
  python3 - <<'PY'
import json
p="/userdata/ipc_config.json"
with open(p,"r",encoding="utf-8") as f:
    cfg=json.load(f)
osd=cfg.setdefault("video",{}).setdefault("osd",{})
osd["enabled"]=True
osd["show_time"]=True
osd["show_name"]=True
osd["name"]="客厅相机"
osd["pos_x"]=40
osd["pos_y"]=80
osd["font_path"]="/userdata/fonts/simsun_cn_3000.ttf"
with open(p,"w",encoding="utf-8") as f:
    json.dump(cfg,f,ensure_ascii=False,indent=2)
print("osd config patched")
PY
fi

: > /tmp/ipc_app.log
killall ipc_app 2>/dev/null || true
sleep 3
# watchdog should restart
for i in 1 2 3 4 5 6 7 8 9 10; do
  if pidof ipc_app >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
sleep 4
echo "=== log ==="
grep -E 'preview-main|freetype|osd|stream start|got #' /tmp/ipc_app.log | head -50
echo "=== ps ==="
ps | grep ipc_app | grep -v grep
echo "=== font ==="
ls -la /userdata/fonts/simsun_cn_3000.ttf
