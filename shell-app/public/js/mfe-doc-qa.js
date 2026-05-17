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
let queuePollTimer = null;
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
}

function logEvent(type, requestId, payload = {}) {
    const div = document.createElement("div");
    const tone = type === "done" ? "ok" : type === "error" || type === "cancelled" ? "bad" : "warn";
    div.className = `event-item ${tone}`;
    div.innerHTML = `<strong>${type}</strong> req=${shortId(requestId)} ${JSON.stringify(payload)}`;
    eventsEl.prepend(div);
}

function delay(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
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

async function pollQueue(requestId) {
    clearInterval(queuePollTimer);
    queuePollTimer = setInterval(async () => {
        try {
            const res = await fetch(
                `/api/queue-status?requestId=${encodeURIComponent(requestId)}`,
            );
            if (!res.ok) return;
            const status = await res.json();
            if (status.state === "queued") {
                const eta = Math.ceil((status.estimatedWaitMs || 0) / 1000);
                setQueue(`Queued: position ${status.position}, ETA ${eta}s`, true);
                logEvent("queued", requestId, { position: status.position, eta });
            } else if (status.state === "active") {
                setQueue("Started: active inference slot in use", true);
            } else {
                setQueue("");
                clearInterval(queuePollTimer);
            }
        } catch {
            setQueue("Queue status unavailable", false);
        }
    }, 1000);
}

async function submit(priority, rawPrompt, options = {}) {
    const prompt = (rawPrompt || promptEl.value).trim();
    if (!prompt) return;

    const requestId = options.requestId || makeId();
    currentRequestId = requestId;
    appendMessage("msg-user", `You (${priority.toUpperCase()}, ${shortId(requestId)}): ${prompt}`);
    lastMetaEl.textContent = `running ${shortId(requestId)}`;
    logEvent("submitted", requestId, { priority });
    pollQueue(requestId);

    let response;
    try {
        response = await fetch("/api/infer", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
                requestId,
                prompt,
                mfeId: "doc-qa",
                priority,
            }),
        });
    } catch {
        clearInterval(queuePollTimer);
        setQueue("");
        logEvent("error", requestId, { error: "shell_unreachable" });
        appendMessage("msg-error", "Shell/API unreachable. Start the runtime and retry.");
        lastMetaEl.textContent = "failed";
        return;
    }

    clearInterval(queuePollTimer);
    setQueue("");

    if (response.status === 503) {
        const payload = await response.json().catch(() => ({}));
        const waitSecs = Number(payload.retryAfterSeconds || 2);
        logEvent("error", requestId, payload);
        if (!options.retried) {
            appendMessage("msg-error", `Worker crashed. Retrying in ${waitSecs}s...`);
            await delay(waitSecs * 1000);
            await submit(priority, prompt, { requestId, retried: true });
            return;
        }
        appendMessage("msg-error", "Worker crashed again. Please retry.");
        lastMetaEl.textContent = "failed";
        return;
    }

    if (!response.ok) {
        const payload = await response.json().catch(() => ({}));
        logEvent("error", requestId, payload);
        appendMessage("msg-error", `Request failed: ${payload.error || response.status}`);
        lastMetaEl.textContent = "failed";
        return;
    }

    const payload = await response.json();
    logEvent("done", requestId, {
        device: payload.device || "cpu",
        degraded: Boolean(payload.degraded),
    });
    appendMessage("msg-bot", `Assistant (${payload.device || "cpu"}): ${payload.result}`);
    lastMetaEl.textContent = `done ${shortId(requestId)} on ${payload.device || "cpu"}`;
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
    await fetch("/api/cancel", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ requestId: currentRequestId }),
    }).catch(() => {});
    clearInterval(queuePollTimer);
    setQueue("");
    logEvent("cancelled", currentRequestId, { reason: "user_cancelled" });
    appendMessage("msg-error", `Cancelled ${shortId(currentRequestId)}.`);
});

clearBtn.addEventListener("click", () => {
    hasChatContent = false;
    chatEl.textContent = "No requests yet.";
    chatEl.className = "scroll-box empty-state";
    eventsEl.textContent = "";
    lastMetaEl.textContent = "no request yet";
    setQueue("");
});

refreshHealth();
setInterval(refreshHealth, 2000);
