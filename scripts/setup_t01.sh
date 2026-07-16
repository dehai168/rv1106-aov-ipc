#!/bin/bash
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

SDK=/home/user/work/luckfox-pico
TOOL=$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
export PATH=$TOOL/bin:$PATH

echo "=== toolchain ==="
which arm-rockchip830-linux-uclibcgnueabihf-gcc
arm-rockchip830-linux-uclibcgnueabihf-gcc --version | head -2

echo "=== ensure bashrc PATH ==="
# keep single correct PATH line
sed -i '/arm-rockchip830-linux-uclibcgnueabihf\/bin/d' "$HOME/.bashrc" 2>/dev/null || true
echo "export PATH=$TOOL/bin:\$PATH" >> "$HOME/.bashrc"
touch "$HOME/.bash_profile"
sed -i '/arm-rockchip830-linux-uclibcgnueabihf\/bin/d' "$HOME/.bash_profile"
echo "export PATH=$TOOL/bin:\$PATH" >> "$HOME/.bash_profile"

echo "=== media samples tree ==="
ls "$SDK/media/samples"
ls "$SDK/media/samples/simple_test" 2>/dev/null || true
find "$SDK/media/samples" -maxdepth 2 -type f -name 'Makefile*' | head -30

echo "=== compile hello ==="
WORKDIR=/tmp/rv1106_hello
mkdir -p "$WORKDIR"
cat > "$WORKDIR/hello.c" <<'EOF'
#include <stdio.h>
#include <unistd.h>
int main(void) {
    printf("rv1106-aov-ipc T0.1 hello from cross-compile\n");
    fflush(stdout);
    return 0;
}
EOF

arm-rockchip830-linux-uclibcgnueabihf-gcc -Wall -O2 "$WORKDIR/hello.c" -o "$WORKDIR/hello_rv1106"
file "$WORKDIR/hello_rv1106"
ls -l "$WORKDIR/hello_rv1106"

# copy to Windows-visible path for adb push
OUT=/mnt/d/CodeProject/rv1106-aov-ipc/build
mkdir -p "$OUT"
cp "$WORKDIR/hello_rv1106" "$OUT/hello_rv1106"
echo "OUT=$OUT/hello_rv1106"
echo "T0.1 toolchain+hello OK"
