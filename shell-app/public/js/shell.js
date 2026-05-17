const apiStateEl = document.getElementById("apiState");
const workerStateEl = document.getElementById("workerState");
const slotStateEl = document.getElementById("slotState");
const queueStateEl = document.getElementById("queueState");
const healthJsonEl = document.getElementById("healthJson");
const refreshBtn = document.getElementById("refreshBtn");

function setText(el, value) {
    el.textContent = value;
}

async function readJson(url) {
    const response = await fetch(url);
    const body = await response.json().catch(() => ({}));
    if (!response.ok) {
        const error = new Error(body.error || response.statusText);
        error.payload = body;
        throw error;
    }
    return body;
}

async function refreshHealth() {
    try {
        const [scheduler, agent] = await Promise.all([
            readJson("/api/health"),
            readJson("/api/agent-health"),
        ]);

        const workers = Array.isArray(agent.workers) ? agent.workers : [];
        const ready = workers.filter((worker) => worker.status === "ready").length;
        const busy = workers.filter((worker) => worker.status === "busy").length;

        setText(apiStateEl, "online");
        setText(workerStateEl, `${ready} ready / ${busy} busy`);
        setText(slotStateEl, `${scheduler.activeCount || 0}/${scheduler.limits?.maxSlots || 4}`);
        setText(queueStateEl, `${scheduler.queueLength || 0}/${scheduler.limits?.maxQueue || 20}`);
        healthJsonEl.textContent = JSON.stringify({ scheduler, agent }, null, 2);
    } catch (err) {
        setText(apiStateEl, "offline");
        setText(workerStateEl, "-");
        setText(slotStateEl, "-");
        setText(queueStateEl, "-");
        healthJsonEl.textContent = JSON.stringify(
            {
                error: "agent_unreachable",
                message: err instanceof Error ? err.message : "unknown_error",
                payload: err.payload || null,
            },
            null,
            2,
        );
    }
}

refreshBtn.addEventListener("click", refreshHealth);
refreshHealth();
setInterval(refreshHealth, 2000);
