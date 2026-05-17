#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STATE_DIR="/tmp/edge-runtime"

ENV_FILE="$ROOT/.env"
if [[ -f "$ENV_FILE" ]]; then
  set -a
  # shellcheck source=/dev/null
  source "$ENV_FILE"
  set +a
fi

API_PORT="${EDGE_API_PORT:-11434}"
SHELL_PORT="${EDGE_SHELL_PORT:-3000}"

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
    index($0, "node api-server/server.js") > 0 ||
    index($0, "node ./api-server/server.js") > 0 ||
    index($0, "node shell-app/server.js") > 0 ||
    index($0, "node ./shell-app/server.js") > 0 {
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
  } | sort -u
)
if [[ "${#port_pids[@]}" -gt 0 ]]; then
  echo "[stop] stopping listeners on ports $API_PORT/$SHELL_PORT: ${port_pids[*]}"
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

rm -f /tmp/edge-supervisor.sock /tmp/edge-api-notify.sock /tmp/edge-worker-*.sock
rm -f /dev/shm/edge-model-weights /dev/shm/edge-model-weights.meta

echo "[stop] runtime processes stopped."
