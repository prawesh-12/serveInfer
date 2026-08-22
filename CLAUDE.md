# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ServeInfer — an on-device, multi-process LLM inference runtime for GGUF models, plus a
micro-frontend client stack that consumes it. Written as a submission for the Sarvam AI Backend
Intern assignment.

Naming: the product is ServeInfer, but **every** binary, identifier and env var says `edge` /
`EDGE_`. Don't rename.

`extras/` (gitignored) holds the assignment PDF and `assignment-gaps.md`, which tracks what the
brief asks for that the code does not yet do. Read the gap file before proposing "missing feature"
work — the omissions are known and deliberate.

## Commands

```bash
make build       # cmake backend/ into build/, then npm install in the two node services

make backend     # supervisor (model-cache, api-server, workers) + shell-app
make clients      # the two sample apps
make dashboard    # the operator status page

make backend-stop # each tier stops on its own, touching nothing else
make clients-stop
make dashboard-stop

make run          # stop, build, then start all three
make stop         # stop all three
make restart
```

`make build` compiles vendored llama.cpp with CUDA on — several minutes cold.

**Fast iteration without the real backend:**

```bash
cmake -S . -B build -DEDGE_ENABLE_LLAMA=OFF && cmake --build build -j"$(nproc)"
```

This path works: `llama.h` is guarded, so the tree builds with no vendored backend.
Without the `EDGE_USE_LLAMA` define, `InferEngine::generate()` returns the literal string
`"Inference response: <prompt>"` (`backend/inference-worker/inferEngine.cpp:228-233`). No model file needed
for the C++ build, though `scripts/backend.sh` still checks one exists. Real backend on CPU only:
`-DEDGE_ENABLE_CUDA=OFF`.

**Model download** (required before `make backend`):

```bash
hf download microsoft/Phi-3-mini-4k-instruct-gguf Phi-3-mini-4k-instruct-q4.gguf --local-dir ./backend/models
```

**Running one service on its own** — Node services read config from env only, so load it first:

```bash
set -a && source .env.example && source .env && set +a
node backend/shell-app/server.js
```

### Tests, lint, CI

There are none: no test file, no test framework, no `test` script, no linter config, no CI.
Verification is manual.

```bash
curl -X POST http://127.0.0.1:11434/infer -H 'Content-Type: application/json' \
  -d '{"prompt":"What is 2+2?","requestId":"smoke-1","mfeId":"doc-qa"}'
curl -N "http://127.0.0.1:11434/infer/stream?prompt=Say+hi&requestId=s1&mfeId=doc-qa"
pkill -f edge-inference-worker      # expect 503 worker_crashed, then a new line in $EDGE_CRASH_LOG
ls -l /dev/shm/edge-model-weights   # one shared copy, not one per worker
```

Browser: Doc Q&A `:5002` (press "Burst LOW x5" to see queue positions), Meeting Summariser `:5001`
(streaming), status dashboard `:3001`.

If you add tests, the pure functions are the cheap wins: scheduler priority/aging
(`backend/shell-app/scheduler.js:196-205`), circuit breaker (`backend/supervisor/supervisor.cpp:50-76`), the worker's
regex JSON parsing (`backend/inference-worker/worker.cpp:22-67`), the env parser (`backend/config/env.js:10`).

## Architecture

### Three tiers, three lifecycles

The repo is split by who owns a process, and each tier starts and stops on its own:

- `backend/` is the runtime. Supervisor, model cache, workers, api-server, shell.
  Nothing in here serves a browser.
- `clients/` are the sample user apps. They are HTTP clients of the shell API and
  import nothing from the backend, so `node clients/document-qa/server.js` runs
  from a clone with no repo config at all.
- `dashboard/` is the operator view. It also imports nothing from the backend, but
  it does read `$EDGE_STATE_DIR`, so it has to run on the same host.

`.env` and `.env.example` stay at the repo root and all three tiers read them. The
one thing that stays deliberately coupled is `EDGE_ALLOWED_MFE_ORIGINS`: the shell
has to name every client origin or CORS drops the request.

