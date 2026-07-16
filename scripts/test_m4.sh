#!/bin/sh
# Run T4.5–T4.8 board smoke tests in order (no reboot/reset).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE="${BASE:-http://127.0.0.1:8080}"
export BASE

run_one() {
  name="$1"
  script="$2"
  echo "######## $name ########"
  sh "$script"
  echo
}

run_one "T4.5 video" "$SCRIPT_DIR/test_video.sh"
run_one "T4.6 storage" "$SCRIPT_DIR/test_storage.sh"
run_one "T4.7 alarm" "$SCRIPT_DIR/test_alarm.sh"
run_one "T4.8 system" "$SCRIPT_DIR/test_system.sh"
echo "ALL M4 SMOKE OK"
