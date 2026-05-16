const transcriptEl = document.getElementById("transcript");
const summariseBtn = document.getElementById("summariseBtn");
const stopBtn = document.getElementById("stopBtn");
const statusEl = document.getElementById("status");
const outputEl = document.getElementById("output");

let currentRequestId = null;
let source = null;

function makeId() {
    return (
        (crypto.randomUUID && crypto.randomUUID()) ||
        `req_${Date.now()}_${Math.random()}`
    );
}

function setStatus(text) {
    statusEl.textContent = text;
}

function stopCurrent(showMessage = true) {
    if (source) {
        source.close();
        source = null;
    }
    stopBtn.disabled = true;
    if (showMessage) {
        setStatus("Stopped");
    }
}

summariseBtn.addEventListener("click", async () => {
    const prompt = transcriptEl.value.trim();
    if (!prompt) return;

    outputEl.textContent = "";
    currentRequestId = makeId();
    const params = new URLSearchParams({
        requestId: currentRequestId,
        prompt,
        mfeId: "meeting-summary",
        priority: "high",
    });

    setStatus("Connecting...");
    stopBtn.disabled = false;

    source = new EventSource(`/api/stream?${params.toString()}`);

    source.addEventListener("queued", (event) => {
        const payload = JSON.parse(event.data);
        setStatus(
            `Waiting... position ${payload.position}, ETA ${Math.ceil((payload.estimatedWaitMs || 0) / 1000)}s`,
        );
    });

    source.addEventListener("started", () => {
        setStatus("Streaming... slot in use");
    });

    source.addEventListener("token", (event) => {
        const payload = JSON.parse(event.data);
        const token = payload.token || "";
        outputEl.textContent += token;
        if (!token.endsWith(" ")) {
            outputEl.textContent += " ";
        }
    });

    source.addEventListener("done", (event) => {
        const payload = JSON.parse(event.data);
        setStatus(`Done (${payload.device || "cpu"})`);
        stopCurrent(false);
    });

    source.addEventListener("error", (event) => {
        try {
            const payload = event.data ? JSON.parse(event.data) : {};
            if (payload.retryAfterSeconds) {
                setStatus(`Worker crashed. Retry in ${payload.retryAfterSeconds}s`);
            } else {
                setStatus(`Error: ${payload.error || "stream_failed"}`);
            }
        } catch {
            setStatus("Connection closed.");
        }
        stopCurrent(false);
    });
});

stopBtn.addEventListener("click", async () => {
    if (!currentRequestId) return;
    await fetch("/api/cancel", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ requestId: currentRequestId }),
    });
    stopCurrent();
});
