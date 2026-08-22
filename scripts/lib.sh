# Shared helpers for the three tier scripts. Source this, do not run it.
#
# Every process registers itself as $EDGE_STATE_DIR/<tier>-<name>.pid. That
# directory is the process list the dashboard reads, and the glob each tier's
# stop uses so it never touches another tier's processes.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

load_env() {
  local file
  for file in "$ROOT/.env.example" "$ROOT/.env"; do
    if [[ -f "$file" ]]; then
      set -a
      # shellcheck source=/dev/null
      source "$file"
      set +a
    fi
  done
}

require_env() {
  local name
  for name in "$@"; do
    if [[ -z "${!name:-}" ]]; then
      echo "[$TIER] missing required environment variable: $name" >&2
      echo "[$TIER] copy .env.example to .env, or define $name before starting." >&2
      exit 1
    fi
  done
}

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

require_free_port() {
  local port="$1" label="$2"
  if port_in_use "$port"; then
    echo "[$TIER] $label port $port is already in use." >&2
    echo "[$TIER] run 'make $TIER-stop' first, or change it in .env." >&2
    exit 1
  fi
}

register() {
  local name="$1" pid="$2"
  mkdir -p "$EDGE_STATE_DIR"
  echo "$pid" > "$EDGE_STATE_DIR/$name.pid"
}

# The redirect matters. A child inheriting the script's stdout holds that pipe
# open, so `make backend | tail` never returns.
spawn() {
  local name="$1"
  shift
  mkdir -p "$EDGE_STATE_DIR"
  local logfile="$EDGE_STATE_DIR/$name.log"
  "$@" > "$logfile" 2>&1 &
  local pid=$!
  register "$name" "$pid"
  disown "$pid" 2>/dev/null || true
  echo "$pid"
}

stop_pidfile() {
  local pidfile="$1"
  [[ -f "$pidfile" ]] || return 0

  local pid label
  pid="$(cat "$pidfile" 2>/dev/null || true)"
  label="$(basename "$pidfile" .pid)"

  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    echo "[$TIER] stopping $label (pid $pid)"
    kill "$pid" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.2
    done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
  fi

  rm -f "$pidfile"
}

stop_glob() {
  local pattern="$1" found=0 pidfile
  for pidfile in $pattern; do
    [[ -e "$pidfile" ]] || continue
    found=1
    stop_pidfile "$pidfile"
  done
  if [[ "$found" -eq 0 ]]; then
    echo "[$TIER] nothing running"
  fi
}

# Covers a process started by hand, or one whose pidfile was lost.
free_port() {
  local port="$1"
  local pids=()
  if command -v lsof >/dev/null 2>&1; then
    mapfile -t pids < <(lsof -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null || true)
  elif command -v ss >/dev/null 2>&1; then
    mapfile -t pids < <(ss -ltnpH "( sport = :$port )" 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u)
  fi
  [[ "${#pids[@]}" -eq 0 ]] && return 0
  echo "[$TIER] freeing port $port (${pids[*]})"
  local pid
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  sleep 0.5
  for pid in "${pids[@]}"; do kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true; done
}
