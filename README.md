# Local multi-process inference runtime

This repository provides an on-device, multi-process inference runtime, especially for GGUF models (for example, Phi-3). It combines:

- **C++ control plane**: `supervisor`, `model-cache`, `inference-worker`
- **Node.js API plane**: `api-server` (worker-pool + HTTP/SSE)
- **Node.js shell plane**: `shell-app` (scheduler + singleton API facade)
- **Independent MFE plane**: `clients/meeting-summary` and `clients/document-qa`
- **System dashboard plane**: `dashboard` (runtime/process status UI)

It exposes local inference APIs and serves two browser UIs:

- **Document Q&A**
- **Meeting Summariser**

---

## 1) Project Structure

```text
server_infer/
├── Makefile                     # build, and start/stop each tier on its own
├── .env.example                 # one config file, read by all three tiers
│
├── backend/                     # the inference runtime. Nothing here serves a browser.
│   ├── CMakeLists.txt           # C++ entrypoint for the three native targets
│   ├── supervisor/              # C++ process supervisor
│   │   ├── main.cpp             # CLI and startup wiring
│   │   ├── supervisor.h
│   │   └── supervisor.cpp       # spawn, monitor, restart, crash notifications
│   ├── model-cache/             # C++ owner of the shared GGUF in /dev/shm
│   │   ├── main.cpp
│   │   ├── model_cache.h
│   │   └── model_cache.cpp      # GGUF load, checksum, shm management
│   ├── inference-worker/        # C++ worker, one per pool slot
│   │   ├── main.cpp
│   │   ├── worker.h / worker.cpp        # socket loop and request handling
│   │   ├── inferEngine.h / .cpp         # llama-backed generation and streaming
│   │   ├── deviceLadder.h / .cpp        # tier policy: escalate, quarantine, health gate
│   │   ├── deviceBackends.h / .cpp      # per-tier probes and vendor error codes
│   │   ├── tests/               # C++ unit tests
│   │   └── llama-src/           # vendored llama.cpp, do not edit
│   ├── api-server/              # Node agent, binds 127.0.0.1 only
│   │   ├── server.js            # bootstrap, /health, supervisor notify listener
│   │   ├── ipc.js               # WorkerPool: sockets, dispatch, crash recovery
│   │   ├── requestRegistry.js   # in-flight tracking and idempotent replay
│   │   └── routes/infer.js      # /infer and /infer/stream
│   ├── shell-app/               # Node singleton, the public API boundary
│   │   ├── server.js            # /api/* routes and SSE re-emission
│   │   ├── scheduler.js         # priority queue, fairness, timeouts, cancel
│   │   └── edgeAgentService.js  # adapter between scheduler and api-server
│   ├── ipc/paths.h              # shared IPC paths for the C++ services
│   ├── config/env.js            # env loader, throws on anything missing
│   └── models/                  # the GGUF lives here, gitignored
│
├── clients/                     # sample user apps. HTTP clients of the shell API.
│   ├── meeting-summary/         # default port 5001
│   └── document-qa/             # default port 5002
│
├── dashboard/                   # operator status page, default port 3001
│   ├── server.js                # reads the pidfile registry and two health endpoints
│   └── public/
│
├── logs/                        # one <name>.log per process, gitignored
├── docs/                        # architecture and design notes
├── tests/                       # Node unit tests
└── scripts/
    ├── lib.sh                   # shared helpers and the pidfile registry
    ├── build.sh                 # compile C++ and install backend node deps
    ├── backend.sh               # start|stop the runtime
    ├── clients.sh               # start|stop the sample apps
    ├── dashboard.sh             # start|stop the status page
    └── stop.sh                  # stop all three
```

---

## 2) Runtime architecture

### Processes

