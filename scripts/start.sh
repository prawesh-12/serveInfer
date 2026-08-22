#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

load_env_file() {
  local file="$1"
  if [[ -f "$file" ]]; then
    set -a
    # shellcheck source=/dev/null
    source "$file"
    set +a
  fi
}

required_env() {
  local name="$1"
  if [[ -z "${!name:-}" ]]; then
    echo "[start] missing required environment variable: $name"
    echo "[start] copy .env.example to .env or define $name before starting."
    exit 1
  fi
}

load_env_file "$ROOT/.env.example"
ENV_FILE="$ROOT/.env"
load_env_file "$ENV_FILE"

for name in \
  EDGE_STATE_DIR \
  EDGE_MODEL_PATH \
  EDGE_WORKER_COUNT \
  EDGE_API_PORT \
  EDGE_SHELL_PORT \
  EDGE_STATUS_DASHBOARD_PORT \
  EDGE_MEETING_MFE_PORT \
  EDGE_DOC_QA_MFE_PORT \
  EDGE_FORCE_CPU \
  EDGE_LOG_LEVEL \
  EDGE_SHELL_PUBLIC_BASE \
  EDGE_API_BASE \
  EDGE_MEETING_MFE_URL \
  EDGE_DOC_QA_MFE_URL \
  EDGE_ALLOWED_MFE_ORIGINS \
  EDGE_MAX_SLOTS \
  EDGE_MAX_PER_MFE \
  EDGE_MAX_QUEUE \
  EDGE_AGING_MS \
  EDGE_QUEUE_TIMEOUT_MS \
  EDGE_DEFAULT_JOB_MS \
  EDGE_EXEC_TIMEOUT_MS \
  EDGE_DONE_TTL_MS \
  EDGE_DONE_MAX_ENTRIES \
  EDGE_WORKER_CONNECT_TIMEOUT_MS \
  EDGE_WORKER_RECOVERY_MS \
  EDGE_WORKER_RECOVERY_ATTEMPTS \
  EDGE_IDEMPOTENCY_TTL_MS \
  EDGE_INFLIGHT_PATH \
  EDGE_CLIENT_RETRY_ATTEMPTS \
  EDGE_CLIENT_RETRY_BASE_MS \
  EDGE_CLIENT_RETRY_MAX_MS \
  EDGE_DEVICE_LADDER \
  EDGE_DEVICE_QUARANTINE_MS \
  EDGE_DEVICE_PROBE_INTERVAL_MS \
  EDGE_SHM_NAME \
  EDGE_SUPERVISOR_SOCK \
  EDGE_API_NOTIFY_SOCK \
  EDGE_WORKER_SOCKET_PREFIX \
  EDGE_CRASH_LOG \
  EDGE_MODEL_CONFIG_PATH
do
  required_env "$name"
done

STATE_DIR="$EDGE_STATE_DIR"
mkdir -p "$STATE_DIR"

MODEL_PATH="$EDGE_MODEL_PATH"
WORKER_COUNT="$EDGE_WORKER_COUNT"
API_PORT="$EDGE_API_PORT"
SHELL_PORT="$EDGE_SHELL_PORT"
STATUS_DASHBOARD_PORT="$EDGE_STATUS_DASHBOARD_PORT"
MEETING_MFE_PORT="$EDGE_MEETING_MFE_PORT"
DOC_QA_MFE_PORT="$EDGE_DOC_QA_MFE_PORT"
FORCE_CPU="$EDGE_FORCE_CPU"
LOG_LEVEL="$EDGE_LOG_LEVEL"

