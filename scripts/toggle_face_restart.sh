#!/bin/sh
# Toggle face.enabled and restart ipc_app to test preview impact.
set -e
CFG=/userdata/ipc_config.json
VAL="${1:-false}"
if [ ! -f "$CFG" ]; then
  echo "no config"; exit 1
fi
# busybox-friendly: rewrite enabled line under face (first match after face block is fine with python-less sed)
cp "$CFG" /userdata/ipc_config.json.bak.preview
# Replace only face.enabled (appears as "enabled": true/false right after face block's model fields —
# use a marker approach: set all "enabled" under face by python if available, else crude sed once.
if command -v python3 >/dev/null 2>&1; then
  python3 - <<PY
import json
p="$CFG"
with open(p) as f: c=json.load(f)
c.setdefault("face",{})["enabled"]=("$VAL"=="true")
with open(p,"w") as f: json.dump(c,f,indent=2,ensure_ascii=False)
print("face.enabled=", c["face"]["enabled"])
PY
else
  # crude: change first occurrence of face enabled via awk on known structure
  awk -v v="$VAL" '
    BEGIN{inface=0}
    /"face"[ \t]*:/{inface=1}
    inface && /"enabled"[ \t]*:/{
      if (v=="true") sub(/:.*$/, ": true,");
      else sub(/:.*$/, ": false,");
      inface=0
    }
    {print}
  ' "$CFG" > /tmp/ipc_config.json.new && mv /tmp/ipc_config.json.new "$CFG"
  echo "sed-set face.enabled=$VAL"
fi
killall ipc_app 2>/dev/null || true
sleep 1
nohup /userdata/ipc_app --web >/dev/null 2>&1 &
sleep 3
ps | grep ipc_app | grep -v grep
echo "=== recent ==="
grep -E 'stream start|face init|detect runtime|preview client' /userdata/log/ipc_app.log | tail -15