### The pidfile registry

Every process writes `$EDGE_STATE_DIR/<name>.pid` on start and removes it on exit.
The supervisor does this for its own children too (`Supervisor::writePidFile`), so
that directory is the complete process list.

It is how the dashboard knows what is running. It used to scrape `ps` and
string-match command lines against hardcoded paths, which broke on every folder
move and could never see a process it had not been told about. A new client joins
the list by writing one file, with no dashboard change.

The name carries the tier, which is also how each stop script knows what is its
own: `backend-supervisor`, `backend-worker-0`, `client-document-qa`, `dashboard`.
Each process also logs to `$EDGE_STATE_DIR/<name>.log`.

### Two process trees inside the backend

`scripts/backend.sh` starts **two independent things**. This is the most common source of confusion:

1. **`edge-supervisor`**, which itself forks and owns, in this order: `edge-model-cache` →
   `api-server` (node) → N × `edge-inference-worker`. It crash-restarts these, behind a circuit
   breaker of 3 crashes / 60 s. See `backend/supervisor/supervisor.cpp:86-107`.
2. **`shell-app`**, a plain background job registered as `backend-shell-app`.

The supervisor does **not** supervise the shell app, and it has never known about the
dashboard or the clients. Killing it leaves all of them running, which is now the
point rather than a surprise. `scripts/backend.sh stop` clears both trees plus any
leftover port listener.

Startup order is strict: model-cache must publish `ready=1` in the shm header before the api-server
or any worker starts (`Supervisor::waitForModelReady`, 30 s timeout).

### The request path

`Browser MFE (:5001/:5002)` → `shell-app (:3000)` → `api-server (:11434)` → `worker (unix socket)` → `llama`

Concurrency is limited in **two independent places**, configured separately:

- `backend/shell-app/scheduler.js` — the client-side scheduler: `EDGE_MAX_SLOTS` global cap,
  `EDGE_MAX_PER_MFE` per-MFE cap, 3 priorities with aging, queue cap → 429, cancel, queue-wait
  timeout.
- `backend/api-server/ipc.js` (`WorkerPool`) — one in-flight request per worker, `EDGE_WORKER_COUNT`
  workers, throws `no_ready_workers` → 503 when none is free.

These can disagree. If `EDGE_MAX_SLOTS` exceeds `EDGE_WORKER_COUNT`, the scheduler admits work the
agent then rejects. `EDGE_MAX_PER_MFE` must be strictly less than `EDGE_MAX_SLOTS` or the fairness
rule is inert: equal values let one MFE hold every slot. Shipped values are 4 slots, 4 workers,
2 per MFE.

Two timeouts bound a request, and the SSE `timeout` event carries `phase` to tell them apart:
`EDGE_QUEUE_TIMEOUT_MS` while queued (408, `phase: "queue"`) and `EDGE_EXEC_TIMEOUT_MS` while
running (504, `phase: "execution"`). The execution one aborts the job's `AbortController`.

### MFEs never see the agent

MFEs are served from their own ports and receive only `shellApiBase`, injected at runtime through a
generated `/config.js` (`clients/document-qa/server.js:25-33`). The api-server binds `127.0.0.1` only.
The shell enforces an origin allowlist from `EDGE_ALLOWED_MFE_ORIGINS`. A new MFE port must be added
to that variable or CORS silently drops its requests.

### Streaming is two SSE hops

The api-server emits SSE to the shell. The shell **parses that SSE stream by hand**
(`backend/shell-app/edgeAgentService.js:115-137`) and re-emits its own SSE to the browser, adding
scheduler-only events. The vocabularies differ:

- api-server → shell: `token`, `done`, `error`
- shell → browser: those plus `queued`, `started`, `cancelled`, `timeout`

Changing a token payload shape means editing both hops and both MFE clients.

### IPC conventions

