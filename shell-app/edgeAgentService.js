'use strict';

const { Scheduler } = require('./scheduler');

class EdgeAgentService {
  constructor() {
    this.apiBase = process.env.EDGE_API_BASE || 'http://127.0.0.1:11434';
    this.scheduler = new Scheduler({
      maxSlots: Number(process.env.EDGE_MAX_SLOTS || 4),
      maxPerMfe: Number(process.env.EDGE_MAX_PER_MFE || 2),
      maxQueue: Number(process.env.EDGE_MAX_QUEUE || 20),
      agingMs: Number(process.env.EDGE_AGING_MS || 15_000),
      queueTimeoutMs: Number(process.env.EDGE_QUEUE_TIMEOUT_MS || 30_000),
      defaultDurationMs: Number(process.env.EDGE_DEFAULT_JOB_MS || 8_000),
    });
  }

  submitInference({ requestId, prompt, mfeId, priority, onStatus }) {
    return this.scheduler.enqueue({
      requestId,
      mfeId,
      priority,
      type: 'infer',
      onStatus,
      execute: async ({ signal }) => {
        const response = await fetch(`${this.apiBase}/infer`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ requestId, prompt, mfeId }),
          signal,
        });
        const body = await response.json().catch(() => ({}));
        if (!response.ok) {
          const error = new Error(body.error || 'api_infer_failed');
          error.status = response.status;
          error.payload = body;
          throw error;
        }
        return body;
      },
    });
  }

  submitStream({ requestId, prompt, mfeId, priority, onStatus, onToken, onDone }) {
    return this.scheduler.enqueue({
      requestId,
      mfeId,
      priority,
      type: 'stream',
      onStatus,
      execute: ({ signal }) => this._streamFromApi({ requestId, prompt, mfeId, signal, onToken, onDone }),
    });
  }

  cancel(requestId) {
    return this.scheduler.cancel(requestId);
  }

  getQueueStatus(requestId) {
    return this.scheduler.getQueueStatus(requestId);
  }

  getSnapshot() {
    return this.scheduler.getSnapshot();
  }

  async _streamFromApi({ requestId, prompt, mfeId, signal, onToken, onDone }) {
    const url = new URL(`${this.apiBase}/infer/stream`);
    url.searchParams.set('requestId', requestId);
    url.searchParams.set('prompt', prompt);
    url.searchParams.set('mfeId', mfeId);

    const response = await fetch(url, {
      method: 'GET',
      signal,
      headers: {
        Accept: 'text/event-stream',
      },
    });

    if (!response.ok) {
      const body = await response.text();
      const error = new Error(body || 'api_stream_failed');
      error.status = response.status;
      throw error;
    }
    if (!response.body) {
      throw new Error('api_stream_missing_body');
    }

    const reader = response.body.getReader();
    const decoder = new TextDecoder('utf-8');
    let buffer = '';
    let event = 'message';
    let dataLines = [];
    let finalDonePayload = null;

    const flushEvent = () => {
      if (dataLines.length === 0) return;
      const payloadRaw = dataLines.join('\n');
      dataLines = [];
      let payload;
      try {
        payload = JSON.parse(payloadRaw);
      } catch {
        payload = { raw: payloadRaw };
      }

      if (event === 'token') {
        onToken?.(payload);
      } else if (event === 'done') {
        finalDonePayload = payload;
        onDone?.(payload);
      } else if (event === 'error') {
        const error = new Error(payload.error || 'api_stream_error');
        error.payload = payload;
        throw error;
      }
      event = 'message';
    };

    while (true) {
      if (signal.aborted) {
        throw new Error('request_cancelled');
      }
      const { value, done } = await reader.read();
      if (done) {
        break;
      }
      buffer += decoder.decode(value, { stream: true });
      while (true) {
        const idx = buffer.indexOf('\n');
        if (idx < 0) break;
        const line = buffer.slice(0, idx).trimEnd();
        buffer = buffer.slice(idx + 1);

        if (line === '') {
          flushEvent();
          continue;
        }
        if (line.startsWith('event:')) {
          event = line.slice(6).trim();
          continue;
        }
        if (line.startsWith('data:')) {
          dataLines.push(line.slice(5).trimStart());
        }
      }
    }

    if (!finalDonePayload) {
      return { requestId, result: '', device: 'cpu', degraded: false };
    }
    return {
      requestId,
      result: String(finalDonePayload.result || ''),
      device: String(finalDonePayload.device || 'cpu'),
      degraded: Boolean(finalDonePayload.degraded),
    };
  }
}

let singleton = null;

function getEdgeAgentService() {
  if (!singleton) {
    singleton = new EdgeAgentService();
  }
  return singleton;
}

module.exports = {
  EdgeAgentService,
  getEdgeAgentService,
};
