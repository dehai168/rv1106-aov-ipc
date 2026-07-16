#!/bin/bash
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SDK=/home/user/work/luckfox-pico
TOOL=$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
export PATH=$TOOL/bin:$PATH
export ARCH=arm
export CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-

cd "$SDK"

BOARD_MK=project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk
if [ ! -f "$BOARD_MK" ]; then
  echo "missing $BOARD_MK"
  exit 1
fi

# Non-interactive lunch: create the symlink build.sh expects
rm -f .BoardConfig.mk
ln -sf "$BOARD_MK" .BoardConfig.mk
# Some SDK versions also look under project/
rm -f project/.BoardConfig.mk
ln -sf "../$BOARD_MK" project/.BoardConfig.mk 2>/dev/null || ln -sf "$SDK/$BOARD_MK" project/.BoardConfig.mk

echo "=== board ==="
readlink -f .BoardConfig.mk
grep -E 'RK_CHIP|RK_BOOT_MEDIUM|LF_TARGET|Luckfox|RK_KERNEL_DTS|RK_APP' -n "$BOARD_MK" | head -40

echo "=== check prebuilt media libs ==="
find media -name 'librockit.so*' 2>/dev/null | head
find media -name 'librkaiq.so*' 2>/dev/null | head
ls output/out/media_out 2>/dev/null | head || echo "no media_out yet"

echo "=== sample Makefile ==="
sed -n '1,100p' media/samples/simple_test/Makefile

echo "=== try build media if needed ==="
if [ ! -d output/out/media_out/lib ] && [ ! -d output/out/media_out/usr/lib ]; then
  # build.sh media - may take long; log to file
  echo "starting ./build.sh media ..."
  ./build.sh media 2>&1 | tee /tmp/build_media.log | tail -n 5
else
  echo "media_out present, skip media rebuild"
fi
