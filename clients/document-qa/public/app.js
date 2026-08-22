const API_BASE = (window.MFE_CONFIG && window.MFE_CONFIG.shellApiBase) || "";

const promptEl = document.getElementById("prompt");
const askBtn = document.getElementById("askBtn");
const prefetchBtn = document.getElementById("prefetchBtn");
const burstBtn = document.getElementById("burstBtn");
const clearBtn = document.getElementById("clearBtn");
const queueBanner = document.getElementById("queueBanner");
const cancelBtn = document.getElementById("cancelBtn");
const chatEl = document.getElementById("chat");
const eventsEl = document.getElementById("events");
const docHealthEl = document.getElementById("docHealth");
const lastMetaEl = document.getElementById("lastMeta");
const queueMetaEl = document.getElementById("queueMeta");

let currentRequestId = null;
let currentSource = null;
let hasChatContent = false;
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

function setQueue(text, showCancel = false) {
    queueBanner.style.display = text ? "block" : "none";
    queueBanner.textContent = text || "";
    cancelBtn.style.display = showCancel ? "inline-block" : "none";
    queueMetaEl.textContent = text || "idle";
}

function appendMessage(cls, text) {
    if (!hasChatContent) {
        chatEl.textContent = "";
        chatEl.classList.remove("empty-state");
        hasChatContent = true;
    }
    const div = document.createElement("div");
    div.className = cls;
    div.textContent = text;
    chatEl.appendChild(div);
    chatEl.scrollTop = chatEl.scrollHeight;
    return div;
}

function logEvent(type, requestId, payload = {}) {
    const div = document.createElement("div");
    const tone = type === "done" ? "ok" : type === "error" || type === "cancelled" ? "bad" : "warn";
    div.className = `event-item ${tone}`;
    div.innerHTML = `<strong>${type}</strong> req=${shortId(requestId)} ${JSON.stringify(payload)}`;
    eventsEl.prepend(div);
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
        const ready = workers.filter((worker) => worker.status === "ready").length;
        docHealthEl.textContent = `ready ${ready}, queue ${scheduler.queueLength || 0}`;
        docHealthEl.className = ready > 0 ? "status-pill ok" : "status-pill bad";
    } catch {
        docHealthEl.textContent = "shell offline";
        docHealthEl.className = "status-pill bad";
    }
}

function closeCurrentSource() {
    if (currentSource) {
        currentSource.close();
        currentSource = null;
    }
}

// Rejects with the shell's error payload attached, which is what withRetry
// reads to decide whether to try again.
function runStreamAttempt({ requestId, prompt, priority, responseEl }) {
    return new Promise((resolve, reject) => {
        const params = new URLSearchParams({
            requestId,
            prompt,
            mfeId: "doc-qa",
            priority,
        });
        const source = new EventSource(apiUrl(`/api/stream?${params.toString()}`));
        currentSource = source;

        const closeSource = () => {
            source.close();
            if (currentSource === source) {
                currentSource = null;
            }
        };

        source.addEventListener("queued", (event) => {
            const payload = JSON.parse(event.data);
            const eta = Math.ceil((payload.estimatedWaitMs || 0) / 1000);
            setQueue(`Queued: position ${payload.position}, ETA ${eta}s`, true);
            logEvent("queued", requestId, { position: payload.position, eta });
        });

        source.addEventListener("started", () => {
            setQueue("Started: active inference slot in use", true);
            logEvent("started", requestId);
        });

        source.addEventListener("token", (event) => {
            const payload = JSON.parse(event.data);
            responseEl.textContent += payload.token || "";
            chatEl.scrollTop = chatEl.scrollHeight;
        });

        source.addEventListener("timeout", (event) => {
            const payload = JSON.parse(event.data);
            logEvent("timeout", requestId, { phase: payload.phase, retryable: payload.retryable });
        });

        source.addEventListener("cancelled", () => {
            closeSource();
            const err = new Error("request_cancelled");
            err.payload = { error: "request_cancelled" };
            reject(err);
        });

        source.addEventListener("done", (event) => {
            const payload = JSON.parse(event.data);
            closeSource();
            resolve(payload);
        });

        source.addEventListener("error", (event) => {
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

async function submit(priority, rawPrompt, options = {}) {
    const prompt = (rawPrompt || promptEl.value).trim();
    if (!prompt) return;

    // Every retry uses this same id. If a retry races an answer that arrived
    // late, the agent replays the cached result instead of running again.
    const requestId = options.requestId || makeId();
    currentRequestId = requestId;
    appendMessage("msg-user", `You (${priority.toUpperCase()}, ${shortId(requestId)}): ${prompt}`);
    const responseEl = appendMessage("msg-bot", "Assistant: ");
    lastMetaEl.textContent = `running ${shortId(requestId)}`;
    logEvent("submitted", requestId, { priority, transport: "sse", shell: API_BASE });
    cancelledRequests.delete(requestId);

    try {
        const payload = await window.MFE_RETRY.withRetry(
            () => {
                // Clear whatever the failed attempt already printed.
                responseEl.textContent = "Assistant: ";
                return runStreamAttempt({ requestId, prompt, priority, responseEl });
            },
            {
                cancelled: () => cancelledRequests.has(requestId),
                onRetry: ({ attempt, of, waitMs, error }) => {
                    const seconds = Math.ceil(waitMs / 1000);
                    setQueue(`${error}: retrying in ${seconds}s (attempt ${attempt + 1} of ${of})`, false);
                    logEvent("retry", requestId, { attempt, of, waitMs, error });
                },
            }
        );

        logEvent("done", requestId, {
            device: payload.device || "cpu",
            degraded: Boolean(payload.degraded),
            replay: Boolean(payload.replay),
        });
        responseEl.textContent = `Assistant (${payload.device || "cpu"}): ${payload.result || responseEl.textContent.replace("Assistant: ", "")}`;
        lastMetaEl.textContent = `done ${shortId(requestId)} on ${payload.device || "cpu"}`;
        setQueue("");
    } catch (err) {
        const payload = (err && err.payload) || {};
        logEvent("error", requestId, payload);
        responseEl.className = "msg-error";
        responseEl.textContent = `Request failed after retries: ${payload.error || "stream_failed"}`;
        lastMetaEl.textContent = "failed";
        setQueue("");
    }
    refreshHealth();
}

askBtn.addEventListener("click", () => submit("high"));
prefetchBtn.addEventListener("click", () => submit("low"));
burstBtn.addEventListener("click", () => {
    const base = promptEl.value.trim() || "Create a short document question suggestion.";
    for (let index = 0; index < 5; index += 1) {
        submit("low", `${base} [burst ${index + 1}]`);
    }
});
promptEl.addEventListener("keydown", (event) => {
    if (event.key === "Enter") submit("high");
});

cancelBtn.addEventListener("click", async () => {
    if (!currentRequestId) return;
    cancelledRequests.add(currentRequestId);
    closeCurrentSource();
    await fetch(apiUrl("/api/cancel"), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ requestId: currentRequestId }),
    }).catch(() => {});
    setQueue("");
    logEvent("cancelled", currentRequestId, { reason: "user_cancelled" });
    appendMessage("msg-error", `Cancelled ${shortId(currentRequestId)}.`);
});

clearBtn.addEventListener("click", () => {
    closeCurrentSource();
    hasChatContent = false;
    chatEl.textContent = "No requests yet.";
    chatEl.className = "scroll-box empty-state";
    eventsEl.textContent = "";
    lastMetaEl.textContent = "no request yet";
    setQueue("");
});

refreshHealth();
setInterval(refreshHealth, 2000);
