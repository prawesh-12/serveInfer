const config = window.DASHBOARD_CONFIG || {};

const refreshBtn = document.getElementById("refreshBtn");
const lastUpdatedEl = document.getElementById("lastUpdated");
const apiStripEl = document.getElementById("apiStrip");
const processListEl = document.getElementById("processList");
const processSummaryEl = document.getElementById("processSummary");
const workerListEl = document.getElementById("workerList");
const workerSummaryEl = document.getElementById("workerSummary");
const schedulerGridEl = document.getElementById("schedulerGrid");
const mfeRequestListEl = document.getElementById("mfeRequestList");
const mfeRequestSummaryEl = document.getElementById("mfeRequestSummary");
const rawJsonEl = document.getElementById("rawJson");

function healthLabel(check) {
    if (!check) return "unknown";
    if (check.ok) return check.status ? `online ${check.status}` : "online";
    return check.error || "offline";
}

function tone(ok) {
    return ok ? "ok" : "bad";
}

function esc(value) {
    return String(value).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
}

function stat(label, value, cls = "") {
    return `<div><div class="k">${esc(label)}</div><div class="v ${cls}">${esc(value)}</div></div>`;
}

function statWithDot(label, value, dot) {
    return `<div><div class="k">${esc(label)}</div><div class="v"><span class="dot ${dot}"></span>${esc(value)}</div></div>`;
}

function row(name, value, dot = "", valueCls = "") {
    return `<div><span class="dot ${dot}"></span><span class="name">${esc(name)}</span><span class="val ${valueCls}">${esc(value)}</span></div>`;
}

const TIER_LABEL = { backend: "Backend", clients: "Clients", dashboard: "Dashboard", other: "Other" };

function registryRows(registry) {
    const running = registry?.processes || [];
    const stale = registry?.stale || [];
    if (running.length === 0 && stale.length === 0) {
        return row("Nothing registered", registry?.stateDir || "no state dir", "bad", "bad");
    }

    const byTier = new Map();
    for (const proc of running) {
        if (!byTier.has(proc.tier)) byTier.set(proc.tier, []);
        byTier.get(proc.tier).push(proc);
    }

    const out = [];
    for (const tier of ["backend", "clients", "dashboard", "other"]) {
        const group = byTier.get(tier);
        if (!group || group.length === 0) continue;
        out.push(`<div class="tier">${esc(TIER_LABEL[tier] || tier)}</div>`);
        for (const proc of group) {
            out.push(row(proc.label, `${proc.pid} · ${proc.uptime}`, "ok"));
        }
    }
    for (const proc of stale) {
        out.push(row(proc.label, `pid ${proc.pid} gone, stale pidfile`, "bad", "bad"));
    }
    return out.join("");
}

function render(status) {
    const shellHealth = status.endpoints?.shell?.health;
    const apiHealth = status.endpoints?.api?.health;
    const scheduler = status.scheduler || {};
    const agent = status.agent || {};
    const workers = Array.isArray(agent.workers) ? agent.workers : [];
    const registry = status.registry || {};

    lastUpdatedEl.textContent = new Date(status.timestamp).toLocaleTimeString();

    const readyWorkers = workers.filter((worker) => worker.status === "ready").length;
    const allReady = workers.length > 0 && readyWorkers === workers.length;

    apiStripEl.innerHTML = [
        statWithDot("Shell singleton", healthLabel(shellHealth), tone(shellHealth?.ok)),
        statWithDot("API server", healthLabel(apiHealth), tone(apiHealth?.ok)),
        statWithDot(
            "Workers ready",
            workers.length ? `${readyWorkers} of ${workers.length}` : "unavailable",
            workers.length === 0 ? "bad" : allReady ? "ok" : "warn"
        ),
        stat("Active slots", `${scheduler.activeCount || 0} / ${scheduler.limits?.maxSlots || 0}`),
        stat("Queued", `${scheduler.queueLength || 0} / ${scheduler.limits?.maxQueue || 0}`),
        stat("API uptime", `${agent.uptime || 0}s`),
    ].join("");

    processListEl.innerHTML = registryRows(registry);
    const staleCount = registry?.stale?.length || 0;
    processSummaryEl.textContent = staleCount
        ? `${registry.processes.length} up, ${staleCount} stale`
        : `${registry?.processes?.length || 0} registered`;

    workerListEl.innerHTML = workers.length
        ? workers
            .map((worker) => {
                const ready = worker.status === "ready";
                const busy = worker.status === "busy";
                const dot = ready ? "ok" : busy ? "" : "warn";
                return row(`Worker ${worker.id}`, worker.status, dot, ready || busy ? "" : "warn");
            })
            .join("")
        : row("Workers", "unavailable", "bad", "bad");
    workerSummaryEl.textContent = `${readyWorkers}/${workers.length} ready`;

    schedulerGridEl.innerHTML = [
        stat("Active", `${scheduler.activeCount || 0}/${scheduler.limits?.maxSlots || 0}`),
        stat("Queue", `${scheduler.queueLength || 0}/${scheduler.limits?.maxQueue || 0}`),
        stat("Per MFE", scheduler.limits?.maxPerMfe ?? "-"),
        stat("Agent busy", agent.activeSlots ?? 0),
    ].join("");

    const activeEntries = Object.entries(scheduler.activeByMfe || {});
    mfeRequestSummaryEl.textContent = activeEntries.length
        ? `${activeEntries.length} active`
        : `queue ${scheduler.queueLength || 0}`;
    mfeRequestListEl.innerHTML = activeEntries.length
        ? activeEntries.map(([mfeId, count]) => row(mfeId, `${count} active`, "warn")).join("")
        : row("No client is holding a slot", "idle", "ok");

    rawJsonEl.textContent = JSON.stringify(status, null, 2);
}

async function refresh() {
    try {
        const response = await fetch("/status");
        const status = await response.json();
        render(status);
    } catch (err) {
        rawJsonEl.textContent = JSON.stringify({
            error: "dashboard_status_failed",
            message: err instanceof Error ? err.message : "unknown_error",
        }, null, 2);
    }
}

refreshBtn.addEventListener("click", refresh);
refresh();
setInterval(refresh, 2000);
