'use strict';

const { randomUUID } = require('node:crypto');
const { WorkerPoolError } = require('../ipc');

function sendSse(res, event, payload) {
  res.write(`event: ${event}\n`);
  res.write(`data: ${JSON.stringify(payload)}\n\n`);
}

function sanitizePrompt(value) {
  if (typeof value !== 'string') {
    return '';
  }
  return value.trim();
}

function registerInferRoutes(app, workerPool) {
  app.post('/infer', async (req, res) => {
    const prompt = sanitizePrompt(req.body?.prompt);
    const mfeId = req.body?.mfeId ? String(req.body.mfeId) : '';
    const requestId = req.body?.requestId ? String(req.body.requestId) : randomUUID();

    if (!prompt) {
      res.status(400).json({ error: 'prompt_required' });
      return;
    }

    try {
      const result = await workerPool.runInference({ prompt, requestId, mfeId });
      res.setHeader('X-Inference-Device', String(result.device || 'cpu'));
      res.setHeader('X-Inference-Degraded', String(Boolean(result.degraded)));
      res.json({
        requestId,
        result: result.text,
        device: result.device,
        degraded: result.degraded,
      });
    } catch (err) {
      if (err instanceof WorkerPoolError && err.code === 'worker_crashed') {
        res.status(503).json({
          error: 'worker_crashed',
          retryAfterSeconds: Number(err.details?.retryAfterSeconds ?? 2),
          requestId,
        });
        return;
      }
      if (err instanceof WorkerPoolError && err.code === 'no_ready_workers') {
        res.status(503).json({
          error: 'no_ready_workers',
          retryAfterSeconds: Number(err.details?.retryAfterSeconds ?? 1),
          requestId,
        });
        return;
      }
      res.status(502).json({
        error: 'worker_unavailable',
        message: err instanceof Error ? err.message : 'unknown_error',
        requestId,
      });
    }
  });

  app.get('/infer/stream', async (req, res) => {
    const prompt = sanitizePrompt(req.query.prompt);
    const mfeId = req.query.mfeId ? String(req.query.mfeId) : '';
    const requestId = req.query.requestId ? String(req.query.requestId) : randomUUID();

    if (!prompt) {
      res.status(400).json({ error: 'prompt_required' });
      return;
    }

    res.setHeader('Content-Type', 'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.setHeader('Connection', 'keep-alive');
    res.setHeader('X-Accel-Buffering', 'no');
    res.flushHeaders?.();

    let streamHandle;
    const abort = () => {
      if (streamHandle?.cancel) {
        streamHandle.cancel();
      }
    };
    req.on('close', abort);
    req.on('aborted', abort);

    try {
      streamHandle = workerPool.runStreamingInference({
        prompt,
        requestId,
        mfeId,
        onToken: (token) => sendSse(res, 'token', { requestId, token }),
      });
      const result = await streamHandle.promise;
      sendSse(res, 'done', {
        requestId,
        result: result.text,
        device: result.device,
        degraded: result.degraded,
      });
      res.end();
    } catch (err) {
      if (err instanceof WorkerPoolError && err.code === 'worker_crashed') {
        if (!res.headersSent) {
          res.status(503).json({
            error: 'worker_crashed',
            retryAfterSeconds: Number(err.details?.retryAfterSeconds ?? 2),
            requestId,
          });
        } else {
          sendSse(res, 'error', {
            error: 'worker_crashed',
            retryAfterSeconds: Number(err.details?.retryAfterSeconds ?? 2),
            requestId,
          });
          res.end();
        }
        return;
      }

      if (!res.headersSent) {
        res.status(502).json({
          error: 'worker_unavailable',
          message: err instanceof Error ? err.message : 'unknown_error',
          requestId,
        });
      } else {
        sendSse(res, 'error', {
          error: 'worker_unavailable',
          message: err instanceof Error ? err.message : 'unknown_error',
          requestId,
        });
        res.end();
      }
    } finally {
      req.off('close', abort);
      req.off('aborted', abort);
    }
  });
}

module.exports = {
  registerInferRoutes,
};