1. **`edge-supervisor`** starts and monitors all child services.
2. **`edge-model-cache`** loads model weights and keeps them in POSIX shared memory.
3. **`edge-inference-worker` x N** runs inference over Unix sockets.
4. **`api-server`** exposes `/infer` and `/infer/stream`.
5. **`shell-app`** exposes the singleton scheduler-backed app API.
6. **`dashboard`** exposes runtime/process/API status UI.
7. **Independent MFEs** run on their own ports and call only the shell singleton.

### IPC and shared resources

- Supervisor socket: `EDGE_SUPERVISOR_SOCK`
- API notify socket: `EDGE_API_NOTIFY_SOCK`
- Worker sockets: `EDGE_WORKER_SOCKET_PREFIX` plus worker id
- Shared memory name: `EDGE_SHM_NAME`
- Process logs: `EDGE_LOG_DIR`, one `<name>.log` per process (`logs/` by default)
- Crash log: `EDGE_CRASH_LOG`
- Model config snapshot: `EDGE_MODEL_CONFIG_PATH`
- Open-request registry: `EDGE_INFLIGHT_PATH` (what was in flight, readable after a crash)

Runtime paths are centralized in `.env.example` and read by `backend/ipc/paths.h`.

---

## 3) Current inference implementation

The worker uses a **thin inference wrapper (`InferEngine`)** over vendored llama APIs, but model ownership is handled by the custom Edge runtime:

- `edge-model-cache` loads the GGUF bytes once into POSIX shared memory.
- The raw shared-memory object is exposed under `/dev/shm` using `EDGE_SHM_NAME`.
- Readiness/checksum metadata is stored beside it with a `.meta` suffix.
- Workers validate the metadata, attach the shared segment, and load llama from the shared-memory GGUF path.
- Attempts GPU offload (`EDGE_GPU_LAYERS`) unless `EDGE_FORCE_CPU=1`
- Falls back to CPU if GPU model load fails
- Supports:
    - blocking generation (`generate`)
    - token streaming (`generateStreaming`)
- Sampling chain includes:
    - top-k
    - top-p
    - temperature
    - dist sampler with seed

This means worker restarts do not reopen the original model path from disk; they attach to the model-cache-owned shared GGUF object.

Qualcomm NPU and Apple ANE **backends** are not implemented in this Linux build, because the
hardware is not present. The fallback **mechanism** they need is implemented and running:
a configurable device ladder with per-tier quarantine, a health-check gate before a faulted
tier is allowed back, and `device_removed` treated as unrecoverable for the session. See
[docs/device-fallback.md](docs/device-fallback.md) for the decision trees and the escalation
order, [docs/build-matrix.md](docs/build-matrix.md) for which backends a given build can
actually contain, and [docs/scheduler.md](docs/scheduler.md) for the queueing model.

---

## 4) Scheduler behavior (shell-app)

The shell scheduler (`backend/shell-app/scheduler.js`) enforces:

- Global max concurrent slots (`EDGE_MAX_SLOTS`, shipped as 4)
- Per-MFE concurrent cap (`EDGE_MAX_PER_MFE`, shipped as 2). It must be strictly less than
  `EDGE_MAX_SLOTS` or the fairness rule does nothing, because one MFE could then hold
  every slot
- Priority queue: `high`, `normal`, `low`
- Aging boost every `EDGE_AGING_MS` (15s)
- Queue cap (`EDGE_MAX_QUEUE`, 20), over which enqueue returns 429
- Queue-wait timeout (`EDGE_QUEUE_TIMEOUT_MS`, 30s) -> 408, `phase: "queue"`
- Execution timeout (`EDGE_EXEC_TIMEOUT_MS`, 120s) -> 504, `phase: "execution"`. It aborts
  the job and releases its slot without waiting for the job to settle
- Cancellation support for queued and active requests
- Queue position + estimated wait reporting

`EDGE_MAX_SLOTS` must not exceed `EDGE_WORKER_COUNT` (also 4), or the shell admits work the
agent then rejects with `no_ready_workers`.

---

## 5) APIs

## 5.1 api-server (port `EDGE_API_PORT`, default `11434`)

### `POST /infer`

