#!/bin/bash
set -euo pipefail

STATE_DIR="/tmp/edge-runtime"

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

stop_from_pidfile "shell-app" "$STATE_DIR/shell.pid"
stop_from_pidfile "edge-supervisor" "$STATE_DIR/supervisor.pid"

rm -f /tmp/edge-supervisor.sock /tmp/edge-api-notify.sock /tmp/edge-worker-*.sock
rm -f /dev/shm/edge-model-weights

echo "[stop] runtime processes stopped."
