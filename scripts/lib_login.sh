#!/bin/sh
# Shared login helper for board smoke tests.
# Usage: . scripts/lib_login.sh && ipc_login "$BASE"
# Env: IPC_PASS (default admin), IPC_USER (default admin)

ipc_login() {
  BASE="$1"
  USER="${IPC_USER:-admin}"
  PASS="${IPC_PASS:-admin}"
  ALT_PASS="${IPC_ALT_PASS:-admin1}"

  printf '%s' "{\"username\":\"$USER\",\"password\":\"$PASS\"}" > /tmp/login_ok.json
  wget -qO /tmp/login_resp.json --post-file=/tmp/login_ok.json \
    --header='Content-Type: application/json' "$BASE/api/v1/auth/login" || true

  MUST=$(sed -n 's/.*"must_change":\([^,}]*\).*/\1/p' /tmp/login_resp.json)
  TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/login_resp.json)

  if [ "$MUST" = "true" ] && [ -n "$TOKEN" ]; then
    printf '%s' "{\"old_password\":\"$PASS\",\"new_password\":\"$ALT_PASS\"}" > /tmp/pw.json
    wget -qO- --post-file=/tmp/pw.json --header='Content-Type: application/json' \
      --header="Authorization: Bearer $TOKEN" \
      "$BASE/api/v1/auth/password" >/dev/null || true
    PASS="$ALT_PASS"
    printf '%s' "{\"username\":\"$USER\",\"password\":\"$PASS\"}" > /tmp/login_ok.json
    wget -qO /tmp/login_resp.json --post-file=/tmp/login_ok.json \
      --header='Content-Type: application/json' "$BASE/api/v1/auth/login" || true
    TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/login_resp.json)
  fi

  if [ -z "$TOKEN" ] && [ "$PASS" = "admin" ]; then
    PASS="$ALT_PASS"
    printf '%s' "{\"username\":\"$USER\",\"password\":\"$PASS\"}" > /tmp/login_ok.json
    wget -qO /tmp/login_resp.json --post-file=/tmp/login_ok.json \
      --header='Content-Type: application/json' "$BASE/api/v1/auth/login" || true
    TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/login_resp.json)
  fi

  if [ -z "$TOKEN" ]; then
    echo "FAIL: login failed (set IPC_PASS if password was changed)" >&2
    return 1
  fi
  IPC_AUTH="Authorization: Bearer $TOKEN"
  export IPC_AUTH IPC_PASS
  return 0
}
