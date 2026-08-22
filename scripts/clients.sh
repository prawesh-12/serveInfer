#!/bin/bash
set -euo pipefail

TIER=clients
source "$(cd "$(dirname "$0")" && pwd)/lib.sh"
load_env

ACTION="${1:-start}"

if [[ "$ACTION" == "stop" ]]; then
  stop_glob "$EDGE_STATE_DIR/client-*.pid"
  free_port "${EDGE_MEETING_MFE_PORT:-5001}"
  free_port "${EDGE_DOC_QA_MFE_PORT:-5002}"
  echo "[clients] stopped"
  exit 0
fi

require_env EDGE_STATE_DIR EDGE_SHELL_PUBLIC_BASE EDGE_MEETING_MFE_PORT EDGE_DOC_QA_MFE_PORT
require_free_port "$EDGE_MEETING_MFE_PORT" "meeting-summary"
require_free_port "$EDGE_DOC_QA_MFE_PORT" "document-qa"
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

start_client "meeting-summary" "$ROOT/clients/meeting-summary/server.js" "$EDGE_MEETING_MFE_PORT"
start_client "document-qa" "$ROOT/clients/document-qa/server.js" "$EDGE_DOC_QA_MFE_PORT"

echo ""
echo "  meeting summary:  http://127.0.0.1:$EDGE_MEETING_MFE_PORT"
echo "  document Q&A:     http://127.0.0.1:$EDGE_DOC_QA_MFE_PORT"
echo "  talking to:       $EDGE_SHELL_PUBLIC_BASE"
echo ""
