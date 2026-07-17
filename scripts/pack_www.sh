#!/bin/bash
# Build web-ui and stage into www/ for board deploy (/userdata/www or /oem/www).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/web-ui"
if [[ -f package-lock.json ]]; then
  npm install
else
  npm install
fi
npm run build
rm -rf "$ROOT/www/dist"
mkdir -p "$ROOT/www"
rm -rf "$ROOT/www/assets" "$ROOT/www/index.html"
cp -a dist/. "$ROOT/www/"
# gzip size check
SIZE=$(du -sb "$ROOT/www" | awk '{print $1}')
echo "www bytes=$SIZE"
if [ "$SIZE" -gt 1572864 ]; then
  echo "WARN: www > 1.5MB uncompressed; check Element Plus tree-shaking"
fi
echo "OK: staged $ROOT/www"
