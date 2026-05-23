# Local multi-process inference runtime

This repository provides an on-device, multi-process inference runtime, especially for GGUF models (for example, Phi-3). It combines:

- **C++ control plane**: `supervisor`, `model-cache`, `inference-worker`
- **Node.js API plane**: `api-server` (worker-pool + HTTP/SSE)
- **Node.js shell plane**: `shell-app` (scheduler + singleton API facade)
- **Independent MFE plane**: `mfes/meeting-summary` and `mfes/document-qa`
- **System dashboard plane**: `system-dashboard` (runtime/process status UI)

It exposes local inference APIs and serves two browser UIs:

- **Document Q&A**
- **Meeting Summariser**

---

## 1) Project Structure

```text
server_infer/
├── CMakeLists.txt               # Root CMake entrypoint for C++ targets
├── Makefile                     # Convenience commands (run/build/start/stop/restart)
├── .env.example                 
├── api-server/                  # Node.js API service exposed to clients
│   ├── server.js                # Express bootstrap + health route + supervisor notify listener
│   ├── ipc.js                   # WorkerPool (worker socket management + request dispatch)
│   ├── routes/infer.js          # /infer and /infer/stream route handlers
│   └── package.json             # API server dependencies/scripts
├── inference-worker/            # C++ inference worker process
│   ├── main.cpp                 # Worker entrypoint + env parsing + socket server startup
│   ├── worker.h                 # Worker interface and request/response contracts
│   ├── worker.cpp               # Worker socket loop + request handling
│   ├── inferEngine.h            # Inference engine interface
│   ├── inferEngine.cpp          # llama-backed generation + streaming implementation
│   ├── CMakeLists.txt           # Worker build config (llama integration flags)
│   └── llama-src/               # Vendored llama.cpp source tree (thin-wrapper approach)
├── model-cache/                 # C++ model cache process (shared memory owner)
│   ├── main.cpp                 # Model-cache entrypoint
│   ├── model_cache.h            # Shared model header/cache API
│   └── model_cache.cpp          # GGUF load + checksum + /dev/shm management
├── supervisor/                  # C++ process supervisor for runtime children
│   ├── main.cpp                 # Supervisor CLI + startup wiring
│   ├── supervisor.h             # Supervisor class/process model definitions
│   └── supervisor.cpp           # Spawn/monitor/restart logic + crash notifications
├── shell-app/                   # Node.js shell singleton + scheduler
│   ├── server.js                # Shell routes (/api/* + shell dashboard)
│   ├── scheduler.js             # Priority queue + fairness + timeout/cancel handling
│   ├── edgeAgentService.js      # Adapter between scheduler and api-server
│   └── package.json             # Shell app dependencies/scripts
├── system-dashboard/            # Independent system status dashboard, default port 3001
│   ├── server.js                # Process/API status collector + static server
│   └── public/                  # Dashboard HTML/CSS/JS
├── mfes/
│   ├── meeting-summary/         # Independent MFE, default port 5001
│   └── document-qa/             # Independent MFE, default port 5002
├── ipc/paths.h                  # Shared IPC paths/constants for C++ services
└── scripts/
    ├── build.sh                 # Build C++ binaries + install Node dependencies
    ├── start.sh                 # Start supervisor and shell-app with env wiring
    └── stop.sh                  # Stop runtime processes and clean IPC artifacts
```

---

## 2) Runtime architecture

### Processes

1. **`edge-supervisor`** starts and monitors all child services.
2. **`edge-model-cache`** loads model weights and keeps them in POSIX shared memory.
3. **`edge-inference-worker` x N** runs inference over Unix sockets.
4. **`api-server`** exposes `/infer` and `/infer/stream`.
5. **`shell-app`** exposes the singleton scheduler-backed app API.
6. **`system-dashboard`** exposes runtime/process/API status UI.
7. **Independent MFEs** run on their own ports and call only the shell singleton.

### IPC and shared resources

- Supervisor socket: `EDGE_SUPERVISOR_SOCK`
- API notify socket: `EDGE_API_NOTIFY_SOCK`
- Worker sockets: `EDGE_WORKER_SOCKET_PREFIX` plus worker id
- Shared memory name: `EDGE_SHM_NAME`
- Crash log: `EDGE_CRASH_LOG`
- Model config snapshot: `EDGE_MODEL_CONFIG_PATH`
- Last request snapshot: `EDGE_LAST_REQUEST_PATH`

