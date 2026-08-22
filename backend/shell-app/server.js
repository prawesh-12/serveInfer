'use strict';

const crypto = require('node:crypto');
const express = require('express');
const { numberEnv, requiredEnv } = require('../config/env');
const { getEdgeAgentService } = require('./edgeAgentService');

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
    debug: (...args) => shouldLog('debug') && console.debug('[shell-app]', ...args),
    info: (...args) => shouldLog('info') && console.log('[shell-app]', ...args),
    warn: (...args) => shouldLog('warn') && console.warn('[shell-app]', ...args),
    error: (...args) => shouldLog('error') && console.error('[shell-app]', ...args),
    level: levelName,
  };
}

const app = express();
const service = getEdgeAgentService();
const port = numberEnv('EDGE_SHELL_PORT');
const logger = createLogger(requiredEnv('EDGE_LOG_LEVEL'));
const allowedMfeOrigins = new Set(
  requiredEnv('EDGE_ALLOWED_MFE_ORIGINS')
    .split(',')
    .map((origin) => origin.trim())
    .filter(Boolean)
);

app.use((req, res, next) => {
  const origin = req.headers.origin;
  if (origin && allowedMfeOrigins.has(origin)) {
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Vary', 'Origin');
    res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  }
  if (req.method === 'OPTIONS') {
    res.sendStatus(204);
    return;
  }
  next();
});
app.use(express.json({ limit: '2mb' }));

function makeRequestId() {
  return crypto.randomUUID();
}

function normalizePriority(input) {
  const value = String(input || 'normal').toLowerCase();
  if (value === 'high') return 'high';
  if (value === 'low') return 'low';
  return 'normal';
}

app.get('/', (_req, res) => {
  res.json({
    service: 'shell-app',
    role: 'singleton_scheduler_facade',
    api: {
      infer: '/api/infer',
      stream: '/api/stream',
      cancel: '/api/cancel',
      queueStatus: '/api/queue-status',
      health: '/api/health',
      agentHealth: '/api/agent-health',
    },
  });
});

app.post('/api/infer', async (req, res) => {
  const prompt = typeof req.body?.prompt === 'string' ? req.body.prompt.trim() : '';
  const mfeId = req.body?.mfeId ? String(req.body.mfeId) : 'doc-qa';
  const priority = normalizePriority(req.body?.priority);
  const requestId = req.body?.requestId ? String(req.body.requestId) : makeRequestId();

  if (!prompt) {
    res.status(400).json({ error: 'prompt_required' });
    return;
  }

  try {
    const handle = service.submitInference({ requestId, prompt, mfeId, priority });
    const result = await handle.promise;
    res.json({
      requestId,
      result: String(result.result || ''),
      device: String(result.device || 'cpu'),
      degraded: Boolean(result.degraded),
      degradedReason: result.degradedReason || null,
    });
  } catch (err) {
    const status = Number(err?.status || 502);
    const payload = err?.payload;
    if (status === 503 && payload?.error === 'worker_crashed') {
      res.status(503).json({
        error: 'worker_crashed',
        retryAfterSeconds: Number(payload.retryAfterSeconds || 2),
        requestId,
      });
      return;
    }
    if (err?.name === 'SchedulerError' && err.code === 'request_cancelled') {
      res.status(499).json({ error: 'request_cancelled', requestId });
      return;
    }
    if (err?.name === 'SchedulerError' && err.code === 'scheduler_overloaded') {
      res.status(429).json({ error: 'scheduler_overloaded', requestId });
      return;
    }
    if (err?.name === 'SchedulerError' && err.code === 'queue_timeout') {
      res.status(408).json({
        error: 'queue_timeout',
        retryable: true,
        requestId,
      });
      return;
    }
    if (err?.name === 'SchedulerError' && err.code === 'exec_timeout') {
      res.status(504).json({
        error: 'exec_timeout',
        ranMs: Number(err.details?.ranMs || 0),
        retryable: true,
        requestId,
      });
      return;
    }
    res.status(status).json({
      error: payload?.error || 'infer_failed',
      message: err instanceof Error ? err.message : 'unknown_error',
      requestId,
    });
  }
});

