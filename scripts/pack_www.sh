#!/bin/bash
# Build web-ui and stage into www/ for board deploy (/userdata/www or /oem/www).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/web-ui"
npm ci
npm run build
rm -rf "$ROOT/www/dist"
mkdir -p "$ROOT/www"
rm -rf "$ROOT/www/assets" "$ROOT/www/index.html"
cp -a dist/. "$ROOT/www/"
# gzip size check
SIZE=$(du -sb "$ROOT/www" | awk '{print $1}')
echo "www bytes=$SIZE"
if [ "$SIZE" -gt 2097152 ]; then
  echo "WARN: www > 2MB uncompressed; consider further trimming"
fi
echo "OK: staged $ROOT/www"
