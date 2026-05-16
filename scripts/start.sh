#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STATE_DIR="/tmp/edge-runtime"
mkdir -p "$STATE_DIR"

MODEL_PATH="${EDGE_MODEL_PATH:-$ROOT/models/Phi-3-mini-4k-instruct-q4.gguf}"
WORKER_COUNT="${EDGE_WORKER_COUNT:-2}"
API_PORT="${EDGE_API_PORT:-11434}"
SHELL_PORT="${EDGE_SHELL_PORT:-3000}"

SUPERVISOR_BIN="$ROOT/build/supervisor/edge-supervisor"
MODEL_CACHE_BIN="$ROOT/build/model-cache/edge-model-cache"
WORKER_BIN="$ROOT/build/inference-worker/edge-inference-worker"
API_ENTRY="$ROOT/api-server/server.js"
SHELL_ENTRY="$ROOT/shell-app/server.js"

if [[ ! -x "$SUPERVISOR_BIN" ]]; then
  echo "[start] missing supervisor binary: $SUPERVISOR_BIN"
  echo "[start] run scripts/build.sh first"
  exit 1
fi
if [[ ! -x "$MODEL_CACHE_BIN" ]]; then
  echo "[start] missing model-cache binary: $MODEL_CACHE_BIN"
  echo "[start] run scripts/build.sh first"
  exit 1
fi
if [[ ! -x "$WORKER_BIN" ]]; then
  echo "[start] missing inference-worker binary: $WORKER_BIN"
  echo "[start] run scripts/build.sh first"
  exit 1
fi
if [[ ! -f "$API_ENTRY" ]]; then
  echo "[start] missing api entry file: $API_ENTRY"
  exit 1
fi
if [[ ! -f "$SHELL_ENTRY" ]]; then
  echo "[start] missing shell entry file: $SHELL_ENTRY"
  exit 1
fi
if [[ ! -f "$MODEL_PATH" ]]; then
  echo "[start] missing model file: $MODEL_PATH"
  exit 1
fi

echo "[start] launching edge-supervisor..."
(
  export EDGE_WORKER_COUNT="$WORKER_COUNT"
  export EDGE_API_PORT="$API_PORT"
  "$SUPERVISOR_BIN" \
    --model-path "$MODEL_PATH" \
    --worker-count "$WORKER_COUNT" \
    --model-cache-bin "$MODEL_CACHE_BIN" \
    --worker-bin "$WORKER_BIN" \
    --api-bin "node" \
    --api-entry "$API_ENTRY" \
    --shm-name "/edge-model-weights" \
    --supervisor-socket "/tmp/edge-supervisor.sock" \
    --api-notify-socket "/tmp/edge-api-notify.sock"
) &
SUPERVISOR_PID=$!
echo "$SUPERVISOR_PID" > "$STATE_DIR/supervisor.pid"

sleep 2

echo "[start] launching shell-app..."
(
  export EDGE_SHELL_PORT="$SHELL_PORT"
  export EDGE_API_BASE="http://127.0.0.1:$API_PORT"
  node "$SHELL_ENTRY"
) &
SHELL_PID=$!
echo "$SHELL_PID" > "$STATE_DIR/shell.pid"

echo ""
echo "Edge Runtime running."
echo "  API server:   http://127.0.0.1:$API_PORT"
echo "  Shell app:    http://127.0.0.1:$SHELL_PORT"
echo "  Doc Q&A MFE:  http://127.0.0.1:$SHELL_PORT/mfe-doc-qa"
echo "  Meeting MFE:  http://127.0.0.1:$SHELL_PORT/mfe-meeting-summary"
echo ""
echo "Use scripts/stop.sh to stop all runtime processes."