app.get('/api/stream', async (req, res) => {
  const prompt = typeof req.query?.prompt === 'string' ? req.query.prompt.trim() : '';
  const mfeId = req.query?.mfeId ? String(req.query.mfeId) : 'meeting-summary';
  const priority = normalizePriority(req.query?.priority || 'high');
  const requestId = req.query?.requestId ? String(req.query.requestId) : makeRequestId();

  if (!prompt) {
    res.status(400).json({ error: 'prompt_required' });
    return;
  }

  res.setHeader('Content-Type', 'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection', 'keep-alive');
  res.setHeader('X-Accel-Buffering', 'no');
  res.flushHeaders?.();

  const sendSse = (event, data) => {
    res.write(`event: ${event}\n`);
    res.write(`data: ${JSON.stringify(data)}\n\n`);
  };

  let requestClosed = false;
  let handle;
  const closeHandler = () => {
    requestClosed = true;
    if (handle) {
      handle.cancel();
    } else {
      service.cancel(requestId);
    }
  };
  req.on('close', closeHandler);
  req.on('aborted', closeHandler);

  try {
    handle = service.submitStream({
      requestId,
      prompt,
      mfeId,
      priority,
      onStatus: (status) => {
        if (status.state === 'queued') {
          sendSse('queued', {
            requestId,
            position: status.position,
            estimatedWaitMs: status.estimatedWaitMs,
          });
        } else if (status.state === 'started') {
          sendSse('started', { requestId });
        } else if (status.state === 'cancelled') {
          sendSse('cancelled', {
            requestId,
            reason: status.reason || 'user_cancelled',
          });
        } else if (status.state === 'timeout') {
          sendSse('timeout', {
            requestId,
            phase: status.phase || 'queue',
            waitedMs: status.waitedMs,
            ranMs: status.ranMs,
            retryable: Boolean(status.retryable),
          });
        }
      },
      onToken: (payload) => {
        sendSse('token', payload);
      },
      onDone: (payload) => {
        sendSse('done', payload);
      },
    });

    await handle.promise;
    if (!requestClosed) {
      res.end();
    }
  } catch (err) {
    if (!requestClosed) {
      const payload = {
        error: err?.payload?.error || 'stream_failed',
        message: err instanceof Error ? err.message : 'unknown_error',
        requestId,
      };
      if (err?.payload?.retryAfterSeconds) {
        payload.retryAfterSeconds = Number(err.payload.retryAfterSeconds);
      }
      if (err?.name === 'SchedulerError') {
        payload.error = err.code || payload.error;
        payload.retryable = Boolean(err.details?.retryable);
      }
      sendSse('error', payload);
      res.end();
    }
  } finally {
    req.off('close', closeHandler);
    req.off('aborted', closeHandler);
  }
});

app.post('/api/cancel', (req, res) => {
  const requestId = req.body?.requestId ? String(req.body.requestId) : '';
  if (!requestId) {
    res.status(400).json({ error: 'requestId_required' });
    return;
  }
  const result = service.cancel(requestId);
  res.json({ requestId, ...result });
});

app.get('/api/queue-status', (req, res) => {
  const requestId = req.query?.requestId ? String(req.query.requestId) : '';
  if (!requestId) {
    res.status(400).json({ error: 'requestId_required' });
    return;
  }
  const status = service.getQueueStatus(requestId);
  res.json(status);
});

app.get('/api/health', (_req, res) => {
  res.json(service.getSnapshot());
});

app.get('/api/agent-health', async (_req, res) => {
  try {
    res.json(await service.getAgentHealth());
  } catch (err) {
    res.status(Number(err?.status || 503)).json({
      error: err?.payload?.error || 'agent_unreachable',
      message: err instanceof Error ? err.message : 'unknown_error',
    });
  }
});

// Nothing in this stack authenticates, so nothing binds a public interface.
app.listen(port, '127.0.0.1', () => {
  logger.info(`listening on http://127.0.0.1:${port} (log level: ${logger.level})`);
});
