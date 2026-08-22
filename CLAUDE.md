# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ServeInfer — an on-device, multi-process LLM inference runtime for GGUF models, plus a
micro-frontend client stack that consumes it. Written as a submission for the Sarvam AI Backend
Intern assignment.

Naming: the product is ServeInfer, but **every** binary, identifier and env var says `edge` /
`EDGE_`. Don't rename.

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

### Vendored llama.cpp version

| Field | Value |
|---|---|
| Commit | `e85caa81ea2b65797396018c179b87ad61fa38ab` (upstream build 10582) |
| ggml | `0.21.0` |
| Location | `backend/inference-worker/llama-src` — the one authoritative copy |
| Vendored subset | `CMakeLists.txt`, `LICENSE`, `cmake/`, `include/`, `src/`, `ggml/`, `vendor/` (1538 files) |

Upgraded from `2555826` / ggml `0.11.1` (b9180). That bump was deliberate, not incidental:
the reference tree only carries `e85caa81`, so syncing from it while refusing to mix
commits leaves exactly one consistent version to land on. `docs/build-matrix.md` §0.1
records the delta and the API impact — which is nil for our wrapper, though
`llama_model_params` did lose `use_mmap` / `use_mlock` / `use_direct_io` in favour of
`enum llama_load_mode`.

Two things about the subset are easy to get wrong:

- `vendor/` **must** be vendored now. `add_subdirectory(vendor)` became unconditional, and
  `vendor/hash` really compiles into the build. It was optional at b9180.
- `common/` is deliberately **not** vendored. `add_subdirectory(common)` is guarded by
  `LLAMA_BUILD_COMMON`, which the build forces OFF, and `llama` links only `ggml`.

`backend/inference-worker/CMakeLists.txt` forces off every `LLAMA_BUILD_*` option whose
directory isn't vendored — including `LLAMA_BUILD_APP` and `LLAMA_BUILD_MTMD`, which are
new since b9180. Adding a vendored directory means revisiting that list.

**Fast iteration without the real backend:**

```bash
cmake -S backend -B build -DEDGE_ENABLE_LLAMA=OFF && cmake --build build -j"$(nproc)"
```

This path works: `llama.h` is guarded, so the tree builds with no vendored backend.
Without the `EDGE_USE_LLAMA` define, `InferEngine::generate()` returns the literal string
`"Inference response: <prompt>"` (`backend/inference-worker/inferEngine.cpp:270-275`). No model file needed
for the C++ build, though `scripts/backend.sh` still checks one exists. Real backend on CPU only:
`-DEDGE_ENABLE_CUDA=OFF`.

**Model download** (required before `make backend`):

```bash
hf download microsoft/Phi-3-mini-4k-instruct-gguf Phi-3-mini-4k-instruct-q4.gguf --local-dir ./backend/models
```

**Running one service on its own.** Backend services read config from env only, so load it first:

```bash
set -a && source .env.example && source .env && set +a
node backend/shell-app/server.js
```

Clients and the dashboard don't need that. They read their own variables and every one has a
default, so `node clients/document-qa/server.js` works from a clone with no repo config.

### Tests, lint, CI

291 tests, no new dependencies: 85 JavaScript and 206 C++. `node:test` for the JavaScript
in `tests/`, a small assert harness for the C++ in `backend/inference-worker/tests/`. Nothing needs the model file, a GPU
or a running stack.

```bash
make test        # both suites, about 3s
make test-js
make test-cpp
```

No linter config and no CI. Untested: `Worker::jsonEscape` (a private static, not reachable
the way the anonymous-namespace parsers are) and the supervisor's circuit breaker, whose logic
sits in `Supervisor` methods that would need a socket-free harness.

Manual end-to-end checks:

```bash
curl -X POST http://127.0.0.1:11434/infer -H 'Content-Type: application/json' \
  -d '{"prompt":"What is 2+2?","requestId":"smoke-1","mfeId":"doc-qa"}'
curl -N "http://127.0.0.1:11434/infer/stream?prompt=Say+hi&requestId=s1&mfeId=doc-qa"
pkill -f edge-inference-worker      # expect 503 worker_crashed, then a new line in $EDGE_CRASH_LOG
ls -l /dev/shm/edge-model-weights   # one shared copy, not one per worker
```

Browser: Doc Q&A `:5002` (press "Burst LOW x5" to see queue positions), Meeting Summariser `:5001`
(streaming), status dashboard `:3001`.

The C++ side is four binaries: `edge-device-tests` (28, ladder and vendor error mapping),
`edge-worker-json-tests` (15, frame parsing), `edge-hardware-tests` (131, capacity
planning, backend assignment, the CUDA environment guarantee, the NPU/ANE state machines
driven through injectable fakes, worker reassignment, fault injection, and the streaming
contract across a fallback) and `edge-remote-recovery-tests` (32, the remote tier's two
policy gates and the climb back up the ladder, all through an injected fake transport that
opens no socket).
`make test-cpp` builds and runs all four.

Already covered by the suites: scheduler priority and aging
(`backend/shell-app/scheduler.js:196-205`), the worker's regex JSON parsing
(`backend/inference-worker/worker.cpp:22-67`), the env parser (`backend/config/env.js:10`), the
device ladder and the vendor error mapping. The circuit breaker
(`backend/supervisor/supervisor.cpp:50-76`) is still the obvious next one.

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

Logs are the one thing that does **not** live there. Each process writes
`$EDGE_LOG_DIR/<name>.log`, which defaults to `logs/` at the repo root: pidfiles are
runtime state that should die with the machine, logs are worth keeping across a reboot.
`EDGE_LOG_DIR` is optional — `scripts/lib.sh` falls back to `$ROOT/logs` and resolves a
relative value against the repo root, so an `.env` written before it existed still boots.
`logs/` is tracked through a `.gitkeep`; its contents are not.

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

