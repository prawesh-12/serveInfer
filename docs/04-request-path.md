# The request path

This is the map. One prompt travels from a browser tab through four processes and back, and
this page shows the shape of that trip without explaining any single hop in depth. Every hop
links to the document that owns it.

```
Browser :5000-:5005  ->  shell-app :3000  ->  api-server :11434  ->  worker (unix socket)  ->  llama
```

## The buffered path

`POST /api/infer` waits for the whole answer and returns one JSON body. None of the shipped
clients use it, they all stream, so in practice this is the `curl` path. See
[19-build-and-run.md](19-build-and-run.md) for the smoke tests.

```mermaid
sequenceDiagram
    autonumber
    participant B as Browser
    participant S as shell-app 3000
    participant Q as Scheduler
    participant A as api-server 11434
    participant P as WorkerPool
    participant W as worker
    participant L as llama

    B->>S: POST /api/infer with prompt, mfeId, priority, requestId
    S->>Q: enqueue, priority defaults to normal
    Note over Q: holds the job until a slot is free<br/>under EDGE_MAX_SLOTS and EDGE_MAX_PER_MFE
    Q->>A: POST /infer with requestId, prompt, mfeId
    A->>A: registry.lookup requestId
    alt this id already completed
        A-->>S: 200 with header X-Idempotent-Replay true
    else new id
        A->>P: _acquireWorker
        P->>W: infer frame, newline delimited JSON on the unix socket
        W->>L: generate
        L-->>W: full text
        W-->>P: result frame with text, device, degraded
        P->>P: worker back to ready
        A-->>S: 200 JSON
    end
    S-->>B: 200 JSON with result, device, degraded, degradedReason
```

## The streaming path

`GET /api/stream` is what every client app actually calls. It is two SSE hops end to end, and
the shell adds events of its own that the api-server never sends.

```mermaid
sequenceDiagram
    autonumber
    participant B as Browser EventSource
    participant S as shell-app 3000
    participant Q as Scheduler
    participant A as api-server 11434
    participant P as WorkerPool
    participant W as worker
    participant L as llama

    B->>S: GET /api/stream?prompt=...&mfeId=chat_1&priority=high
    S-->>B: SSE headers, X-Accel-Buffering no
    S->>Q: enqueue
    Q-->>S: onStatus queued
    S-->>B: event queued with position and estimatedWaitMs
    Note over Q: queued always fires, even at position 1<br/>with every slot free
    Q->>Q: _schedule picks this job
    Q-->>S: onStatus started
    S-->>B: event started
    Q->>A: GET /infer/stream
    A->>P: _acquireWorker
    P->>W: infer frame with stream true
    W->>L: generate with a token callback
    loop each token
        L-->>W: one piece
        W-->>A: token frame
        A-->>S: event token
        S-->>B: event token
    end
    W-->>A: result frame with device and degraded
    A-->>S: event done
    S-->>B: event done, then the response ends
```

## When it does not go smoothly

The interesting path. A request queues behind three others, gets its slot, and then finds the
pool has nothing ready.

```mermaid
sequenceDiagram
    autonumber
    participant B as Browser EventSource
    participant S as shell-app 3000
    participant Q as Scheduler
    participant A as api-server 11434
    participant P as WorkerPool

    B->>S: GET /api/stream
    S->>Q: enqueue
    Q-->>S: queued at position 3
    S-->>B: event queued, estimatedWaitMs 8000
    Note over Q: all four slots held, three jobs ahead
    Q->>Q: a slot frees, _pickNextJob takes this job
    Q-->>S: started
    S-->>B: event started
    S->>A: GET /infer/stream
    A->>P: _acquireWorker
    alt no worker in state ready
        P-->>A: WorkerPoolError no_ready_workers
        Note over A: SSE headers are already flushed,<br/>so this cannot be a 503 body
        A-->>S: event error worker_unavailable
        S-->>B: event error worker_unavailable
        Note over B: retry.js backs off and resubmits<br/>the same requestId
    else a worker is ready
        P-->>A: worker marked busy
        A-->>S: event token, repeatedly
    end
```

