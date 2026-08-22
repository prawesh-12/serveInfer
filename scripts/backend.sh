#!/bin/bash
set -euo pipefail

TIER=backend
source "$(cd "$(dirname "$0")" && pwd)/lib.sh"
load_env

ACTION="${1:-start}"

if [[ "$ACTION" == "stop" ]]; then
  stop_glob "$EDGE_STATE_DIR/backend-*.pid"
  free_port "$EDGE_API_PORT"
  free_port "$EDGE_SHELL_PORT"
  rm -f "$EDGE_SUPERVISOR_SOCK" "$EDGE_API_NOTIFY_SOCK" "${EDGE_WORKER_SOCKET_PREFIX}"*.sock
  rm -f "/dev/shm/${EDGE_SHM_NAME#/}" "/dev/shm/${EDGE_SHM_NAME#/}.meta"
  echo "[backend] stopped"
  exit 0
fi

require_env \
  EDGE_STATE_DIR EDGE_MODEL_PATH EDGE_WORKER_COUNT EDGE_API_PORT EDGE_SHELL_PORT \
  EDGE_FORCE_CPU EDGE_LOG_LEVEL EDGE_API_BASE EDGE_ALLOWED_MFE_ORIGINS \
  EDGE_MAX_SLOTS EDGE_MAX_PER_MFE EDGE_MAX_QUEUE EDGE_AGING_MS \
  EDGE_QUEUE_TIMEOUT_MS EDGE_DEFAULT_JOB_MS EDGE_EXEC_TIMEOUT_MS \
  EDGE_DONE_TTL_MS EDGE_DONE_MAX_ENTRIES EDGE_WORKER_CONNECT_TIMEOUT_MS \
  EDGE_WORKER_RECOVERY_MS EDGE_WORKER_RECOVERY_ATTEMPTS EDGE_WORKER_STARTUP_GRACE_MS \
  EDGE_IDEMPOTENCY_TTL_MS EDGE_INFLIGHT_PATH \
  EDGE_DEVICE_LADDER EDGE_DEVICE_QUARANTINE_MS EDGE_DEVICE_PROBE_INTERVAL_MS \
  EDGE_SHM_NAME EDGE_SUPERVISOR_SOCK EDGE_API_NOTIFY_SOCK EDGE_WORKER_SOCKET_PREFIX \
  EDGE_CRASH_LOG EDGE_MODEL_CONFIG_PATH

MODEL_PATH="$EDGE_MODEL_PATH"
[[ "$MODEL_PATH" != /* ]] && MODEL_PATH="$ROOT/${MODEL_PATH#./}"

SUPERVISOR_BIN="$ROOT/build/supervisor/edge-supervisor"
MODEL_CACHE_BIN="$ROOT/build/model-cache/edge-model-cache"
WORKER_BIN="$ROOT/build/inference-worker/edge-inference-worker"
API_ENTRY="$ROOT/backend/api-server/server.js"
SHELL_ENTRY="$ROOT/backend/shell-app/server.js"

for path in "$SUPERVISOR_BIN" "$MODEL_CACHE_BIN" "$WORKER_BIN"; do
  if [[ ! -x "$path" ]]; then
    echo "[backend] missing binary: $path" >&2
    echo "[backend] run 'make build' first" >&2
    exit 1
  fi
done
for path in "$API_ENTRY" "$SHELL_ENTRY"; do
  [[ -f "$path" ]] || { echo "[backend] missing entry file: $path" >&2; exit 1; }
done
if [[ ! -f "$MODEL_PATH" ]]; then
  echo "[backend] missing model file: $MODEL_PATH" >&2
  exit 1
fi

require_free_port "$EDGE_API_PORT" "api-server"
require_free_port "$EDGE_SHELL_PORT" "shell-app"
mkdir -p "$EDGE_STATE_DIR" "$EDGE_LOG_DIR"

export EDGE_MODEL_PATH="$MODEL_PATH"

echo "[backend] starting supervisor..."
spawn "backend-supervisor" \
  "$SUPERVISOR_BIN" \
  --model-path "$MODEL_PATH" \
  --worker-count "$EDGE_WORKER_COUNT" \
  --model-cache-bin "$MODEL_CACHE_BIN" \
  --worker-bin "$WORKER_BIN" \
  --api-bin "node" \
  --api-entry "$API_ENTRY" \
  --shm-name "$EDGE_SHM_NAME" \
  --supervisor-socket "$EDGE_SUPERVISOR_SOCK" \
  --api-notify-socket "$EDGE_API_NOTIFY_SOCK" > /dev/null

sleep 2

echo "[backend] starting shell-app..."
spawn "backend-shell-app" node "$SHELL_ENTRY" > /dev/null

echo ""
echo "  shell API:  http://127.0.0.1:$EDGE_SHELL_PORT"
echo "  agent API:  http://127.0.0.1:$EDGE_API_PORT"
echo "  workers:    $EDGE_WORKER_COUNT   slots: $EDGE_MAX_SLOTS   per client: $EDGE_MAX_PER_MFE"
echo "  logs:       $EDGE_LOG_DIR/backend-*.log"
echo ""
