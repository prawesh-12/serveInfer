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
            fetch("/api/health"),
            fetch("/api/agent-health"),
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
        streamHealthEl.textContent = "agent offline";
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

summariseBtn.addEventListener("click", async () => {
    const prompt = transcriptEl.value.trim();
    if (!prompt) return;

    outputEl.textContent = "";
    outputEl.classList.remove("empty-state");
    currentRequestId = makeId();
    tokenCount = 0;
    const params = new URLSearchParams({
        requestId: currentRequestId,
        prompt,
        mfeId: "meeting-summary",
        priority: "high",
    });

    setStatus("Connecting to SSE...");
    streamMetaEl.textContent = `stream ${shortId(currentRequestId)}`;
    logEvent("submitted", currentRequestId, { priority: "high", transport: "sse" });
    stopBtn.disabled = false;
    summariseBtn.disabled = true;

    source = new EventSource(`/api/stream?${params.toString()}`);

    source.addEventListener("queued", (event) => {
        const payload = JSON.parse(event.data);
        const eta = Math.ceil((payload.estimatedWaitMs || 0) / 1000);
        setStatus(`Queued: position ${payload.position}, ETA ${eta}s`);
        logEvent("queued", currentRequestId, { position: payload.position, eta });
    });

    source.addEventListener("started", () => {
        setStatus("Streaming: active inference slot in use");
        logEvent("started", currentRequestId);
    });

    source.addEventListener("token", (event) => {
        const payload = JSON.parse(event.data);
        const token = payload.token || "";
        tokenCount += 1;
        outputEl.textContent += token;
        streamMetaEl.textContent = `${tokenCount} tokens`;
    });

    source.addEventListener("done", (event) => {
        const payload = JSON.parse(event.data);
        setStatus(`Done on ${payload.device || "cpu"}`);
        logEvent("done", currentRequestId, {
            device: payload.device || "cpu",
            degraded: Boolean(payload.degraded),
            tokens: tokenCount,
        });
        stopCurrent(false);
        refreshHealth();
    });

    source.addEventListener("error", (event) => {
        let payload = {};
        try {
            payload = event.data ? JSON.parse(event.data) : {};
        } catch {
            payload = { error: "connection_closed" };
        }
        const message = payload.retryAfterSeconds
            ? `Worker crashed. Retry in ${payload.retryAfterSeconds}s`
            : `Error: ${payload.error || "stream_failed"}`;
        setStatus(message);
        logEvent("error", currentRequestId, payload);
        stopCurrent(false);
        refreshHealth();
    });
});

stopBtn.addEventListener("click", async () => {
    if (!currentRequestId) return;
    await fetch("/api/cancel", {
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