**Known limitation: the JS side never learns the effective worker count.** Capacity planning
clamps how many workers actually start — `placeableWorkerCount` in
`backend/hardware/capacityPlan.cpp`, which `Supervisor::startWorkers` counts to — but the
api-server reads `EDGE_WORKER_COUNT` straight from the env (`backend/api-server/server.js:122`)
and pre-creates that many pool entries (`backend/api-server/ipc.js:36`). The supervisor does
write what actually started into the model-config JSON (`Supervisor::writeModelConfig`, as
`workerCount` beside `configuredWorkerCount`), but nothing in the Node code reads that file.
So on a RAM-constrained host the pool holds entries for workers that were never started. It
degrades safely — a pool entry whose socket is absent never becomes ready, so it is not handed
out — but the failure shape is wrong: the scheduler admits a request against a slot count that
assumes the ceiling, and the api-server then answers 503 `no_ready_workers` instead of the
request queueing. Not fixed; wiring the effective count into `WorkerPool` is the fix.

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

### Hardware assignment happens in the supervisor, before `execvp`

`backend/hardware/` is shared, llama-free code compiled into both the supervisor and the
worker: `hardwareReport.{h,cpp}` (the probe wire format plus `/proc/meminfo`) and
`capacityPlan.{h,cpp}` (pure capacity maths, worker assignment, and the env vector each
worker is started with).

The order is discovery, capacity, assignment, startup. Discovery runs in a short-lived
`edge-inference-worker --probe-hardware` child, because reaching a `ggml_backend_dev_t`
builds the ggml registry and that initializes CUDA — a cost the supervisor must not pay,
since llama.cpp b9180 has no way to release a CUDA primary context in-process. **The
supervisor must keep linking only `rt`.** Do not give it llama.

`EDGE_WORKER_COUNT` is a ceiling, not a promise. `placeableWorkerCount` decides how much of
it the machine can pay for, and that is what the start loops count to, so a worker the plan
has no budget for is never started into an OOM-kill and restart loop.

`EDGE_GPU_RESERVE_MB` / `EDGE_WORKER_GPU_MB` size the GPU pool, `EDGE_RAM_RESERVE_MB` /
`EDGE_WORKER_RAM_MB` the CPU pool. These are worker counts, not request counts:
`EDGE_MAX_SLOTS` is a different number and nothing here divides VRAM by it. The per-worker
budgets are tunable runtime numbers, deliberately not derived from the GGUF size — the
weights are one shared copy in `/dev/shm`, so the budget covers context, KV and compute
buffers.

A CPU worker is CPU-only because `Supervisor::forkExec` sets `CUDA_VISIBLE_DEVICES=-1` in
the child between fork and exec. That is the whole guarantee, and `n_gpu_layers = 0` is not
a substitute for it. Its limit: a process that has already initialized CUDA can never give
the context back, so a worker that faults off the GPU releases what it can, answers the
request in flight, then exits with `EdgeExit::kReassignCpu` (70) and the supervisor respawns
it as a CPU worker. Exit 70 is a planned exit: no crash-log line, no circuit-breaker tick.
`EDGE_DEVICE_FALLBACK_MODE=reload` opts out and keeps the context resident.

See `docs/build-matrix.md` for why one binary cannot hold CUDA, Metal and Hexagon.

### Device fallback lives in the ladder, not in `selectDevice`

`backend/inference-worker/deviceLadder.cpp` owns tier selection, quarantine and the health-check gate that
must pass before a faulted tier is used again. `EDGE_DEVICE_LADDER` names the tiers, so a tier with
no backend compiled in (`npu`, `ane`, `metal`) fails its probe and is skipped rather than breaking.

`backendRouter.{h,cpp}` sits above the ladder and below the llama wrapper. It owns the
adapters (`inferenceBackend.{h,cpp}`: Qualcomm/Hexagon, Core ML/ANE, and the llama wrapper
itself) and points the ladder's probe at them, which is what makes the NPU and ANE state
machines testable through fakes on a machine that has neither. Both platform adapters
compile everywhere and **refuse to execute** rather than faking success.

`degraded` is now measured against the best tier available at startup, not against `activeDevice_ ==
"cpu"`. A CPU-only machine is not degraded; a machine that fell off cuda onto cpu is. Reporting it
any other way makes the flag meaningless. Set `EDGE_SIMULATE_DEVICE_FAULT=[<tier>:]removed|unsupported|runtime`
to exercise the path without the hardware. It fires once per worker, and only on the named
tier, so the tier the ladder falls to keeps answering and a respawned worker is untouched.

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
- `backend/inference-worker/llama-src/` is a vendored upstream tree, pinned to llama.cpp
  commit `e85caa81` (ggml **0.21.0**). Don't edit or reformat it; it is excluded from
  searches for a reason, and its value as evidence depends on staying byte-exact against
  upstream. Re-sync it wholesale from a clean checkout of that commit, never file by file.
- `backend/inference-worker/llama-src/LICENSE` must stay in place. It is llama.cpp's MIT
  licence, upstream's build reads it (`CMakeLists.txt:198`, `license_add_file`), and MIT
  requires it to ship with any redistribution of the source.
- There is no upstream checkout in the tree. To re-verify the vendored copy or re-sync it,
  clone llama.cpp, `git checkout e85caa81`, and diff against `llama-src`. Keep any such
  checkout outside the repo, or gitignored, so `git add -A` cannot sweep it in.
