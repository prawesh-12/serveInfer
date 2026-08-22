'use strict';

const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const express = require('express');
const { loadEnv, numberEnv, requiredEnv } = require('../config/env');
const { WorkerPool } = require('./ipc');
const { resolveWorkerCount, logWorkerCountDecision } = require('./workerCountSource');
const { registerInferRoutes, registry } = require('./routes/infer');

const LOG_LEVELS = {
  debug: 10,
  info: 20,
  warn: 30,
  error: 40,
};

function createLogger(rawLevel) {
  const levelName = String(rawLevel || 'info').toLowerCase();
  const threshold = LOG_LEVELS[levelName] || LOG_LEVELS.info;
  const shouldLog = (level) => LOG_LEVELS[level] >= threshold;
  return {
    debug: (...args) => shouldLog('debug') && console.debug('[api-server]', ...args),
    info: (...args) => shouldLog('info') && console.log('[api-server]', ...args),
    warn: (...args) => shouldLog('warn') && console.warn('[api-server]', ...args),
    error: (...args) => shouldLog('error') && console.error('[api-server]', ...args),
    level: levelName,
  };
}

function parseArgs(argv) {
  const options = {};
  for (let i = 2; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg.startsWith('--') && arg.includes('=')) {
      const idx = arg.indexOf('=');
      const key = arg.slice(2, idx);
      const value = arg.slice(idx + 1);
      options[key] = value;
      continue;
    }
    if (arg.startsWith('--')) {
      const key = arg.slice(2);
      const next = argv[i + 1];
      if (next && !next.startsWith('--')) {
        options[key] = next;
        i += 1;
      } else {
        options[key] = '1';
      }
    }
  }
  return options;
}

function parseSupervisorMessage(data) {
  try {
    return JSON.parse(data);
  } catch {
    return null;
  }
}

function startSupervisorIpcListener(socketPath, workerPool, logger) {
  try {
    if (fs.existsSync(socketPath)) {
      fs.unlinkSync(socketPath);
    }
  } catch (err) {
    logger.error(`failed to unlink existing socket ${socketPath}:`, err.message);
  }

  const server = net.createServer((socket) => {
    let buffer = '';
    socket.on('data', (chunk) => {
      buffer += chunk.toString('utf8');
      while (true) {
        const idx = buffer.indexOf('\n');
        if (idx < 0) {
          break;
        }
        const line = buffer.slice(0, idx).trim();
        buffer = buffer.slice(idx + 1);
        if (!line) {
          continue;
        }
        const msg = parseSupervisorMessage(line);
        if (msg) {
          workerPool.handleSupervisorMessage(msg);
        }
      }
    });
  });

  server.on('error', (err) => {
    logger.error('supervisor IPC listener error:', err.message);
  });

  server.listen(socketPath, () => {
    logger.info(`supervisor notify listener on ${socketPath}`);
  });

  const cleanup = () => {
    server.close(() => {
      try {
        if (fs.existsSync(socketPath)) {
          fs.unlinkSync(socketPath);
        }
      } catch {
      }
    });
  };

  process.on('SIGTERM', cleanup);
  process.on('SIGINT', cleanup);
  process.on('exit', cleanup);
}

const args = parseArgs(process.argv);
const port = Number(args.port || numberEnv('EDGE_API_PORT'));
loadEnv();
const workerCountDecision = resolveWorkerCount({
  configuredCount: numberEnv('EDGE_WORKER_COUNT'),
  modelConfigPath: args['model-config'] || process.env.EDGE_MODEL_CONFIG_PATH || null,
});
const workerCount = workerCountDecision.workerCount;
const workerSocketPrefix = requiredEnv('EDGE_WORKER_SOCKET_PREFIX');
const workerConnectTimeoutMs = numberEnv('EDGE_WORKER_CONNECT_TIMEOUT_MS');
const workerRecoveryMs = numberEnv('EDGE_WORKER_RECOVERY_MS');
const workerRecoveryAttempts = numberEnv('EDGE_WORKER_RECOVERY_ATTEMPTS');
const supervisorSocketPath =
  args['supervisor-socket'] || requiredEnv('EDGE_API_NOTIFY_SOCK');
const logger = createLogger(requiredEnv('EDGE_LOG_LEVEL'));

logWorkerCountDecision(logger, workerCountDecision);

const app = express();
app.use(express.json({ limit: '2mb' }));

const workerPool = new WorkerPool({
  workerCount,
  workerSocketPrefix,
  connectTimeoutMs: workerConnectTimeoutMs,
  recoveryMs: workerRecoveryMs,
  recoveryAttempts: workerRecoveryAttempts,
});

registerInferRoutes(app, workerPool);

app.get('/health', (_req, res) => {
  const health = workerPool.getHealth();
  const requests = registry.snapshot();
  res.json({
    workers: health.workers.map((w) => ({ id: w.id, status: w.status })),
    activeSlots: health.activeSlots,
    uptime: Math.floor(health.uptimeMs / 1000),
    requests: {
      inflight: requests.inflight,
      completedCached: requests.completedCount,
      orphanedFromPreviousRun: requests.orphanedFromPreviousRun,
    },
  });
});

app.use((err, _req, res, _next) => {
  logger.error('unhandled route error:', err);
  res.status(500).json({ error: 'internal_error' });
});

startSupervisorIpcListener(path.resolve(supervisorSocketPath), workerPool, logger);

app.listen(port, '127.0.0.1', () => {
  logger.info(`listening on http://127.0.0.1:${port} (log level: ${logger.level})`);
});
