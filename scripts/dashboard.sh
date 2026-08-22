#!/bin/bash
# The operator view. Reads the pidfile registry in $EDGE_STATE_DIR and the two
# health endpoints, so it runs on the same host but starts and stops on its own.
set -euo pipefail

TIER=dashboard
source "$(cd "$(dirname "$0")" && pwd)/lib.sh"
load_env

ACTION="${1:-start}"

if [[ "$ACTION" == "stop" ]]; then
  stop_glob "$EDGE_STATE_DIR/dashboard.pid"
  free_port "${EDGE_STATUS_DASHBOARD_PORT:-3001}"
  echo "[dashboard] stopped"
  exit 0
fi

require_env EDGE_STATE_DIR EDGE_STATUS_DASHBOARD_PORT EDGE_SHELL_PUBLIC_BASE EDGE_API_BASE
require_free_port "$EDGE_STATUS_DASHBOARD_PORT" "dashboard"
mkdir -p "$EDGE_STATE_DIR"

ENTRY="$ROOT/dashboard/server.js"
[[ -f "$ENTRY" ]] || { echo "[dashboard] missing entry file: $ENTRY" >&2; exit 1; }

echo "[dashboard] starting on :$EDGE_STATUS_DASHBOARD_PORT"
DASHBOARD_PORT="$EDGE_STATUS_DASHBOARD_PORT" \
SHELL_API_BASE="$EDGE_SHELL_PUBLIC_BASE" \
AGENT_API_BASE="$EDGE_API_BASE" \
  spawn "dashboard" node "$ENTRY" > /dev/null

echo ""
echo "  dashboard:  http://127.0.0.1:$EDGE_STATUS_DASHBOARD_PORT"
echo ""
