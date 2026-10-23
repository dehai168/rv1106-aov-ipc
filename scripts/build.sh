#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${LUCKFOX_SDK:-/home/user/work/luckfox-pico}"
TOOL="$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"

export PATH="$TOOL/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LUCKFOX_SDK="$SDK"

BUILD_DIR="$ROOT/build"
mkdir -p "$BUILD_DIR"

# V<major>.<minor>.<YYYYMMDD>.<5-digit commit serial>. A dirty tree gets the
# serial of the commit it will become, so a tested build matches what lands.
VERSION_BASE="V1.0"
git_q() { git -C "$ROOT" -c safe.directory='*' "$@" 2>/dev/null; }
SERIAL=$(git_q rev-list --count HEAD || echo 0)
if [ -n "$(git_q status --porcelain --untracked-files=no)" ]; then
  SERIAL=$((SERIAL + 1))
fi
APP_VERSION=$(printf '%s.%s.%05d' "$VERSION_BASE" "$(TZ=Asia/Shanghai date +%Y%m%d)" "$SERIAL")
echo "version: $APP_VERSION"

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-rv1106.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DIPC_APP_VERSION="$APP_VERSION"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "OK: $BUILD_DIR/ipc_app"
file "$BUILD_DIR/ipc_app"
