'use strict';

const path = require('node:path');
const crypto = require('node:crypto');
const express = require('express');
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
const port = Number(process.env.EDGE_SHELL_PORT || 3000);
const publicDir = path.join(__dirname, 'public');
const logger = createLogger(process.env.EDGE_LOG_LEVEL);

app.use(express.json({ limit: '2mb' }));
app.use(express.static(publicDir));

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
  res.sendFile(path.join(publicDir, 'shell.html'));
});

app.get('/mfe-doc-qa', (_req, res) => {
  res.sendFile(path.join(publicDir, 'mfe-doc-qa.html'));
});

app.get('/mfe-meeting-summary', (_req, res) => {
  res.sendFile(path.join(publicDir, 'mfe-meeting-summary.html'));
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

app.listen(port, () => {
  logger.info(`listening on http://127.0.0.1:${port} (log level: ${logger.level})`);
});
