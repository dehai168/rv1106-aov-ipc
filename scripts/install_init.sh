#!/bin/bash
# Deploy ipc_app watchdog + init script to board (T5.2).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

adb push "$ROOT/build/ipc_app" /userdata/ipc_app
adb shell chmod +x /userdata/ipc_app
adb push "$ROOT/scripts/ipc_watchdog.sh" /userdata/ipc_watchdog.sh
adb shell chmod +x /userdata/ipc_watchdog.sh
adb push "$ROOT/scripts/S99ipc_app" /userdata/S99ipc_app
adb shell chmod +x /userdata/S99ipc_app

echo "Installing init script (needs root)..."
adb shell "cp /userdata/S99ipc_app /etc/init.d/S99ipc_app 2>/dev/null && chmod +x /etc/init.d/S99ipc_app && echo OK: /etc/init.d/S99ipc_app || echo WARN: copy to /etc/init.d failed — run manually as root"

echo "Try: adb shell /userdata/S99ipc_app start"
