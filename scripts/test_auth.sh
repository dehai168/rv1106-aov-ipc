#!/bin/sh
set -e
printf '%s' '{"username":"admin","password":"wrong"}' > /tmp/login_bad.json
printf '%s' '{"username":"admin","password":"admin"}' > /tmp/login_ok.json

echo "=== me unauthorized ==="
wget -qO- http://127.0.0.1:8080/api/v1/auth/me || true
echo

echo "=== login bad ==="
wget -qO- --post-file=/tmp/login_bad.json --header='Content-Type: application/json' \
  http://127.0.0.1:8080/api/v1/auth/login || true
echo

echo "=== login ok ==="
wget -qO- --post-file=/tmp/login_ok.json --header='Content-Type: application/json' \
  http://127.0.0.1:8080/api/v1/auth/login > /tmp/login_resp.json
cat /tmp/login_resp.json
echo

TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/login_resp.json)
echo "=== me with bearer ==="
wget -qO- --header="Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/auth/me || true
echo

echo "=== index ==="
wget -qO- http://127.0.0.1:8080/index.html | head -c 100
echo
echo OK
