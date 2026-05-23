const config = window.DASHBOARD_CONFIG || {};

const refreshBtn = document.getElementById("refreshBtn");
const lastUpdatedEl = document.getElementById("lastUpdated");
const apiGridEl = document.getElementById("apiGrid");
const processListEl = document.getElementById("processList");
const processSummaryEl = document.getElementById("processSummary");
const workerListEl = document.getElementById("workerList");
const workerSummaryEl = document.getElementById("workerSummary");
const schedulerGridEl = document.getElementById("schedulerGrid");
const mfeRequestListEl = document.getElementById("mfeRequestList");
const mfeStatusListEl = document.getElementById("mfeStatusList");
const mfeRequestSummaryEl = document.getElementById("mfeRequestSummary");
const rawJsonEl = document.getElementById("rawJson");

function setLink(id, url) {
    const el = document.getElementById(id);
    if (el && url) el.href = url;
}

setLink("docQaLink", config.docQaMfeUrl);
setLink("meetingLink", config.meetingMfeUrl);

function healthLabel(check) {
    if (!check) return "unknown";
    if (check.ok) return check.status ? `online ${check.status}` : "online";
    return check.error || "offline";
}

function pill(ok) {
    return ok ? "ok" : "bad";
}

function metric(label, value, tone = "") {
    return `<div class="metric ${tone}"><span>${label}</span><strong>${value}</strong></div>`;
}

function processRows(title, processes) {
    if (!processes || processes.length === 0) {
        return `<div class="row bad"><strong>${title}</strong><span>not running</span></div>`;
    }
    return processes
        .map((proc) => `<div class="row ok"><strong>${title}</strong><span>pid ${proc.pid} ${proc.stat} ${proc.etime}</span></div>`)
        .join("");
}

function render(status) {
    const shellHealth = status.endpoints?.shell?.health;
    const apiHealth = status.endpoints?.api?.health;
    const meetingHealth = status.endpoints?.meetingMfe?.health;
    const docHealth = status.endpoints?.documentQaMfe?.health;
    const scheduler = status.scheduler || {};
    const agent = status.agent || {};
    const workers = Array.isArray(agent.workers) ? agent.workers : [];
    const processGroups = status.processes || {};

    lastUpdatedEl.textContent = new Date(status.timestamp).toLocaleTimeString();
    apiGridEl.innerHTML = [
        metric("Shell singleton", healthLabel(shellHealth), pill(shellHealth?.ok)),
        metric("API server", healthLabel(apiHealth), pill(apiHealth?.ok)),
        metric("Agent health", workers.length ? "available" : "unavailable", workers.length ? "ok" : "bad"),
        metric("API uptime", `${agent.uptime || 0}s`),
    ].join("");

    const coreProcessHtml = [
        processRows("Supervisor", processGroups.supervisor),
        processRows("Model cache", processGroups.modelCache),
        processRows("API server", processGroups.apiServer),
        processRows("Shell app", processGroups.shellApp),
        processRows("System dashboard", processGroups.statusDashboard),
        processRows("Meeting MFE", processGroups.meetingMfe),
        processRows("Document Q&A MFE", processGroups.documentQaMfe),
    ].join("");
    processListEl.innerHTML = coreProcessHtml;
    const processCount = Object.values(processGroups).reduce((sum, group) => sum + (Array.isArray(group) ? group.length : 0), 0);
    processSummaryEl.textContent = `${processCount} tracked`;

    workerListEl.innerHTML = workers.length
        ? workers.map((worker) => `<div class="row ${worker.status === "ready" ? "ok" : "warn"}"><strong>Worker ${worker.id}</strong><span>${worker.status}</span></div>`).join("")
        : `<div class="row bad"><strong>Workers</strong><span>unavailable</span></div>`;
    const readyWorkers = workers.filter((worker) => worker.status === "ready").length;
    workerSummaryEl.textContent = `${readyWorkers}/${workers.length} ready`;

    schedulerGridEl.innerHTML = [
        metric("Active slots", `${scheduler.activeCount || 0}/${scheduler.limits?.maxSlots || 0}`),
        metric("Queue length", `${scheduler.queueLength || 0}/${scheduler.limits?.maxQueue || 0}`),
        metric("Per-MFE cap", scheduler.limits?.maxPerMfe ?? "-"),
        metric("Worker active slots", agent.activeSlots ?? 0),
    ].join("");

    const activeByMfe = scheduler.activeByMfe || {};
    const activeEntries = Object.entries(activeByMfe);
    mfeRequestSummaryEl.textContent = `${activeEntries.length} active MFE groups`;
    mfeRequestListEl.innerHTML = activeEntries.length
        ? activeEntries.map(([mfeId, count]) => `<div class="row warn"><strong>${mfeId}</strong><span>${count} active</span></div>`).join("")
        : `<div class="row ok"><strong>No active MFE requests</strong><span>queue ${scheduler.queueLength || 0}</span></div>`;

    mfeStatusListEl.innerHTML = [
        `<div class="row ${pill(meetingHealth?.ok)}"><strong>Meeting Summariser</strong><span>${healthLabel(meetingHealth)}</span></div>`,
        `<div class="row ${pill(docHealth?.ok)}"><strong>Document Q&A</strong><span>${healthLabel(docHealth)}</span></div>`,
    ].join("");

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
