'use strict';

const { execFile } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { numberEnv, requiredEnv } = require('../config/env');

const root = path.resolve(__dirname, '..');
const publicDir = path.join(__dirname, 'public');
const port = numberEnv('EDGE_STATUS_DASHBOARD_PORT');

const shellBase = requiredEnv('EDGE_SHELL_PUBLIC_BASE');
const apiBase = requiredEnv('EDGE_API_BASE');
const meetingMfeUrl = requiredEnv('EDGE_MEETING_MFE_URL');
const docQaMfeUrl = requiredEnv('EDGE_DOC_QA_MFE_URL');
const stateDir = requiredEnv('EDGE_STATE_DIR');

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

function checkHttp(url, timeoutMs = 1200) {
  return new Promise((resolve) => {
    const req = http.get(url, { timeout: timeoutMs }, (res) => {
      res.resume();
      res.on('end', () => {
        resolve({
          ok: res.statusCode >= 200 && res.statusCode < 400,
          status: res.statusCode,
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

function listProcesses() {
  return new Promise((resolve) => {
    execFile('ps', ['-eo', 'pid=,ppid=,stat=,etime=,cmd='], { maxBuffer: 1024 * 1024 }, (err, stdout) => {
      if (err) {
        resolve([]);
        return;
      }
      const rows = stdout
        .split('\n')
        .map((line) => line.trim())
        .filter(Boolean)
        .map((line) => {
          const match = line.match(/^(\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(.+)$/);
          if (!match) return null;
          return {
            pid: Number(match[1]),
            ppid: Number(match[2]),
            stat: match[3],
            etime: match[4],
            cmd: match[5],
          };
        })
        .filter(Boolean);
      resolve(rows);
    });
  });
}

function readProcCommand(pid) {
  try {
    const raw = fs.readFileSync(`/proc/${pid}/cmdline`, 'utf8');
    return raw.split('\0').filter(Boolean).join(' ');
  } catch {
    return '';
  }
}

function isPidAlive(pid) {
  if (!Number.isInteger(pid) || pid <= 0) {
    return false;
  }
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

function readPidFile(name) {
  try {
    const raw = fs.readFileSync(path.join(stateDir, name), 'utf8').trim();
    const pid = Number(raw);
    if (!isPidAlive(pid)) {
      return null;
    }
    return {
      pid,
      ppid: null,
      stat: 'alive',
      etime: '-',
      cmd: readProcCommand(pid) || `${name.replace(/\.pid$/, '')} pidfile`,
      source: 'pidfile',
    };
  } catch {
    return null;
  }
}

function addUnique(group, proc) {
  if (!proc || group.some((item) => item.pid === proc.pid)) {
    return;
  }
  group.push(proc);
}

function commandMatches(cmd, exactPath, executableName, pathPart) {
  if (!cmd) {
    return false;
  }
  if (cmd.includes(exactPath)) {
    return true;
  }
  if (pathPart && cmd.includes(pathPart)) {
    return true;
  }
  if (!executableName) {
    return false;
  }
  return new RegExp(`(^|[/\\s])${executableName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(\\s|$)`).test(cmd);
}

function classifyProcesses(processes) {
  const specs = [
    {
      name: 'supervisor',
      exactPath: `${root}/build/supervisor/edge-supervisor`,
      executableName: 'edge-supervisor',
      pidFile: 'supervisor.pid',
    },
    {
      name: 'modelCache',
      exactPath: `${root}/build/model-cache/edge-model-cache`,
      executableName: 'edge-model-cache',
    },
    {
      name: 'apiServer',
      exactPath: `${root}/api-server/server.js`,
      executableName: null,
      pathPart: 'api-server/server.js',
    },
    {
      name: 'shellApp',
      exactPath: `${root}/shell-app/server.js`,
      executableName: null,
      pathPart: 'shell-app/server.js',
      pidFile: 'shell.pid',
    },
    {
      name: 'statusDashboard',
      exactPath: `${root}/system-dashboard/server.js`,
      executableName: null,
      pathPart: 'system-dashboard/server.js',
      pidFile: 'status-dashboard.pid',
    },
    {
      name: 'meetingMfe',
      exactPath: `${root}/mfes/meeting-summary/server.js`,
      executableName: null,
      pathPart: 'mfes/meeting-summary/server.js',
      pidFile: 'meeting-mfe.pid',
    },
    {
      name: 'documentQaMfe',
      exactPath: `${root}/mfes/document-qa/server.js`,
      executableName: null,
      pathPart: 'mfes/document-qa/server.js',
      pidFile: 'doc-qa-mfe.pid',
    },
  ];

  const result = Object.fromEntries(specs.map(({ name }) => [name, []]));
  result.workers = [];

  for (const proc of processes) {
    if (
      commandMatches(
        proc.cmd,
        `${root}/build/inference-worker/edge-inference-worker`,
        'edge-inference-worker'
      )
    ) {
      result.workers.push(proc);
      continue;
    }
    for (const spec of specs) {
      if (commandMatches(proc.cmd, spec.exactPath, spec.executableName, spec.pathPart)) {
        result[spec.name].push(proc);
        break;
      }
    }
  }

  for (const spec of specs) {
    if (spec.pidFile) {
      addUnique(result[spec.name], readPidFile(spec.pidFile));
    }
  }

  return result;
}

async function buildStatus() {
  const [scheduler, agent, api, meetingMfe, documentQaMfe, processes] = await Promise.all([
    getJson(`${shellBase}/api/health`),
    getJson(`${shellBase}/api/agent-health`),
    getJson(`${apiBase}/health`),
    checkHttp(meetingMfeUrl),
    checkHttp(docQaMfeUrl),
    listProcesses(),
  ]);

  return {
    timestamp: new Date().toISOString(),
    endpoints: {
      shell: { url: shellBase, health: scheduler },
      api: { url: apiBase, health: api },
      meetingMfe: { url: meetingMfeUrl, health: meetingMfe },
      documentQaMfe: { url: docQaMfeUrl, health: documentQaMfe },
    },
    scheduler: scheduler.body || null,
    agent: agent.body || null,
    processes: classifyProcesses(processes),
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
      `window.DASHBOARD_CONFIG = ${JSON.stringify({ shellBase, apiBase, meetingMfeUrl, docQaMfeUrl })};\n`,
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
  console.log(`[system-dashboard] listening on http://127.0.0.1:${port}`);
});
