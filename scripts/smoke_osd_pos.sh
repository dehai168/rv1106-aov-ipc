#!/bin/sh
set -e
export LD_LIBRARY_PATH=/oem/usr/lib
export IPC_PASS=123456
. /userdata/lib_login.sh
ipc_login http://127.0.0.1:8080 || exit 1

for y in 0 64 160 320; do
  printf '{"enabled":true,"show_time":true,"show_name":true,"name":"Y测试","pos_x":16,"pos_y":%s,"apply":true}' "$y" > /tmp/osd.json
  wget -qO- --header="$IPC_AUTH" --header='Content-Type: application/json' --post-file=/tmp/osd.json \
    http://127.0.0.1:8080/api/v1/video/osd
  echo
  sleep 1
done
grep 'osd.*pos=' /userdata/log/ipc_app.log | tail -8
