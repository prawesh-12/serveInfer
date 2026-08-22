#!/bin/bash
set -euo pipefail

TIER=clients
source "$(cd "$(dirname "$0")" && pwd)/lib.sh"
load_env

ACTION="${1:-start}"
CHAT_PORT_VARS=(EDGE_CHAT_ALL_PORT EDGE_CHAT_1_PORT EDGE_CHAT_2_PORT EDGE_CHAT_3_PORT EDGE_CHAT_4_PORT EDGE_CHAT_5_PORT)

if [[ "$ACTION" == "stop" ]]; then
  stop_glob "$EDGE_STATE_DIR/client-*.pid"
  free_port "${EDGE_CHAT_ALL_PORT:-5000}"
  for n in 1 2 3 4 5; do
    var="EDGE_CHAT_${n}_PORT"
    free_port "${!var:-$((5000 + n))}"
  done
  echo "[clients] stopped"
  exit 0
fi

require_env EDGE_STATE_DIR EDGE_SHELL_PUBLIC_BASE "${CHAT_PORT_VARS[@]}"
require_free_port "$EDGE_CHAT_ALL_PORT" "all"
for n in 1 2 3 4 5; do
  var="EDGE_CHAT_${n}_PORT"
  require_free_port "${!var}" "chat_$n"
done
mkdir -p "$EDGE_STATE_DIR" "$EDGE_LOG_DIR"

start_client() {
  local name="$1" entry="$2" port="$3"
  [[ -f "$entry" ]] || { echo "[clients] missing entry file: $entry" >&2; exit 1; }
  echo "[clients] starting $name on :$port"
  CLIENT_PORT="$port" \
  SHELL_API_BASE="$EDGE_SHELL_PUBLIC_BASE" \
  CLIENT_RETRY_ATTEMPTS="${EDGE_CLIENT_RETRY_ATTEMPTS:-3}" \
  CLIENT_RETRY_BASE_MS="${EDGE_CLIENT_RETRY_BASE_MS:-500}" \
  CLIENT_RETRY_MAX_MS="${EDGE_CLIENT_RETRY_MAX_MS:-8000}" \
    spawn "client-$name" node "$entry" > /dev/null
}

start_client "all" "$ROOT/clients/all/server.js" "$EDGE_CHAT_ALL_PORT"
for n in 1 2 3 4 5; do
  var="EDGE_CHAT_${n}_PORT"
  start_client "chat_$n" "$ROOT/clients/chat_$n/server.js" "${!var}"
done

echo ""
echo "  all five:  http://127.0.0.1:$EDGE_CHAT_ALL_PORT"
for n in 1 2 3 4 5; do
  var="EDGE_CHAT_${n}_PORT"
  echo "  chat_$n:  http://127.0.0.1:${!var}"
done
echo "  talking to:  $EDGE_SHELL_PUBLIC_BASE"
echo ""
