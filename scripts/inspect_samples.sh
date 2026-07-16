#!/bin/bash
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH=/home/user/work/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
SDK=/home/user/work/luckfox-pico

echo "=== samples Makefile ==="
sed -n '1,140p' "$SDK/media/samples/Makefile"
echo "==== simple_test Makefile ===="
sed -n '1,100p' "$SDK/media/samples/simple_test/Makefile"
echo "==== prebuilt rockit ===="
find "$SDK/media" -name 'librockit*' 2>/dev/null | head -20
echo "==== rv1106 sos ===="
find "$SDK/media" -path '*rv1106*' -name '*.so' 2>/dev/null | head -40
echo "==== media build scripts ===="
ls "$SDK/media" | head -40
head -80 "$SDK/media/Makefile" 2>/dev/null || true
