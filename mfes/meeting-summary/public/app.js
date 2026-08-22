const API_BASE = (window.MFE_CONFIG && window.MFE_CONFIG.shellApiBase) || "";

const transcriptEl = document.getElementById("transcript");
const summariseBtn = document.getElementById("summariseBtn");
const stopBtn = document.getElementById("stopBtn");
const clearStreamBtn = document.getElementById("clearStreamBtn");
const statusEl = document.getElementById("status");
const outputEl = document.getElementById("output");
const streamEventsEl = document.getElementById("streamEvents");
const streamHealthEl = document.getElementById("streamHealth");
const streamMetaEl = document.getElementById("streamMeta");
const streamQueueMetaEl = document.getElementById("streamQueueMeta");

let currentRequestId = null;
let source = null;
let tokenCount = 0;
const cancelledRequests = new Set();

function apiUrl(path) {
    return `${API_BASE}${path}`;
}

function makeId() {
    return (
        (crypto.randomUUID && crypto.randomUUID()) ||
        `req_${Date.now()}_${Math.random()}`
    );
}

function shortId(requestId) {
    return requestId ? requestId.slice(0, 8) : "-";
}

function setStatus(text) {
    statusEl.style.display = "block";
    statusEl.textContent = text;
    streamQueueMetaEl.textContent = text;
}

function logEvent(type, requestId, payload = {}) {
    const div = document.createElement("div");
    const tone = type === "done" ? "ok" : type === "error" || type === "cancelled" ? "bad" : "warn";
    div.className = `event-item ${tone}`;
    div.innerHTML = `<strong>${type}</strong> req=${shortId(requestId)} ${JSON.stringify(payload)}`;
    streamEventsEl.prepend(div);
}

async function refreshHealth() {
    try {
        const [schedulerRes, agentRes] = await Promise.all([
            fetch(apiUrl("/api/health")),
            fetch(apiUrl("/api/agent-health")),
        ]);
        if (!schedulerRes.ok || !agentRes.ok) {
            throw new Error("offline");
        }
        const scheduler = await schedulerRes.json();
        const agent = await agentRes.json();
        const workers = Array.isArray(agent.workers) ? agent.workers : [];
        const busy = workers.filter((worker) => worker.status === "busy").length;
        streamHealthEl.textContent = `busy ${busy}, queue ${scheduler.queueLength || 0}`;
        streamHealthEl.className = "status-pill ok";
    } catch {
        streamHealthEl.textContent = "shell offline";
        streamHealthEl.className = "status-pill bad";
    }
}

function stopCurrent(showMessage = true) {
    if (source) {
        source.close();
        source = null;
    }
    stopBtn.disabled = true;
    summariseBtn.disabled = false;
    if (showMessage) {
        setStatus("Stopped");
        logEvent("cancelled", currentRequestId, { reason: "user_cancelled" });
    }
}

// One attempt is one EventSource, opened and closed. It resolves when the
// `done` event arrives. It rejects with the shell's error payload attached, so
// withRetry can decide whether to try again.
function runStreamAttempt({ requestId, prompt }) {
    return new Promise((resolve, reject) => {
        const params = new URLSearchParams({
            requestId,
            prompt,
            mfeId: "meeting-summary",
            priority: "high",
        });
        source = new EventSource(apiUrl(`/api/stream?${params.toString()}`));
        const active = source;

        const closeSource = () => {
            active.close();
            if (source === active) {
                source = null;
            }
        };

        active.addEventListener("queued", (event) => {
            const payload = JSON.parse(event.data);
            const eta = Math.ceil((payload.estimatedWaitMs || 0) / 1000);
            setStatus(`Queued: position ${payload.position}, ETA ${eta}s`);
            logEvent("queued", requestId, { position: payload.position, eta });
        });

        active.addEventListener("started", () => {
            setStatus("Streaming: active inference slot in use");
            logEvent("started", requestId);
        });

        active.addEventListener("token", (event) => {
            const payload = JSON.parse(event.data);
            tokenCount += 1;
            outputEl.textContent += payload.token || "";
            streamMetaEl.textContent = `${tokenCount} tokens`;
        });

        active.addEventListener("timeout", (event) => {
            const payload = JSON.parse(event.data);
            logEvent("timeout", requestId, { phase: payload.phase, retryable: payload.retryable });
        });

        active.addEventListener("cancelled", () => {
            closeSource();
            const err = new Error("request_cancelled");
            err.payload = { error: "request_cancelled" };
            reject(err);
        });

        active.addEventListener("done", (event) => {
            closeSource();
            resolve(JSON.parse(event.data));
        });

        active.addEventListener("error", (event) => {
            let payload = {};
            try {
                payload = event.data ? JSON.parse(event.data) : {};
            } catch {
                payload = { error: "connection_closed" };
            }
            closeSource();
            const err = new Error(payload.error || "stream_failed");
            err.payload = payload;
            reject(err);
        });
    });
}

summariseBtn.addEventListener("click", async () => {
    const prompt = transcriptEl.value.trim();
    if (!prompt) return;

    outputEl.textContent = "";
    outputEl.classList.remove("empty-state");
    // Every retry uses this same id, so the agent replays a cached answer
    // rather than writing the same summary twice.
    currentRequestId = makeId();
    cancelledRequests.delete(currentRequestId);
    const requestId = currentRequestId;
    tokenCount = 0;

    setStatus("Connecting to shell SSE...");
    streamMetaEl.textContent = `stream ${shortId(requestId)}`;
    logEvent("submitted", requestId, { priority: "high", transport: "sse", shell: API_BASE });
    stopBtn.disabled = false;
    summariseBtn.disabled = true;

    try {
        const payload = await window.MFE_RETRY.withRetry(
            () => {
                // Clear the half-written summary from the failed attempt.
                outputEl.textContent = "";
                tokenCount = 0;
                return runStreamAttempt({ requestId, prompt });
            },
            {
                cancelled: () => cancelledRequests.has(requestId),
                onRetry: ({ attempt, of, waitMs, error }) => {
                    const seconds = Math.ceil(waitMs / 1000);
                    setStatus(`${error}: retrying in ${seconds}s (attempt ${attempt + 1} of ${of})`);
                    logEvent("retry", requestId, { attempt, of, waitMs, error });
                },
            }
        );

        setStatus(`Done on ${payload.device || "cpu"}`);
        logEvent("done", requestId, {
            device: payload.device || "cpu",
            degraded: Boolean(payload.degraded),
            replay: Boolean(payload.replay),
            tokens: tokenCount,
        });
    } catch (err) {
        const payload = (err && err.payload) || {};
        setStatus(`Failed after retries: ${payload.error || "stream_failed"}`);
        logEvent("error", requestId, payload);
    }

    stopCurrent(false);
    refreshHealth();
});

stopBtn.addEventListener("click", async () => {
    if (!currentRequestId) return;
    cancelledRequests.add(currentRequestId);
    await fetch(apiUrl("/api/cancel"), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ requestId: currentRequestId }),
    }).catch(() => {});
    stopCurrent();
});

clearStreamBtn.addEventListener("click", () => {
    stopCurrent(false);
    outputEl.textContent = "No tokens yet.";
    outputEl.className = "scroll-box empty-state";
    streamEventsEl.textContent = "";
    streamMetaEl.textContent = "no stream yet";
    setStatus("Idle");
});

refreshHealth();
setInterval(refreshHealth, 2000);
