'use strict';

const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
// Imports nothing from the backend. Every value has a default, so this runs
// standalone against a local stack.
const publicDir = path.join(__dirname, 'public');
const port = Number(process.env.DASHBOARD_PORT || 3001);
const shellBase = process.env.SHELL_API_BASE || 'http://127.0.0.1:3000';
const apiBase = process.env.AGENT_API_BASE || 'http://127.0.0.1:11434';
const stateDir = process.env.EDGE_STATE_DIR || '/tmp/edge-runtime';

const contentTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
};

function send(res, status, body, contentType = 'application/json; charset=utf-8') {
  res.writeHead(status, { 'Content-Type': contentType });
  res.end(body);
}

function getJson(url, timeoutMs = 1200) {
  return new Promise((resolve) => {
    const req = http.get(url, { timeout: timeoutMs }, (res) => {
      let raw = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => {
        raw += chunk;
      });
      res.on('end', () => {
        let body = null;
        try {
          body = raw ? JSON.parse(raw) : null;
        } catch {
          body = raw;
        }
        resolve({
          ok: res.statusCode >= 200 && res.statusCode < 300,
          status: res.statusCode,
          body,
        });
      });
    });
    req.on('timeout', () => {
      req.destroy(new Error('timeout'));
    });
    req.on('error', (err) => {
      resolve({ ok: false, status: 0, error: err.message });
    });
  });
}

// $EDGE_STATE_DIR is the process list. Every process writes <name>.pid on start
// and removes it on exit, so a new client joins by writing one file and nothing
// here changes. The name prefix carries the tier.
function tierOf(name) {
  if (name.startsWith('backend-')) return { tier: 'backend', label: name.slice('backend-'.length) };
  if (name.startsWith('client-')) return { tier: 'clients', label: name.slice('client-'.length) };
  if (name === 'dashboard') return { tier: 'dashboard', label: 'dashboard' };
  return { tier: 'other', label: name };
}

function isPidAlive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    // EPERM means it exists and belongs to someone else, which still counts.
    return err.code === 'EPERM';
  }
}

function formatUptime(ms) {
  const seconds = Math.max(0, Math.floor(ms / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ${seconds % 60}s`;
  const hours = Math.floor(minutes / 60);
  return `${hours}h ${minutes % 60}m`;
}

function readRegistry() {
  let entries;
  try {
    entries = fs.readdirSync(stateDir).filter((name) => name.endsWith('.pid'));
  } catch {
    return { processes: [], stale: [], stateDir };
  }

  const processes = [];
  const stale = [];

  for (const file of entries.sort()) {
    const name = file.slice(0, -'.pid'.length);
    const { tier, label } = tierOf(name);
    let pid = 0;
    let startedAt = null;
    try {
      pid = Number(fs.readFileSync(path.join(stateDir, file), 'utf8').trim());
      startedAt = fs.statSync(path.join(stateDir, file)).mtimeMs;
    } catch {
      continue;
    }
    if (!Number.isInteger(pid) || pid <= 0) {
      continue;
    }
    if (!isPidAlive(pid)) {
      // Worth showing: usually a crash nothing cleaned up after.
      stale.push({ name, tier, label, pid });
      continue;
    }
    processes.push({
      name,
      tier,
      label,
      pid,
      uptime: startedAt ? formatUptime(Date.now() - startedAt) : '-',
    });
  }

  return { processes, stale, stateDir };
}

async function buildStatus() {
  const [scheduler, agent, api] = await Promise.all([
    getJson(`${shellBase}/api/health`),
    getJson(`${shellBase}/api/agent-health`),
    getJson(`${apiBase}/health`),
  ]);
  const registry = readRegistry();

  return {
    timestamp: new Date().toISOString(),
    endpoints: {
      shell: { url: shellBase, health: scheduler },
      api: { url: apiBase, health: api },
    },
    scheduler: scheduler.body || null,
    agent: agent.body || null,
    registry,
  };
}

function serveStatic(req, res) {
  const requestUrl = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
  if (requestUrl.pathname === '/status') {
    buildStatus()
      .then((status) => send(res, 200, JSON.stringify(status)))
      .catch((err) => send(res, 500, JSON.stringify({ error: err.message })));
    return;
  }

  if (requestUrl.pathname === '/dashboard-config.js') {
    send(
      res,
      200,
      `window.DASHBOARD_CONFIG = ${JSON.stringify({ shellBase, apiBase })};\n`,
      'text/javascript; charset=utf-8'
    );
    return;
  }

  const pathname = requestUrl.pathname === '/' ? '/index.html' : requestUrl.pathname;
  const filePath = path.normalize(path.join(publicDir, pathname));
  if (!filePath.startsWith(publicDir)) {
    send(res, 403, 'forbidden', 'text/plain; charset=utf-8');
    return;
  }

  fs.readFile(filePath, (err, content) => {
    if (err) {
      send(res, 404, 'not_found', 'text/plain; charset=utf-8');
      return;
    }
    send(res, 200, content, contentTypes[path.extname(filePath)] || 'application/octet-stream');
  });
}

http.createServer(serveStatic).listen(port, '127.0.0.1', () => {
  console.log(`[dashboard] listening on http://127.0.0.1:${port}`);
});
