#!/bin/bash
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SDK=/home/user/work/luckfox-pico
SRC=$SDK/output/out/media_out/bin/simple_vi_get_frame
OUT=/mnt/d/CodeProject/rv1106-aov-ipc/build
mkdir -p "$OUT"
cp -f "$SRC" "$OUT/simple_vi_get_frame"
# also copy dependent libs that may be needed if not on board
ls -l "$OUT/simple_vi_get_frame"
file "$OUT/simple_vi_get_frame"
# check linked libs
TOOL=$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
$TOOL/bin/arm-rockchip830-linux-uclibcgnueabihf-readelf -d "$OUT/simple_vi_get_frame" | grep NEEDED || true
