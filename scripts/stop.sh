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

load_env_file "$ROOT/.env.example"
ENV_FILE="$ROOT/.env"
load_env_file "$ENV_FILE"

STATE_DIR="$EDGE_STATE_DIR"
API_PORT="$EDGE_API_PORT"
SHELL_PORT="$EDGE_SHELL_PORT"
STATUS_DASHBOARD_PORT="$EDGE_STATUS_DASHBOARD_PORT"
MEETING_MFE_PORT="$EDGE_MEETING_MFE_PORT"
DOC_QA_MFE_PORT="$EDGE_DOC_QA_MFE_PORT"
SHM_NAME="$EDGE_SHM_NAME"
SUPERVISOR_SOCK="$EDGE_SUPERVISOR_SOCK"
API_NOTIFY_SOCK="$EDGE_API_NOTIFY_SOCK"
WORKER_SOCKET_PREFIX="$EDGE_WORKER_SOCKET_PREFIX"

stop_from_pidfile() {
  local label="$1"
  local pidfile="$2"

  if [[ ! -f "$pidfile" ]]; then
    return
  fi

  local pid
  pid="$(cat "$pidfile")"
  if [[ -z "${pid}" ]]; then
    rm -f "$pidfile"
    return
  fi

  if kill -0 "$pid" 2>/dev/null; then
    echo "[stop] stopping $label (pid $pid)..."
    kill "$pid" 2>/dev/null || true
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
  fi

  rm -f "$pidfile"
}

find_runtime_pids() {
  ps -eo pid=,cmd= | awk -v root="$ROOT" '
    index($0, root "/build/supervisor/edge-supervisor") > 0 ||
    index($0, root "/build/model-cache/edge-model-cache") > 0 ||
    index($0, root "/build/inference-worker/edge-inference-worker") > 0 ||
	    index($0, "node " root "/api-server/server.js") > 0 ||
	    index($0, "node " root "/shell-app/server.js") > 0 ||
	    index($0, "node " root "/system-dashboard/server.js") > 0 ||
	    index($0, "node " root "/mfes/meeting-summary/server.js") > 0 ||
	    index($0, "node " root "/mfes/document-qa/server.js") > 0 ||
	    index($0, "node api-server/server.js") > 0 ||
	    index($0, "node ./api-server/server.js") > 0 ||
	    index($0, "node shell-app/server.js") > 0 ||
	    index($0, "node ./shell-app/server.js") > 0 ||
	    index($0, "node system-dashboard/server.js") > 0 ||
	    index($0, "node ./system-dashboard/server.js") > 0 ||
	    index($0, "node mfes/meeting-summary/server.js") > 0 ||
	    index($0, "node ./mfes/meeting-summary/server.js") > 0 ||
	    index($0, "node mfes/document-qa/server.js") > 0 ||
	    index($0, "node ./mfes/document-qa/server.js") > 0 {
	      print $1
	    }
	  '
}

find_port_pids() {
  local port="$1"
  if command -v lsof >/dev/null 2>&1; then
    lsof -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null || true
    return
  fi
  if command -v ss >/dev/null 2>&1; then
    ss -ltnpH "( sport = :$port )" 2>/dev/null \
      | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' \
      | sort -u
  fi
}

kill_pid_list() {
  local signal="$1"
  shift || true
  local pids=("$@")
  if [[ "${#pids[@]}" -eq 0 ]]; then
    return
  fi
  for pid in "${pids[@]}"; do
    kill "-$signal" "$pid" 2>/dev/null || true
  done
}

stop_from_pidfile "shell-app" "$STATE_DIR/shell.pid"
stop_from_pidfile "system dashboard" "$STATE_DIR/status-dashboard.pid"
stop_from_pidfile "meeting-summary MFE" "$STATE_DIR/meeting-mfe.pid"
stop_from_pidfile "document Q&A MFE" "$STATE_DIR/doc-qa-mfe.pid"
stop_from_pidfile "edge-supervisor" "$STATE_DIR/supervisor.pid"

mapfile -t runtime_pids < <(find_runtime_pids)
if [[ "${#runtime_pids[@]}" -gt 0 ]]; then
  echo "[stop] stopping remaining runtime processes: ${runtime_pids[*]}"
  kill_pid_list TERM "${runtime_pids[@]}"
  sleep 1

  mapfile -t still_running < <(
    for pid in "${runtime_pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        echo "$pid"
      fi
    done
  )

  if [[ "${#still_running[@]}" -gt 0 ]]; then
    kill_pid_list KILL "${still_running[@]}"
  fi
fi

mapfile -t port_pids < <(
  {
	    find_port_pids "$API_PORT"
	    find_port_pids "$SHELL_PORT"
	    find_port_pids "$STATUS_DASHBOARD_PORT"
	    find_port_pids "$MEETING_MFE_PORT"
	    find_port_pids "$DOC_QA_MFE_PORT"
	  } | sort -u
)
if [[ "${#port_pids[@]}" -gt 0 ]]; then
  echo "[stop] stopping listeners on ports $API_PORT/$SHELL_PORT/$STATUS_DASHBOARD_PORT/$MEETING_MFE_PORT/$DOC_QA_MFE_PORT: ${port_pids[*]}"
  kill_pid_list TERM "${port_pids[@]}"
  sleep 1

  mapfile -t still_listening < <(
    for pid in "${port_pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        echo "$pid"
      fi
    done
  )

  if [[ "${#still_listening[@]}" -gt 0 ]]; then
    kill_pid_list KILL "${still_listening[@]}"
  fi
fi

rm -f "$SUPERVISOR_SOCK" "$API_NOTIFY_SOCK" "${WORKER_SOCKET_PREFIX}"*.sock
rm -f "/dev/shm/${SHM_NAME#/}" "/dev/shm/${SHM_NAME#/}.meta"

echo "[stop] runtime processes stopped."