Request:

```json
{
    "prompt": "Explain transformers briefly",
    "requestId": "optional-id",
    "mfeId": "doc-qa"
}
```

Response:

```json
{
    "requestId": "id",
    "result": "generated text",
    "device": "cuda|cpu",
    "degraded": false,
    "degradedReason": null,
    "replay": false
}
```

`degradedReason` names the tier and the fault that caused a fallback, for example
`cuda:device_removed`. It is null when `degraded` is false. `replay` is true when this
`requestId` was already answered and the cached result came back instead of a second
inference run.

Headers:

- `X-Inference-Device`
- `X-Inference-Degraded`
- `X-Latency-Mode`
- `X-Degraded-Reason` (only when degraded)
- `X-Idempotent-Replay`
- `Retry-After` on a 503

### `GET /infer/stream`

Query: `prompt`, `requestId`, `mfeId`

Server-Sent Events. The api-server emits three, and only three:

- `token`
- `done`
- `error`

`queued`, `started`, `cancelled` and `timeout` are **shell-only** events. The shell parses
the api-server's stream by hand and re-emits its own, adding those four from its scheduler.
A client talking to the api-server directly will never see them. Replaying a finished
`requestId` returns one `done` with `replay: true` and no tokens, because tokens cannot be
re-sent. Replaying one that is still live returns 409 `request_in_flight`.

### `GET /health`

Returns worker status and slot usage.

## 5.2 shell-app (port `EDGE_SHELL_PORT`, default `3000`)

Service routes:

- `/` returns shell singleton metadata

App routes:

- `POST /api/infer`
- `GET /api/stream`
- `POST /api/cancel`
- `GET /api/queue-status`
- `GET /api/health`
- `GET /api/agent-health`

---

## 6) Environment variables

Defaults live in `.env.example`; `.env` can override them locally. The Node services and lifecycle scripts load `.env.example` first and then `.env`, so the code does not carry fallback port or URL literals.

```env
EDGE_STATE_DIR=/tmp/edge-runtime
EDGE_LOG_DIR=./logs
EDGE_WORKER_COUNT=4
EDGE_MODEL_PATH=./backend/models/Phi-3-mini-4k-instruct-q4.gguf
EDGE_FORCE_CPU=0
EDGE_LOG_LEVEL=info

# Ports
EDGE_API_PORT=11434
EDGE_SHELL_PORT=3000
EDGE_STATUS_DASHBOARD_PORT=3001
EDGE_MEETING_MFE_PORT=5001
EDGE_DOC_QA_MFE_PORT=5002

# Public/internal service URLs
EDGE_API_BASE=http://127.0.0.1:11434
EDGE_SHELL_PUBLIC_BASE=http://127.0.0.1:3000
EDGE_ALLOWED_MFE_ORIGINS=http://127.0.0.1:5001,http://localhost:5001,http://127.0.0.1:5002,http://localhost:5002

# Inference defaults
EDGE_MAX_TOKENS=512
EDGE_TEMPERATURE=0.8
EDGE_GPU_LAYERS=99
EDGE_SEED=42

# Scheduler limits
EDGE_MAX_SLOTS=4
EDGE_MAX_PER_MFE=2
EDGE_MAX_QUEUE=20
EDGE_AGING_MS=15000
EDGE_QUEUE_TIMEOUT_MS=30000
EDGE_DEFAULT_JOB_MS=8000

# IPC and runtime files
EDGE_SHM_NAME=/edge-model-weights
EDGE_SUPERVISOR_SOCK=/tmp/edge-supervisor.sock
EDGE_WORKER_SOCKET_PREFIX=/tmp/edge-worker-
EDGE_WORKER_CONNECT_TIMEOUT_MS=3000
EDGE_API_NOTIFY_SOCK=/tmp/edge-api-notify.sock
EDGE_CRASH_LOG=./logs/edge-crash.log
EDGE_MODEL_CONFIG_PATH=/tmp/edge-model-config.json

# Execution bounds (a running job, not just a queued one)
EDGE_EXEC_TIMEOUT_MS=120000
EDGE_DONE_TTL_MS=300000
EDGE_DONE_MAX_ENTRIES=500

# Worker recovery after a crash: probe the socket instead of trusting a timer
EDGE_WORKER_RECOVERY_MS=2000
EDGE_WORKER_RECOVERY_ATTEMPTS=10

# Replay safety
EDGE_IDEMPOTENCY_TTL_MS=300000
EDGE_INFLIGHT_PATH=/tmp/edge-inflight.json

# Client retry policy, injected into each MFE through /config.js
EDGE_CLIENT_RETRY_ATTEMPTS=3
EDGE_CLIENT_RETRY_BASE_MS=500
EDGE_CLIENT_RETRY_MAX_MS=8000

# Device fallback ladder (A3/A4). Comma-separated, highest tier first.
# Tier names this build understands: cpu cuda rocm vulkan npu directml gpu ane
# metal accelerate remote. A tier for another OS probes as wrong_platform and is
# skipped, so a Windows ladder (npu,directml,cpu) is safe to leave in place here.
EDGE_DEVICE_LADDER=cuda,cpu
EDGE_DEVICE_QUARANTINE_MS=60000
EDGE_DEVICE_PROBE_INTERVAL_MS=5000

# The remote tier sends prompts off the device, so it needs an endpoint AND an
# explicit opt-in. Both unset means the tier probes as runtime_missing and is
# skipped. Endpoint set without the opt-in probes as policy_disabled.
EDGE_REMOTE_ENDPOINT=
EDGE_REMOTE_FALLBACK_ALLOWED=0

# Exercises the fallback path without the hardware: removed | unsupported | runtime.
# Empty means no injection, which costs one getenv per generate call.
EDGE_SIMULATE_DEVICE_FAULT=
```

