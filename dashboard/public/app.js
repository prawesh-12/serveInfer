const config = window.DASHBOARD_CONFIG || {};

const refreshBtn = document.getElementById("refreshBtn");
const lastUpdatedEl = document.getElementById("lastUpdated");
const apiStripEl = document.getElementById("apiStrip");
const processListEl = document.getElementById("processList");
const processSummaryEl = document.getElementById("processSummary");
const workerListEl = document.getElementById("workerList");
const workerSummaryEl = document.getElementById("workerSummary");
const schedulerGridEl = document.getElementById("schedulerGrid");
const hardwareGridEl = document.getElementById("hardwareGrid");
const hardwareListEl = document.getElementById("hardwareList");
const hardwareSummaryEl = document.getElementById("hardwareSummary");
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

function gb(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return "-";
    return `${(bytes / 1024 ** 3).toFixed(1)} GB`;
}

function usage(freeBytes, totalBytes) {
    if (!Number.isFinite(totalBytes) || totalBytes <= 0) return "-";
    const used = Math.max(0, totalBytes - (freeBytes || 0));
    const pct = Math.round((used / totalBytes) * 100);
    return `${gb(used)} used · ${gb(freeBytes)} free · ${pct}%`;
}

function mb(value) {
    if (!Number.isFinite(value) || value <= 0) return "-";
    return `${value} MB`;
}

function workerTierRow(assignment, live) {
    const planned = assignment?.backend || "-";
    const running = live?.device || null;
    const label = `Worker ${assignment?.workerId ?? live?.id}`;

    if (!running) {
        return row(label, `planned ${planned} · no request yet`, "warn", "warn");
    }
    if (live.degraded) {
        return row(label, `${planned} → ${running} · ${live.degradedReason || "degraded"}`, "bad", "bad");
    }
    if (running !== planned) {
        return row(label, `planned ${planned} · running ${running}`, "warn", "warn");
    }
    return row(label, `${running}`, "ok");
}

function renderHardware(modelConfig, workers) {
    const hardware = modelConfig?.hardware;
    const capacity = modelConfig?.capacity;
    const assignments = Array.isArray(modelConfig?.assignments) ? modelConfig.assignments : [];

    if (!hardware) {
        hardwareSummaryEl.textContent = "no probe";
        hardwareGridEl.innerHTML = "";
        hardwareListEl.innerHTML = row("No discovery on file", "start the backend to probe", "warn", "warn");
        return;
    }

    const gpus = Array.isArray(hardware.gpus) ? hardware.gpus : [];
    hardwareSummaryEl.textContent = hardware.probeOk
        ? gpus.length
            ? `${gpus.length} gpu${gpus.length > 1 ? "s" : ""}`
            : "cpu only"
        : "probe failed";

    hardwareGridEl.innerHTML = [
        statWithDot("Probe", hardware.probeOk ? "ok" : "failed", hardware.probeOk ? "ok" : "bad"),
        stat("Accelerator", gpus.length ? capacity?.gpuName || gpus[0].name : "none"),
        stat("RAM free", gb(hardware.ramAvailableBytes)),
        stat("Placed", `${modelConfig.workerCount ?? "-"} / ${modelConfig.configuredWorkerCount ?? "-"}`),
    ].join("");

    const rows = [];

    for (const gpu of gpus) {
        rows.push(row(gpu.description || gpu.name, usage(gpu.freeBytes, gpu.totalBytes), "ok"));
        rows.push(row("VRAM total", gb(gpu.totalBytes)));
        rows.push(row("VRAM free", gb(gpu.freeBytes)));
    }
    if (gpus.length === 0) {
        rows.push(row("No GPU found", hardware.note || "cpu only", "warn", "warn"));
    }

    rows.push(row("Host RAM", usage(hardware.ramAvailableBytes, hardware.ramTotalBytes), "ok"));
    rows.push(row("RAM total", gb(hardware.ramTotalBytes)));
    rows.push(row("RAM free", gb(hardware.ramAvailableBytes)));

    if (capacity) {
        if (gpus.length) {
            rows.push(row("GPU budget", `${mb(capacity.usableGpuMb)} usable of ${mb(capacity.freeVramMb)} free`));
        }
        rows.push(row("CPU budget", `${mb(capacity.usableRamMb)} usable of ${mb(capacity.availableRamMb)} available`));
        rows.push(row("Capacity", `${capacity.gpuWorkerCapacity} gpu + ${capacity.cpuWorkerCapacity} cpu worker(s)`));
        if (capacity.gpuReason) rows.push(row("GPU plan", capacity.gpuReason));
        if (capacity.cpuReason) rows.push(row("CPU plan", capacity.cpuReason));
    }

    const liveById = new Map((workers || []).map((worker) => [worker.id, worker]));
    for (const assignment of assignments) {
        rows.push(workerTierRow(assignment, liveById.get(assignment.workerId)));
    }

    hardwareListEl.innerHTML = rows.join("");
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

    renderHardware(status.modelConfig, workers);

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
