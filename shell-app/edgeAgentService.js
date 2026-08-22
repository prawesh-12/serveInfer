'use strict';

const { numberEnv, requiredEnv } = require('../config/env');
const { Scheduler } = require('./scheduler');

class EdgeAgentService {
  constructor() {
    this.apiBase = requiredEnv('EDGE_API_BASE');
    const maxSlots = numberEnv('EDGE_MAX_SLOTS');
    this.scheduler = new Scheduler({
      maxSlots,
      maxPerMfe: numberEnv('EDGE_MAX_PER_MFE'),
      maxQueue: numberEnv('EDGE_MAX_QUEUE'),
      agingMs: numberEnv('EDGE_AGING_MS'),
      queueTimeoutMs: numberEnv('EDGE_QUEUE_TIMEOUT_MS'),
      defaultDurationMs: numberEnv('EDGE_DEFAULT_JOB_MS'),
      execTimeoutMs: numberEnv('EDGE_EXEC_TIMEOUT_MS'),
      doneTtlMs: numberEnv('EDGE_DONE_TTL_MS'),
      doneMaxEntries: numberEnv('EDGE_DONE_MAX_ENTRIES'),
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

  async getAgentHealth() {
    const response = await fetch(`${this.apiBase}/health`);
    const body = await response.json().catch(() => ({}));
    if (!response.ok) {
      const error = new Error(body.error || 'agent_health_failed');
      error.status = response.status;
      error.payload = body;
      throw error;
    }
    return body;
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
      degradedReason: finalDonePayload.degradedReason || null,
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
