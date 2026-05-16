const promptEl = document.getElementById("prompt");
const askBtn = document.getElementById("askBtn");
const prefetchBtn = document.getElementById("prefetchBtn");
const queueBanner = document.getElementById("queueBanner");
const cancelBtn = document.getElementById("cancelBtn");
const chatEl = document.getElementById("chat");

let currentRequestId = null;
let queuePollTimer = null;

function appendMessage(cls, text) {
    const div = document.createElement("div");
    div.className = cls;
    div.textContent = text;
    chatEl.appendChild(div);
    chatEl.scrollTop = chatEl.scrollHeight;
}

function makeId() {
    return (
        (crypto.randomUUID && crypto.randomUUID()) ||
        `req_${Date.now()}_${Math.random()}`
    );
}

function delay(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

async function pollQueue(requestId) {
    clearInterval(queuePollTimer);
    queuePollTimer = setInterval(async () => {
        const res = await fetch(
            `/api/queue-status?requestId=${encodeURIComponent(requestId)}`,
        );
        if (!res.ok) return;
        const status = await res.json();
        if (status.state === "queued") {
            queueBanner.style.display = "block";
            queueBanner.textContent = `Waiting... position ${status.position}, ETA ${Math.ceil((status.estimatedWaitMs || 0) / 1000)}s`;
            cancelBtn.style.display = "inline-block";
        } else if (status.state === "active") {
            queueBanner.style.display = "block";
            queueBanner.textContent = "Started...";
            cancelBtn.style.display = "none";
        } else {
            queueBanner.style.display = "none";
            cancelBtn.style.display = "none";
            clearInterval(queuePollTimer);
        }
    }, 1000);
}

async function submit(priority, rawPrompt, existingRequestId, retryCount = 0) {
    const prompt = (rawPrompt || promptEl.value).trim();
    if (!prompt) return;

    const requestId = existingRequestId || makeId();
    currentRequestId = requestId;
    if (!existingRequestId) {
        appendMessage("msg-user", `You (${priority.toUpperCase()}): ${prompt}`);
    }
    pollQueue(requestId);

    const response = await fetch("/api/infer", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
            requestId,
            prompt,
            mfeId: "doc-qa",
            priority,
        }),
    });

    clearInterval(queuePollTimer);
    queueBanner.style.display = "none";
    cancelBtn.style.display = "none";

    if (response.status === 503) {
        const payload = await response.json();
        const waitSecs = Number(payload.retryAfterSeconds || 2);
        if (retryCount < 1) {
            appendMessage(
                "msg-error",
                `Worker crashed. Retrying in ${waitSecs}s...`,
            );
            await delay(waitSecs * 1000);
            await submit(priority, prompt, requestId, retryCount + 1);
            return;
        }
        appendMessage("msg-error", "Worker crashed again. Please retry.");
        return;
    }
    if (!response.ok) {
        const payload = await response.json().catch(() => ({}));
        appendMessage("msg-error", `Request failed: ${payload.error || response.status}`);
        return;
    }
    const payload = await response.json();
    appendMessage("msg-bot", `Assistant: ${payload.result}`);
}

askBtn.addEventListener("click", () => submit("high"));
prefetchBtn.addEventListener("click", () => submit("low"));
promptEl.addEventListener("keydown", (e) => {
    if (e.key === "Enter") submit("high");
});

cancelBtn.addEventListener("click", async () => {
    if (!currentRequestId) return;
    await fetch("/api/cancel", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ requestId: currentRequestId }),
    });
    clearInterval(queuePollTimer);
    queueBanner.style.display = "none";
    cancelBtn.style.display = "none";
    appendMessage("msg-error", "Request cancelled.");
});
