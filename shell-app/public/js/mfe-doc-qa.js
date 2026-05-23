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
            fetch("/api/health"),
            fetch("/api/agent-health"),
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
        docHealthEl.textContent = "agent offline";
        docHealthEl.className = "status-pill bad";
    }
}

function closeCurrentSource() {
    if (currentSource) {
        currentSource.close();
        currentSource = null;
    }
}

function submit(priority, rawPrompt, options = {}) {
    const prompt = (rawPrompt || promptEl.value).trim();
    if (!prompt) return;

    const requestId = options.requestId || makeId();
    currentRequestId = requestId;
    appendMessage("msg-user", `You (${priority.toUpperCase()}, ${shortId(requestId)}): ${prompt}`);
    const responseEl = appendMessage("msg-bot", "Assistant: ");
    lastMetaEl.textContent = `running ${shortId(requestId)}`;
    logEvent("submitted", requestId, { priority, transport: "sse" });

    const params = new URLSearchParams({
        requestId,
        prompt,
        mfeId: "doc-qa",
        priority,
    });

    const source = new EventSource(`/api/stream?${params.toString()}`);
    currentSource = source;

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

    source.addEventListener("done", (event) => {
        const payload = JSON.parse(event.data);
        logEvent("done", requestId, {
            device: payload.device || "cpu",
            degraded: Boolean(payload.degraded),
        });
        responseEl.textContent = `Assistant (${payload.device || "cpu"}): ${payload.result || responseEl.textContent.replace("Assistant: ", "")}`;
        lastMetaEl.textContent = `done ${shortId(requestId)} on ${payload.device || "cpu"}`;
        setQueue("");
        source.close();
        if (currentSource === source) {
            currentSource = null;
        }
        refreshHealth();
    });

    source.addEventListener("error", (event) => {
        let payload = {};
        try {
            payload = event.data ? JSON.parse(event.data) : {};
        } catch {
            payload = { error: "connection_closed" };
        }
        logEvent("error", requestId, payload);
        responseEl.className = "msg-error";
        responseEl.textContent = `Request failed: ${payload.error || "stream_failed"}`;
        lastMetaEl.textContent = "failed";
        setQueue("");
        source.close();
        if (currentSource === source) {
            currentSource = null;
        }
        refreshHealth();
    });
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
    closeCurrentSource();
    await fetch("/api/cancel", {
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
