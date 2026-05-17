# Local multi-process inference runtime

- **C++ control plane**: `supervisor`, `model-cache`, `inference-worker`
- **Node.js API plane**: `api-server` (worker-pool + HTTP/SSE)
- **Node.js shell plane**: `shell-app` (scheduler + two MFEs)
- **Single command orchestration**: `make run`

This project runs an on device GGUF model (for example: Phi-3), exposes inference APIs and serves two browser UIs:

- **Document Q&A**
- **Meeting Summariser**

---

## 1) Repository layout

```text
server_infer/
├── CMakeLists.txt
├── Makefile
├── .env.example
├── api-server/
│   ├── server.js
│   ├── ipc.js
│   ├── routes/infer.js
│   └── package.json
├── inference-worker/
│   ├── main.cpp
│   ├── worker.h
│   ├── worker.cpp
│   ├── inferEngine.h
│   ├── inferEngine.cpp
│   ├── CMakeLists.txt
│   └── llama-src/          # vendored llama.cpp source tree (thin-wrapper approach)
├── model-cache/
│   ├── main.cpp
│   ├── model_cache.h
│   └── model_cache.cpp
├── supervisor/
│   ├── main.cpp
│   ├── supervisor.h
│   └── supervisor.cpp
├── shell-app/
│   ├── server.js
│   ├── scheduler.js
│   ├── edgeAgentService.js
│   ├── package.json
│   └── public/
│       ├── shell.html
│       ├── mfe-doc-qa.html
│       ├── mfe-meeting-summary.html
│       ├── css/styles.css
│       └── js/*.js
├── ipc/paths.h
└── scripts/
    ├── build.sh
    ├── start.sh
    └── stop.sh
```

---

## 2) Runtime architecture

### Processes

1. **`edge-supervisor`** starts and monitors all child services.
2. **`edge-model-cache`** loads model weights and keeps them in POSIX shared memory.
3. **`edge-inference-worker` x N** runs inference over Unix sockets.
4. **`api-server`** exposes `/infer` and `/infer/stream`.
5. **`shell-app`** exposes UI + scheduler-backed app routes.

### IPC and shared resources

- Supervisor socket: `/tmp/edge-supervisor.sock`
- API notify socket: `/tmp/edge-api-notify.sock`
- Worker sockets: `/tmp/edge-worker-0.sock`, `/tmp/edge-worker-1.sock`, ...
- Shared memory name: `/edge-model-weights`
- Crash log: `/tmp/edge-crash.log`
- Model config snapshot: `/tmp/edge-model-config.json`
- Last request snapshot: `/tmp/edge-last-request.json`

All constants are centralized in `ipc/paths.h`.

---

## 3) Current inference implementation

The worker uses a **thin inference wrapper (`InferEngine`)** over vendored llama APIs, but model ownership is handled by the custom Edge runtime:

- `edge-model-cache` loads the GGUF bytes once into POSIX shared memory.
- The raw shared-memory object is exposed at `/dev/shm/edge-model-weights`.
- Readiness/checksum metadata is stored separately at `/dev/shm/edge-model-weights.meta`.
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

- Global max concurrent slots (`maxSlots`, default 4)
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

### `GET /infer/stream`

Query: `prompt`, `requestId`, `mfeId`

Server-Sent Events:

- `token`
- `done`
- `error`

### `GET /health`

Returns worker status and slot usage.

## 5.2 shell-app (port `EDGE_SHELL_PORT`, default `3000`)

UI routes:

- `/`
- `/mfe-doc-qa`
- `/mfe-meeting-summary`

App routes:

- `POST /api/infer`
- `GET /api/stream`
- `POST /api/cancel`
- `GET /api/queue-status`
- `GET /api/health`

---

## 6) Environment variables

Defined in `.env` (loaded by `scripts/start.sh`):

```env
EDGE_WORKER_COUNT=2
EDGE_MODEL_PATH=./models/Phi-3-mini-4k-instruct-q4.gguf
EDGE_API_PORT=11434
EDGE_SHELL_PORT=3000
EDGE_FORCE_CPU=0
EDGE_LOG_LEVEL=info
EDGE_MAX_TOKENS=512
EDGE_TEMPERATURE=0.8
EDGE_GPU_LAYERS=99
EDGE_SEED=42
```

Related scheduler envs (optional, read by shell-app):

- `EDGE_MAX_SLOTS` (default 4)
- `EDGE_MAX_PER_MFE` (default 2)
- `EDGE_MAX_QUEUE` (default 20)
- `EDGE_AGING_MS` (default 15000)
- `EDGE_QUEUE_TIMEOUT_MS` (default 30000)
- `EDGE_DEFAULT_JOB_MS` (default 8000)

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
curl -X POST http://127.0.0.1:11434/infer \
  -H "Content-Type: application/json" \
  -d '{"prompt":"What is 2+2?","requestId":"smoke-1","mfeId":"doc-qa"}'
```

### Quick streaming check

```bash
curl -N "http://127.0.0.1:11434/infer/stream?prompt=Summarize+AI+in+3+points&requestId=s1&mfeId=meeting-summary"
```

### Open UI

- `http://127.0.0.1:3000/mfe-doc-qa`
- `http://127.0.0.1:3000/mfe-meeting-summary`

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