Runtime paths are centralized in `.env.example` and read by `ipc/paths.h`.

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

Qualcomm NPU and Apple ANE fallback branches are intentionally not implemented in this Linux build.

---

## 4) Scheduler behavior (shell-app)

The shell scheduler (`shell-app/scheduler.js`) enforces:

- Global max concurrent slots (`maxSlots`, default `EDGE_WORKER_COUNT`, usually 2)
- Per-MFE (Micro-Frontends) concurrent cap (`maxPerMfe`, default 2)
- Priority queue: `high`, `normal`, `low`
- Aging boost every `agingMs` (default 15s)
- Queue cap (`maxQueue`, default 20)
- Queue timeout (`queueTimeoutMs`, default 30s)
- Cancellation support for queued and active requests
- Queue position + estimated wait reporting

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
    "degraded": false
}
```

Headers:

- `X-Inference-Device`
- `X-Inference-Degraded`
- `X-Latency-Mode`

### `GET /infer/stream`

Query: `prompt`, `requestId`, `mfeId`

Server-Sent Events:

- `queued`
- `started`
- `cancelled`
- `timeout`
- `token`
- `done`
- `error`

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
EDGE_WORKER_COUNT=2
EDGE_MODEL_PATH=./models/Phi-3-mini-4k-instruct-q4.gguf
EDGE_FORCE_CPU=0
EDGE_LOG_LEVEL=info

EDGE_API_PORT=11434
EDGE_SHELL_PORT=3000
EDGE_STATUS_DASHBOARD_PORT=3001
EDGE_MEETING_MFE_PORT=5001
EDGE_DOC_QA_MFE_PORT=5002

EDGE_API_BASE=http://127.0.0.1:11434
EDGE_SHELL_PUBLIC_BASE=http://127.0.0.1:3000
EDGE_MEETING_MFE_URL=http://127.0.0.1:5001
EDGE_DOC_QA_MFE_URL=http://127.0.0.1:5002
EDGE_ALLOWED_MFE_ORIGINS=http://127.0.0.1:5001,http://localhost:5001,http://127.0.0.1:5002,http://localhost:5002

EDGE_MAX_TOKENS=512
EDGE_TEMPERATURE=0.8
EDGE_GPU_LAYERS=99
EDGE_SEED=42

EDGE_MAX_SLOTS=2
EDGE_MAX_PER_MFE=2
EDGE_MAX_QUEUE=20
EDGE_AGING_MS=15000
EDGE_QUEUE_TIMEOUT_MS=30000
EDGE_DEFAULT_JOB_MS=8000

EDGE_SHM_NAME=/edge-model-weights
EDGE_SUPERVISOR_SOCK=/tmp/edge-supervisor.sock
EDGE_WORKER_SOCKET_PREFIX=/tmp/edge-worker-
EDGE_WORKER_CONNECT_TIMEOUT_MS=3000
EDGE_API_NOTIFY_SOCK=/tmp/edge-api-notify.sock
EDGE_CRASH_LOG=/tmp/edge-crash.log
EDGE_MODEL_CONFIG_PATH=/tmp/edge-model-config.json
EDGE_LAST_REQUEST_PATH=/tmp/edge-last-request.json
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
  --local-dir ./models
```

## 7.3 Build + run

```bash
make run
```

Other commands:

```bash
make build
make start
make stop
make restart
```

---

## 8) Process lifecycle and reliability

- `make run` does: stop -> build -> start
- `start.sh` fails fast if API/shell ports are occupied
- `stop.sh` stops pidfile processes + scans and kills leftover runtime processes
- Supervisor monitors child exits and restarts workers
- Circuit breaker prevents infinite restart storms after repeated crashes
- API worker-pool tracks worker states (`starting`, `ready`, `busy`, `crashed`)

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
- Meeting Summariser MFE: `$EDGE_MEETING_MFE_URL`
- Document Q&A MFE: `$EDGE_DOC_QA_MFE_URL`

---

## 10) Troubleshooting

### `worker_unavailable` / `no_ready_workers`

- Workers are still loading model or unavailable.
- Wait a few seconds and retry.
- Check `make start` logs for worker initialization.

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

## 12) Known warning

During CMake configure of vendored llama-src, you may see:

`License file .../inference-worker/llama-src/LICENSE not found`

This is a build-time warning from vendored source layout and does not block build/run.

---
