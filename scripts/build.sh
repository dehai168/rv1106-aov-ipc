#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${LUCKFOX_SDK:-/home/user/work/luckfox-pico}"
TOOL="$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"

export PATH="$TOOL/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LUCKFOX_SDK="$SDK"

BUILD_DIR="$ROOT/build"
mkdir -p "$BUILD_DIR"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-rv1106.cmake" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "OK: $BUILD_DIR/ipc_app"
file "$BUILD_DIR/ipc_app"