When changing ports, update the matching URL variables and `EDGE_ALLOWED_MFE_ORIGINS` as well.

---

## 7) Setup and run

## 7.1 System dependencies

Install base tooling:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake git curl \
  python3-pip libssl-dev \
  nodejs npm
```

For NVIDIA path:

```bash
sudo apt install -y nvidia-cuda-toolkit
```

Install Hugging Face CLI (`hf`):

```bash
pip3 install -U huggingface-hub
```

## 7.2 Download model

```bash
hf download microsoft/Phi-3-mini-4k-instruct-gguf \
  Phi-3-mini-4k-instruct-q4.gguf \
  --local-dir ./backend/models
```

## 7.3 Build + run

```bash
make run
```

Other commands:

```bash
make build

make backend        # the runtime
make clients        # the sample apps
make dashboard      # the operator status page

make backend-stop   # each tier stops on its own
make clients-stop
make dashboard-stop

make stop           # all three
make restart
```

---

## 8) Process lifecycle and reliability

### Lifecycle class of every process

| Process | Class | Count | Who owns it | Restart policy |
|---|---|---|---|---|
| `edge-supervisor` | **persistent** | 1 | `scripts/backend.sh` (background job, pidfile) | not restarted; killing it leaves the shell and MFEs running |
| `edge-model-cache` | **persistent** | 1 | supervisor | restarted on crash, behind the breaker; workers restart after it |
| `api-server` (node) | **persistent** | 1 | supervisor | restarted on crash, behind the breaker |
| `edge-inference-worker` | **pooled** | `EDGE_WORKER_COUNT`, clamped by `placeableWorkerCount` to what the machine's RAM and VRAM can pay for | supervisor | fixed-size pool, each slot restarted on crash behind the breaker |
| `shell-app` (node) | **persistent** | 1 | `scripts/backend.sh` (background job, pidfile) | not supervised |
| `dashboard` (node) | **persistent** | 1 | `scripts/dashboard.sh` (background job, pidfile) | not supervised |
| MFE static servers | **persistent** | 2 | `scripts/clients.sh` (background job, pidfile) | not supervised |

Nothing here is **on-demand**: no process is spawned per request. Workers are
pre-forked into a fixed pool precisely so a request never pays model load time.

### Mechanics

- `make run` does: stop, build, then start all three tiers
- `scripts/backend.sh` fails fast if API/shell ports are occupied
- `stop.sh` stops pidfile processes + scans and kills leftover runtime processes
- Supervisor monitors child exits and restarts workers
- Circuit breaker opens after `3` crashes in `60s` (`backend/supervisor/supervisor.h`), and the
  count is also read back from `EDGE_CRASH_LOG`, so it survives a supervisor restart
- API worker-pool tracks worker states (`starting`, `ready`, `busy`, `crashed`). A crashed
  worker returns to the pool only when a probe connects to its socket, so a restart the
  breaker suppressed never gets handed a request
- Open requests are written to `EDGE_INFLIGHT_PATH`; on boot the api-server reports what
  the previous process left in flight under `/health` -> `requests.orphanedFromPreviousRun`

### Platform assumption

This build is POSIX-only: `fork`/`execvp`, `AF_UNIX` sockets, POSIX shared memory. A crash
reason is recorded as `signal_<n>` (for example `signal_11` for a segfault), which is the
Linux analogue of a **Windows access violation**. The recovery contract is the same; only
the fault code and the process API differ.

---

## 9) Typical usage

### Quick blocking check

```bash
curl -X POST "$EDGE_API_BASE/infer" \
  -H "Content-Type: application/json" \
  -d '{"prompt":"What is 2+2?","requestId":"smoke-1","mfeId":"doc-qa"}'
