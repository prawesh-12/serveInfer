'use strict';

const { randomUUID } = require('node:crypto');
const { numberEnv, requiredEnv } = require('../../config/env');
const { WorkerPoolError } = require('../ipc');
const { RequestRegistry } = require('../requestRegistry');

const registry = new RequestRegistry({
  inflightPath: requiredEnv('EDGE_INFLIGHT_PATH'),
  idempotencyTtlMs: numberEnv('EDGE_IDEMPOTENCY_TTL_MS'),
});

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

    const attempt = registry.run({
      requestId,
      mfeId,
      stream: false,
      execute: () => workerPool.runInference({ prompt, requestId, mfeId }),
    });

    try {
      const result = await attempt.promise;
      res.setHeader('X-Idempotent-Replay', String(attempt.replay));
      res.setHeader('X-Inference-Device', String(result.device || 'cpu'));
      res.setHeader('X-Inference-Degraded', String(Boolean(result.degraded)));
      res.setHeader('X-Latency-Mode', result.degraded ? 'degraded' : 'normal');
      if (result.degradedReason) {
        res.setHeader('X-Degraded-Reason', String(result.degradedReason));
      }
      res.json({
        requestId,
        result: result.text,
        device: result.device,
        degraded: result.degraded,
        degradedReason: result.degradedReason,
        replay: attempt.replay,
      });
    } catch (err) {
      if (err instanceof WorkerPoolError && err.code === 'worker_crashed') {
        const retryAfterSeconds = Number(err.details?.retryAfterSeconds ?? 2);
        res.setHeader('Retry-After', String(retryAfterSeconds));
        res.setHeader('X-Edge-Error', 'worker_crash');
        res.status(503).json({
          error: 'worker_crashed',
          retryAfterSeconds,
          requestId,
        });
        return;
      }
      if (err instanceof WorkerPoolError && err.code === 'no_ready_workers') {
        const retryAfterSeconds = Number(err.details?.retryAfterSeconds ?? 1);
        res.setHeader('Retry-After', String(retryAfterSeconds));
        res.setHeader('X-Edge-Error', 'no_ready_workers');
        res.status(503).json({
          error: 'no_ready_workers',
          retryAfterSeconds,
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

    const prior = registry.lookup(requestId);
    if (prior.state === 'inflight') {
      res.status(409).json({ error: 'request_in_flight', requestId });
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
      if (prior.state === 'completed') {
        // Tokens cannot be re-sent, so replay the finished answer alone.
        sendSse(res, 'done', {
          requestId,
          result: prior.result.text,
          device: prior.result.device,
          degraded: prior.result.degraded,
          degradedReason: prior.result.degradedReason,
          replay: true,
        });
        res.end();
        return;
      }

      const attempt = registry.run({
        requestId,
        mfeId,
        stream: true,
        execute: () => {
          streamHandle = workerPool.runStreamingInference({
            prompt,
            requestId,
            mfeId,
            onToken: (token) => sendSse(res, 'token', { requestId, token }),
          });
          return streamHandle.promise;
        },
      });

      const result = await attempt.promise;
      sendSse(res, 'done', {
        requestId,
        result: result.text,
        device: result.device,
        degraded: result.degraded,
        degradedReason: result.degradedReason,
        replay: attempt.replay,
      });
      res.end();
    } catch (err) {
      if (err instanceof WorkerPoolError && err.code === 'worker_crashed') {
        const retryAfterSeconds = Number(err.details?.retryAfterSeconds ?? 2);
        if (!res.headersSent) {
          res.setHeader('Retry-After', String(retryAfterSeconds));
          res.setHeader('X-Edge-Error', 'worker_crash');
          res.status(503).json({
            error: 'worker_crashed',
            retryAfterSeconds,
            requestId,
          });
        } else {
          sendSse(res, 'error', {
            error: 'worker_crashed',
            retryAfterSeconds,
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
  registry,
};
