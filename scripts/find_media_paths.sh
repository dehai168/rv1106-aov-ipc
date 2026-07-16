#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
SDK=/home/user/work/luckfox-pico
echo "=== media/out ==="
ls "$SDK/media/out" 2>/dev/null || echo none
ls "$SDK/media/out/lib" 2>/dev/null | head -20 || true
echo "=== output/out/media_out ==="
ls "$SDK/output/out/media_out" 2>/dev/null || echo none
ls "$SDK/output/out/media_out/lib" 2>/dev/null | head -20 || true
echo "=== headers ==="
find "$SDK/media/out" "$SDK/output/out/media_out" -name 'rk_mpi_vi.h' 2>/dev/null
find "$SDK/media/out" "$SDK/output/out/media_out" -name 'rk_aiq_user_api2_sysctl.h' 2>/dev/null | head
find "$SDK/media" -path '*include*' -name 'rk_mpi_vi.h' 2>/dev/null | head