```

### Quick streaming check

```bash
curl -N "$EDGE_API_BASE/infer/stream?prompt=Summarize+AI+in+3+points&requestId=s1&mfeId=meeting-summary"
```

### Open UI

- System dashboard: `http://127.0.0.1:$EDGE_STATUS_DASHBOARD_PORT`
- Meeting Summariser: `http://127.0.0.1:$EDGE_MEETING_MFE_PORT`
- Document Q&A: `http://127.0.0.1:$EDGE_DOC_QA_MFE_PORT`

---

## 10) Troubleshooting

### `worker_unavailable` / `no_ready_workers`

- Workers are still loading model or unavailable.
- Wait a few seconds and retry.
- Check `$EDGE_LOG_DIR/backend-*.log` (`logs/` by default) for worker initialization.

### `worker_crashed`

- Supervisor detected worker crash and signaled API.
- Retry after the provided `retryAfterSeconds`.

### Empty or weak output

- Tune:
    - `EDGE_TEMPERATURE`
    - `EDGE_MAX_TOKENS`
    - prompt style
- Confirm model file path and model integrity.

### Port conflict

- Use `make stop` first.
- Or change `EDGE_API_PORT` / `EDGE_SHELL_PORT` in `.env`.

### CPU fallback

- If GPU load fails, engine falls back to CPU automatically.
- Force CPU mode explicitly with `EDGE_FORCE_CPU=1`.

---

## 11) Implementation notes

- `model-cache` writes a 256-byte header (`SharedModelHeader`) with:
    - magic/version
    - model size
    - checksum (FNV-1a)
    - loaded timestamp
    - ready flag
- Worker heartbeat is emitted to supervisor over Unix socket.
- API receives supervisor crash notifications and maps in-flight failures.
- Shell app mediates all UI inference through scheduler and API.

---

## 12) Vendored llama.cpp

`backend/inference-worker/llama-src/` is an unmodified copy of llama.cpp, used through the
thin `InferEngine` wrapper. Don't edit or reformat it.

It ships with its MIT licence at `llama-src/LICENSE`, which upstream's own build expects
(`CMakeLists.txt:186`) and which MIT requires to travel with the source. If CMake warns
that the licence file is not found, the file has gone missing and needs restoring from
the upstream repository, not ignoring.

---
