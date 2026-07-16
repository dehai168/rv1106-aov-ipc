#!/bin/bash
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SDK=/home/user/work/luckfox-pico
TOOL=$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
export PATH=$TOOL/bin:$PATH

cd "$SDK"

BOARD_MK=project/cfg/BoardConfig_IPC/BoardConfig-SPI_NAND-Buildroot-RV1106_Luckfox_Pico_Pro_Max-IPC.mk
rm -f .BoardConfig.mk
ln -sf "$BOARD_MK" .BoardConfig.mk

# Fix PATH for buildroot (no spaces)
export PATH=$(echo "$PATH" | tr -d ' \t\n' | sed 's/::/:/g')
# But we still need standard bins - reconstruct clean PATH
export PATH=$TOOL/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

echo "=== lunch via build.sh info ==="
# Source board config vars
set +u
source "$BOARD_MK"
set -u
env | grep -E '^RK_|^LF_' | sort | head -60

echo "=== build media_libs (long) ==="
# Prefer media-only target
if [ ! -f output/out/media_out/lib/librockit.so ]; then
  ./build.sh media 2>&1 | tee /tmp/build_media.log
else
  echo "media already built"
fi

echo "=== build samples ==="
# After media, samples should be in media_out
if [ -d media/samples/simple_test/out/bin ]; then
  ls -l media/samples/simple_test/out/bin | head
else
  # try make samples from media dir with param
  make -C media samples 2>&1 | tee /tmp/build_samples.log | tail -50
fi

echo "=== find sample binaries ==="
find output media/samples -name 'simple_vi_get_frame' -o -name 'simple_vi_bind_venc' 2>/dev/null | head
