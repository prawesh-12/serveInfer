const DEFAULT_SHELL_API_BASE = 'http://127.0.0.1:3000';

const PROGRESS_EVENTS = {
  queued: 'onQueued',
  started: 'onStarted',
  token: 'onToken',
  timeout: 'onTimeout',
};

const TERMINAL_EVENTS = {
  cancelled: 'onCancelled',
  done: 'onDone',
};

export function resolveShellApiBase() {
  const configured =
    typeof window !== 'undefined' ? window.MFE_CONFIG?.shellApiBase : null;
  return configured || DEFAULT_SHELL_API_BASE;
}

export function makeRequestId(mfeId) {
  const prefix = mfeId || 'mfe';
  const stamp = Date.now().toString(36);
  const rand = Math.random().toString(36).slice(2, 8);
  return `${prefix}-${stamp}-${rand}`;
}

function parseEventData(event) {
  if (typeof event?.data !== 'string' || event.data === '') {
    return { ok: true, value: {} };
  }
  try {
    return { ok: true, value: JSON.parse(event.data) };
  } catch {
    return { ok: false, value: null };
  }
}

async function readJsonBody(response) {
  const text = await response.text();
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

export function startChatStream({
  shellApiBase,
  mfeId,
  requestId,
  prompt,
  priority,
  handlers,
}) {
  const callbacks = handlers || {};
  let source = null;
  let closed = false;

  const close = () => {
    if (closed) return;
    closed = true;
    if (source) {
      source.close();
      source = null;
    }
  };

  const emit = (name, payload) => {
    const handler = callbacks[name];
    if (typeof handler === 'function') handler(payload);
  };

  // close first, so a handler that throws still leaves the stream torn down
  const finish = (name, payload) => {
    if (closed) return;
    close();
    emit(name, payload);
  };

  const failMalformed = (eventName) => {
    finish('onError', {
      error: 'bad_event_payload',
      message: `Malformed data on the "${eventName}" event`,
      requestId,
      retryable: true,
    });
  };

  let url;
  try {
    url = new URL('/api/stream', shellApiBase || resolveShellApiBase());
    url.search = new URLSearchParams({
      prompt: prompt == null ? '' : String(prompt),
      mfeId: mfeId == null ? '' : String(mfeId),
      priority: priority || 'normal',
      requestId: requestId == null ? '' : String(requestId),
    }).toString();
    source = new EventSource(url.toString());
  } catch (err) {
    closed = true;
    emit('onError', {
      error: 'stream_open_failed',
      message: err?.message || 'Could not open the stream',
      requestId,
      retryable: false,
    });
    return { close() {} };
  }

  for (const [eventName, handlerName] of Object.entries(PROGRESS_EVENTS)) {
    source.addEventListener(eventName, (event) => {
      if (closed) return;
      const parsed = parseEventData(event);
      if (!parsed.ok) {
        failMalformed(eventName);
        return;
      }
      emit(handlerName, parsed.value);
    });
  }

  for (const [eventName, handlerName] of Object.entries(TERMINAL_EVENTS)) {
    source.addEventListener(eventName, (event) => {
      if (closed) return;
      const parsed = parseEventData(event);
      if (!parsed.ok) {
        failMalformed(eventName);
        return;
      }
      finish(handlerName, parsed.value);
    });
  }

  // EventSource reports a dropped connection on the same 'error' name the shell uses
  source.addEventListener('error', (event) => {
    if (closed) return;
    if (typeof event?.data !== 'string') {
      finish('onError', {
        error: 'connection_lost',
        message: 'The connection to the shell stream dropped',
        requestId,
        retryable: true,
      });
      return;
    }
    const parsed = parseEventData(event);
    if (!parsed.ok) {
      failMalformed('error');
      return;
    }
    finish('onError', { requestId, ...parsed.value });
  });

  return { close };
}

export async function cancelRequest({ shellApiBase, requestId }) {
  const base = shellApiBase || resolveShellApiBase();
  try {
    const response = await fetch(new URL('/api/cancel', base).toString(), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ requestId }),
    });
    const body = await readJsonBody(response);
    if (!response.ok) {
      return {
        ok: false,
        error: body?.error || `http_${response.status}`,
        status: response.status,
        body,
      };
    }
    return { ok: true, body };
  } catch (err) {
    return { ok: false, error: err?.name === 'AbortError' ? 'aborted' : 'network_error' };
  }
}

export async function fetchShellHealth({ shellApiBase, signal } = {}) {
  const base = shellApiBase || resolveShellApiBase();
  try {
    const response = await fetch(new URL('/api/health', base).toString(), { signal });
    const body = await readJsonBody(response);
    if (!response.ok) {
      return { ok: false, error: `http_${response.status}`, status: response.status, body };
    }
    return { ok: true, body };
  } catch (err) {
    return { ok: false, error: err?.name === 'AbortError' ? 'aborted' : 'network_error' };
  }
}