Everything between processes is **newline-delimited JSON over AF_UNIX**. The C++ side has no JSON
library: the worker extracts fields with `std::regex` (`backend/inference-worker/worker.cpp:22-67`) and
builds replies by string concatenation with a hand-rolled `jsonEscape`. Adding a field to a worker
message means touching both the extractor and the emitter.

- `$EDGE_SUPERVISOR_SOCK` — workers heartbeat here every 50 ms; the supervisor currently only drains
  and discards them (`drainSupervisorSocket`), so heartbeats are not yet a liveness signal.
- `$EDGE_API_NOTIFY_SOCK` — supervisor → api-server crash notifications. Drives
  `WorkerPool._markWorkerCrashed`, which fails in-flight requests with 503 + `Retry-After`.
- `$EDGE_WORKER_SOCKET_PREFIX<id>.sock` — api-server → worker, one connection per request.
- `$EDGE_SHM_NAME` and `$EDGE_SHM_NAME.meta` — the GGUF bytes and a 256-byte `SharedModelHeader`.

### Device fallback lives in the ladder, not in `selectDevice`

`backend/inference-worker/deviceLadder.cpp` owns tier selection, quarantine and the health-check gate that
must pass before a faulted tier is used again. `EDGE_DEVICE_LADDER` names the tiers, so a tier with
no backend compiled in (`npu`, `ane`, `metal`) fails its probe and is skipped rather than breaking.

`degraded` is now measured against the best tier available at startup, not against `activeDevice_ ==
"cpu"`. A CPU-only machine is not degraded; a machine that fell off cuda onto cpu is. Reporting it
any other way makes the flag meaningless. Set `EDGE_SIMULATE_DEVICE_FAULT=removed|unsupported|runtime`
to exercise the path without the hardware.

### Replayed request ids are idempotent

`backend/api-server/requestRegistry.js` coalesces concurrent submissions of one `requestId` onto a single
inference run, and caches successful results for `EDGE_IDEMPOTENCY_TTL_MS`. Failures are
deliberately **not** cached, or a client could never retry that id. Both MFEs retry with the same
id on purpose (`clients/*/public/retry.js`), which is what makes the cache useful.

The set of open requests is written to `$EDGE_INFLIGHT_PATH` (temp file plus rename) and read back
at boot, surfacing under `/health` as `requests.orphanedFromPreviousRun`. It replaced
`EDGE_LAST_REQUEST_PATH`, which was write-only and held one id.

### The model is loaded once, into /dev/shm

`edge-model-cache` copies the GGUF into POSIX shared memory and publishes size, FNV-1a checksum and
a ready flag in the `.meta` header. Each worker validates that header, mmaps the segment, then
**repoints its own model path at `/dev/shm/...`** (`backend/inference-worker/worker.cpp:231-234`) so llama
loads from shared memory rather than disk. This is what makes a worker restart cheaper than a cold
start, and why N workers don't cost N × 2.3 GB.

### Config is env-only, and strict

`backend/config/env.js` loads `.env.example` first, then `.env` (which overrides it); a variable already
present in the real process environment beats both. `requiredEnv()` **throws** on a missing or empty
value — there are deliberately no fallback literals anywhere in the Node code. The C++ side reads
the same variables through `backend/ipc/paths.h`.

Consequence: a new config value must be added to `.env.example` (tracked), not just `.env`
(gitignored), or a fresh clone crashes at startup. `scripts/backend.sh` keeps its own explicit
required-variable list that also needs updating.

## Gotchas

- each tier aborts if its own port is taken. Run that tier's stop first, which also removes
  stale sockets and shm objects, which otherwise break the next boot.
- Changing a port means updating its matching `*_URL` / `*_BASE` variable **and**
  `EDGE_ALLOWED_MFE_ORIGINS`.
- `backend/inference-worker/llama-src/` is a vendored upstream tree. Don't edit or reformat it; it is
  excluded from searches for a reason.
- `backend/inference-worker/llama-src/LICENSE` must stay in place. It is llama.cpp's MIT
  licence, upstream's build reads it (`CMakeLists.txt:186`), and MIT requires it to ship
  with any redistribution of the source.