if [[ "$MODEL_PATH" != /* ]]; then
  MODEL_PATH="$ROOT/${MODEL_PATH#./}"
fi

port_in_use() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -ltnH "( sport = :$port )" | grep -q .
    return $?
  fi
  if command -v lsof >/dev/null 2>&1; then
    lsof -iTCP:"$port" -sTCP:LISTEN -t >/dev/null 2>&1
    return $?
  fi
  return 1
}

SUPERVISOR_BIN="$ROOT/build/supervisor/edge-supervisor"
MODEL_CACHE_BIN="$ROOT/build/model-cache/edge-model-cache"
WORKER_BIN="$ROOT/build/inference-worker/edge-inference-worker"
API_ENTRY="$ROOT/api-server/server.js"
SHELL_ENTRY="$ROOT/shell-app/server.js"
STATUS_DASHBOARD_ENTRY="$ROOT/system-dashboard/server.js"
MEETING_MFE_ENTRY="$ROOT/mfes/meeting-summary/server.js"
DOC_QA_MFE_ENTRY="$ROOT/mfes/document-qa/server.js"
SHELL_PUBLIC_BASE="$EDGE_SHELL_PUBLIC_BASE"
MEETING_MFE_URL="$EDGE_MEETING_MFE_URL"
DOC_QA_MFE_URL="$EDGE_DOC_QA_MFE_URL"
ALLOWED_MFE_ORIGINS="$EDGE_ALLOWED_MFE_ORIGINS"
SHM_NAME="$EDGE_SHM_NAME"
SUPERVISOR_SOCK="$EDGE_SUPERVISOR_SOCK"
API_NOTIFY_SOCK="$EDGE_API_NOTIFY_SOCK"

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
if [[ ! -f "$STATUS_DASHBOARD_ENTRY" ]]; then
  echo "[start] missing system dashboard entry file: $STATUS_DASHBOARD_ENTRY"
  exit 1
fi
if [[ ! -f "$MEETING_MFE_ENTRY" ]]; then
  echo "[start] missing meeting MFE entry file: $MEETING_MFE_ENTRY"
  exit 1
fi
if [[ ! -f "$DOC_QA_MFE_ENTRY" ]]; then
  echo "[start] missing document Q&A MFE entry file: $DOC_QA_MFE_ENTRY"
  exit 1
fi
if [[ ! -f "$MODEL_PATH" ]]; then
  echo "[start] missing model file: $MODEL_PATH"
  exit 1
fi
if port_in_use "$API_PORT"; then
  echo "[start] EDGE_API_PORT=$API_PORT is already in use."
  echo "[start] run 'make stop' if an old runtime is still running, or change EDGE_API_PORT in .env."
  exit 1
fi
if port_in_use "$SHELL_PORT"; then
  echo "[start] EDGE_SHELL_PORT=$SHELL_PORT is already in use."
  echo "[start] run 'make stop' if an old runtime is still running, or change EDGE_SHELL_PORT in .env."
  exit 1
fi
if port_in_use "$STATUS_DASHBOARD_PORT"; then
  echo "[start] EDGE_STATUS_DASHBOARD_PORT=$STATUS_DASHBOARD_PORT is already in use."
  echo "[start] run 'make stop' if an old runtime is still running, or change EDGE_STATUS_DASHBOARD_PORT in .env."
  exit 1
fi
if port_in_use "$MEETING_MFE_PORT"; then
  echo "[start] EDGE_MEETING_MFE_PORT=$MEETING_MFE_PORT is already in use."
  echo "[start] run 'make stop' if an old runtime is still running, or change EDGE_MEETING_MFE_PORT in .env."
  exit 1
fi
if port_in_use "$DOC_QA_MFE_PORT"; then
  echo "[start] EDGE_DOC_QA_MFE_PORT=$DOC_QA_MFE_PORT is already in use."
  echo "[start] run 'make stop' if an old runtime is still running, or change EDGE_DOC_QA_MFE_PORT in .env."
  exit 1
fi

echo "[start] launching edge-supervisor..."
(
  export EDGE_MODEL_PATH="$MODEL_PATH"
  export EDGE_WORKER_COUNT="$WORKER_COUNT"
  export EDGE_API_PORT="$API_PORT"
  export EDGE_FORCE_CPU="$FORCE_CPU"
  export EDGE_LOG_LEVEL="$LOG_LEVEL"
  export EDGE_WORKER_SOCKET_PREFIX="$EDGE_WORKER_SOCKET_PREFIX"
  export EDGE_CRASH_LOG="$EDGE_CRASH_LOG"
  export EDGE_MODEL_CONFIG_PATH="$EDGE_MODEL_CONFIG_PATH"
  "$SUPERVISOR_BIN" \
    --model-path "$MODEL_PATH" \
    --worker-count "$WORKER_COUNT" \
    --model-cache-bin "$MODEL_CACHE_BIN" \
    --worker-bin "$WORKER_BIN" \
    --api-bin "node" \
    --api-entry "$API_ENTRY" \
    --shm-name "$SHM_NAME" \
    --supervisor-socket "$SUPERVISOR_SOCK" \
    --api-notify-socket "$API_NOTIFY_SOCK"
) &
SUPERVISOR_PID=$!
echo "$SUPERVISOR_PID" > "$STATE_DIR/supervisor.pid"

sleep 2

echo "[start] launching shell-app..."
(
  export EDGE_SHELL_PORT="$SHELL_PORT"
  export EDGE_API_BASE="$EDGE_API_BASE"
  export EDGE_WORKER_COUNT="$WORKER_COUNT"
  export EDGE_MAX_SLOTS="$EDGE_MAX_SLOTS"
  export EDGE_MAX_PER_MFE="$EDGE_MAX_PER_MFE"
  export EDGE_EXEC_TIMEOUT_MS="$EDGE_EXEC_TIMEOUT_MS"
  export EDGE_DONE_TTL_MS="$EDGE_DONE_TTL_MS"
  export EDGE_DONE_MAX_ENTRIES="$EDGE_DONE_MAX_ENTRIES"
  export EDGE_ALLOWED_MFE_ORIGINS="$ALLOWED_MFE_ORIGINS"
  export EDGE_LOG_LEVEL="$LOG_LEVEL"
  node "$SHELL_ENTRY"
) &
SHELL_PID=$!
echo "$SHELL_PID" > "$STATE_DIR/shell.pid"

echo "[start] launching system dashboard..."
(
  export EDGE_STATUS_DASHBOARD_PORT="$STATUS_DASHBOARD_PORT"
  export EDGE_SHELL_PUBLIC_BASE="$SHELL_PUBLIC_BASE"
  export EDGE_API_BASE="$EDGE_API_BASE"
  export EDGE_MEETING_MFE_URL="$MEETING_MFE_URL"
  export EDGE_DOC_QA_MFE_URL="$DOC_QA_MFE_URL"
  export EDGE_STATE_DIR="$STATE_DIR"
  node "$STATUS_DASHBOARD_ENTRY"
) &
STATUS_DASHBOARD_PID=$!
echo "$STATUS_DASHBOARD_PID" > "$STATE_DIR/status-dashboard.pid"

echo "[start] launching meeting-summary MFE..."
(
  export EDGE_MEETING_MFE_PORT="$MEETING_MFE_PORT"
  export EDGE_SHELL_PUBLIC_BASE="$SHELL_PUBLIC_BASE"
  export EDGE_CLIENT_RETRY_ATTEMPTS="$EDGE_CLIENT_RETRY_ATTEMPTS"
  export EDGE_CLIENT_RETRY_BASE_MS="$EDGE_CLIENT_RETRY_BASE_MS"
  export EDGE_CLIENT_RETRY_MAX_MS="$EDGE_CLIENT_RETRY_MAX_MS"
  node "$MEETING_MFE_ENTRY"
) &
MEETING_MFE_PID=$!
echo "$MEETING_MFE_PID" > "$STATE_DIR/meeting-mfe.pid"

echo "[start] launching document Q&A MFE..."
(
  export EDGE_DOC_QA_MFE_PORT="$DOC_QA_MFE_PORT"
  export EDGE_SHELL_PUBLIC_BASE="$SHELL_PUBLIC_BASE"
  export EDGE_CLIENT_RETRY_ATTEMPTS="$EDGE_CLIENT_RETRY_ATTEMPTS"
  export EDGE_CLIENT_RETRY_BASE_MS="$EDGE_CLIENT_RETRY_BASE_MS"
  export EDGE_CLIENT_RETRY_MAX_MS="$EDGE_CLIENT_RETRY_MAX_MS"
  node "$DOC_QA_MFE_ENTRY"
) &
DOC_QA_MFE_PID=$!
echo "$DOC_QA_MFE_PID" > "$STATE_DIR/doc-qa-mfe.pid"

echo ""
echo "Edge Runtime running."
echo "  API server:   http://127.0.0.1:$API_PORT"
echo "  Shell app:    http://127.0.0.1:$SHELL_PORT"
echo "  Status UI:    http://127.0.0.1:$STATUS_DASHBOARD_PORT"
echo "  Meeting MFE:  $MEETING_MFE_URL"
echo "  Doc Q&A MFE:  $DOC_QA_MFE_URL"
echo "  Force CPU:    $FORCE_CPU"
echo "  Log level:    $LOG_LEVEL"
echo ""
echo "Use scripts/stop.sh to stop all runtime processes."