The same failure on the buffered path never gets that far into the response, so it arrives as
a real 503 `no_ready_workers` with `Retry-After: 1`
([infer.js:73](../backend/api-server/routes/infer.js#L73)). Same cause, two shapes, because
SSE cannot take back a status line it already sent.

## What crosses each hop

| Hop | What crosses it | Owned by |
|---|---|---|
| Browser to shell | HTTP JSON in, SSE out. `prompt`, `mfeId`, `priority`, `requestId` | [05-shell-app.md](05-shell-app.md) |
| Inside the shell | A job in the priority queue, holding a slot while it runs | [06-scheduler.md](06-scheduler.md) |
| Shell to api-server | HTTP JSON in, SSE out, over `EDGE_API_BASE` | [07-api-server.md](07-api-server.md) |
| Inside the api-server | A `requestId` looked up in the registry before any work starts | [08-idempotency.md](08-idempotency.md) |
| api-server to worker | One newline-delimited JSON frame per request over AF_UNIX | [09-inference-worker.md](09-inference-worker.md), frames in [16-ipc-protocols.md](16-ipc-protocols.md) |
| Inside the worker | Prompt string to token string, through the device ladder | [10-llama-integration.md](10-llama-integration.md), [13-device-fallback.md](13-device-fallback.md) |

The api-server binds `127.0.0.1` only, and no client is ever told its address. A browser
knows one base URL, the shell's, injected at runtime through a generated `/config.js`. See
[17-clients.md](17-clients.md).

## Two concurrency gates, configured separately

Admission is limited twice, in two processes, from two sets of variables. This is the single
most common source of confusion in the system.

```mermaid
flowchart LR
    C1[chat_1] --> SH
    C2[chat_2] --> SH
    C3[chat_3 to chat_5] --> SH
    SH["gate 1: shell-app scheduler<br/>EDGE_MAX_SLOTS 4<br/>EDGE_MAX_PER_MFE 2<br/>EDGE_MAX_QUEUE 20"] -->|admits or queues| AP
    AP["gate 2: api-server WorkerPool<br/>one request per worker<br/>count from workerCountSource"] --> W0[worker 0]
    AP --> W1[worker 1]
    AP --> W2[worker 2]
    AP --> W3[worker 3]
    SH -.->|429 scheduler_overloaded| X1[client]
    AP -.->|503 no_ready_workers| X2[client]
```

The scheduler decides whether a request is allowed to start at all. The pool decides whether
a worker is free to take it. Nothing keeps the two numbers in agreement: if `EDGE_MAX_SLOTS`
is larger than the number of workers that actually started, gate 1 admits work gate 2 then
refuses, instead of the request queueing. Shipped values are 4 slots, 2 per MFE, 4 workers.

The pool no longer trusts `EDGE_WORKER_COUNT` blindly. It reads the count the supervisor
actually placed out of `$EDGE_MODEL_CONFIG_PATH` through
[workerCountSource.js](../backend/api-server/workerCountSource.js), so on a RAM-constrained
host the pool sizes itself to the workers that exist. The scheduler still knows nothing about
either number. Details in [07-api-server.md](07-api-server.md) and
[12-hardware-capacity.md](12-hardware-capacity.md).

## Two timeouts, told apart by `phase`

Both live in the scheduler, and both are visible to a streaming client as the same SSE event
name.

| Bound | Variable | Applies while | HTTP on the buffered path | SSE `phase` |
|---|---|---|---|---|
| Queue wait | `EDGE_QUEUE_TIMEOUT_MS` | the job is queued | 408 `queue_timeout` | `queue` |
| Execution | `EDGE_EXEC_TIMEOUT_MS` | the job is running | 504 `exec_timeout` | `execution` |

A `timeout` event without a `phase` field cannot be told apart from the other kind, which is
why the shell always fills it in, defaulting to `queue`
([server.js:204](../backend/shell-app/server.js#L204)). The execution timeout also aborts the
job's `AbortController`, which tears down the shell's fetch to the api-server and gives the
slot back. See [06-scheduler.md](06-scheduler.md).

## Where a failure enters the path

Read this as the same trip again, with every exit marked.

```mermaid
flowchart TD
    START[browser submits] --> Q{queue at EDGE_MAX_QUEUE}
    Q -->|full| E429["429 scheduler_overloaded<br/>the api-server never sees the request"]
    Q -->|room| WAIT{waited past EDGE_QUEUE_TIMEOUT_MS}
    WAIT -->|yes| E408["408 queue_timeout<br/>SSE timeout with phase queue"]
    WAIT -->|no| POOL{a worker is in state ready}
    POOL -->|none| E503A["503 no_ready_workers<br/>Retry-After 1"]
    POOL -->|yes| RUN{the request completes}
    RUN -->|worker died| E503B["503 worker_crashed<br/>Retry-After 2, supervisor respawns it"]
    RUN -->|ran past EDGE_EXEC_TIMEOUT_MS| E504["504 exec_timeout<br/>the AbortController fires"]
    RUN -->|device faulted, ladder caught it| OKD["200 with degraded true<br/>and a degradedReason"]
    RUN -->|clean| OK[200 or SSE done]
```

Crash recovery is [14-crash-recovery.md](14-crash-recovery.md) and the ladder is
[13-device-fallback.md](13-device-fallback.md). Every failure above carries either
`retryAfterSeconds` or `retryable`, and the clients retry with the same `requestId` on
purpose. That is what makes the result cache in [08-idempotency.md](08-idempotency.md) worth
having.
