import { useCallback, useEffect, useRef, useState } from 'react';
import {
  cancelRequest,
  fetchShellHealth,
  makeRequestId,
  resolveShellApiBase,
  startChatStream,
} from '../lib/shellApi.js';
import { withRetry } from '../lib/retry.js';

const MAX_EVENTS = 200;
const HEALTH_POLL_MS = 5000;

let counter = 0;
const nextId = (prefix) => `${prefix}${(counter += 1)}`;

export function useChatClient({ id, priority = 'high', retry = false }) {
  const [shellApiBase] = useState(() => resolveShellApiBase());
  const [messages, setMessages] = useState([]);
  const [events, setEvents] = useState([]);
  const [status, setStatus] = useState('idle');
  const [connection, setConnection] = useState('checking');
  const [tokenCount, setTokenCount] = useState(0);
  const [queuePosition, setQueuePosition] = useState(null);
  const [etaSeconds, setEtaSeconds] = useState(null);
  const [error, setError] = useState(null);
  const [device, setDevice] = useState(null);

  const mountedRef = useRef(true);
  const runsRef = useRef(new Map());
  const cancelledRef = useRef(new Set());
  const [runCount, setRunCount] = useState(0);

  const pushEvent = useCallback((type, detail) => {
    if (!mountedRef.current) return;
    setEvents((prev) => {
      const next = [...prev, { id: nextId('e'), at: Date.now(), type, detail }];
      return next.length > MAX_EVENTS ? next.slice(next.length - MAX_EVENTS) : next;
    });
  }, []);

  const appendToken = useCallback((answerId, token) => {
    if (!mountedRef.current || !token) return;
    setMessages((prev) =>
      prev.map((message) => (message.id === answerId ? { ...message, text: message.text + token } : message))
    );
    setTokenCount((n) => n + 1);
  }, []);

  const finishRun = useCallback((requestId) => {
    runsRef.current.delete(requestId);
    if (mountedRef.current) setRunCount(runsRef.current.size);
  }, []);

  const runOnce = useCallback(
    ({ requestId, answerId, text, runPriority }) =>
      new Promise((resolve, reject) => {
        const handlers = {
          onQueued: (payload = {}) => {
            const waitMs = Number(payload.estimatedWaitMs) || 0;
            const eta = waitMs > 0 ? Math.ceil(waitMs / 1000) : null;
            const position = typeof payload.position === 'number' ? payload.position : null;
            if (mountedRef.current) {
              setStatus('queued');
              setQueuePosition(position);
              setEtaSeconds(eta);
            }
            pushEvent('queued', { requestId, position, etaSeconds: eta });
          },
          onStarted: () => {
            if (mountedRef.current) {
              setStatus('streaming');
              setQueuePosition(null);
              setEtaSeconds(null);
            }
            pushEvent('started', { requestId });
          },
          onToken: (payload = {}) => appendToken(answerId, payload.token),
          onTimeout: (payload = {}) => pushEvent('timeout', { requestId, ...payload }),
          onCancelled: (payload = {}) => {
            const err = new Error('request_cancelled');
            err.payload = { error: 'request_cancelled', ...payload };
            reject(err);
          },
          onDone: (payload = {}) => {
            if (mountedRef.current) {
              setMessages((prev) =>
                prev.map((message) =>
                  message.id === answerId
                    ? {
                        ...message,
                        device: payload.device,
                        degraded: Boolean(payload.degraded),
                        degradedReason: payload.degradedReason ?? null,
                      }
                    : message
                )
              );
            }
            if (mountedRef.current && payload.device) setDevice(payload.device);
            pushEvent('done', { requestId, device: payload.device, degraded: Boolean(payload.degraded) });
            resolve(payload);
          },
          onError: (payload = {}) => {
            const err = new Error(payload.message || payload.error || 'stream_failed');
            err.payload = payload;
            reject(err);
          },
        };

        const handle = startChatStream({
          shellApiBase,
          mfeId: id,
          requestId,
          prompt: text,
          priority: runPriority,
          handlers,
        });
        runsRef.current.set(requestId, { handle, answerId });
        if (mountedRef.current) setRunCount(runsRef.current.size);
      }),
    [appendToken, id, pushEvent, shellApiBase]
  );

  const startRun = useCallback(
    async (text, runPriority) => {
      const requestId = makeRequestId(id);
      const answerId = nextId('m');
      cancelledRef.current.delete(requestId);

      if (mountedRef.current) {
        setError(null);
        setStatus('queued');
        setMessages((prev) => [
          ...prev,
          { id: nextId('m'), role: 'user', text, priority: runPriority, requestId },
          { id: answerId, role: 'assistant', text: '', requestId },
        ]);
      }
      pushEvent('submitted', { requestId, priority: runPriority });

      const attempt = () => runOnce({ requestId, answerId, text, runPriority });
      const hooks = {
        cancelled: () => cancelledRef.current.has(requestId),
        onRetry: (info) => pushEvent('retry', { requestId, ...info }),
      };

      try {
        await (retry ? withRetry(attempt, hooks) : attempt());
        if (mountedRef.current && runsRef.current.size <= 1) setStatus('done');
      } catch (err) {
        const payload = err?.payload ?? {};
        const code = payload.error || 'stream_failed';
        const cancelled = code === 'request_cancelled';
        if (mountedRef.current) {
          setStatus(cancelled ? 'cancelled' : 'error');
          if (!cancelled) setError({ code, message: err?.message || code });
        }
        pushEvent(cancelled ? 'cancelled' : 'error', { requestId, ...payload });
      } finally {
        finishRun(requestId);
      }
    },
    [finishRun, id, pushEvent, retry, runOnce]
  );

  const send = useCallback(
    (prompt, options = {}) => {
      const text = typeof prompt === 'string' ? prompt.trim() : '';
      if (!text || runsRef.current.size > 0) return;
      setTokenCount(0);
      startRun(text, options.priority || priority);
    },
    [priority, startRun]
  );

  const burst = useCallback(
    (prompt, count = 5, burstPriority = 'low') => {
      const text = typeof prompt === 'string' ? prompt.trim() : '';
      if (!text || runsRef.current.size > 0) return;
      setTokenCount(0);
      pushEvent('burst', { count, priority: burstPriority });
      for (let i = 0; i < count; i += 1) startRun(text, burstPriority);
    },
    [pushEvent, startRun]
  );

  const cancel = useCallback(() => {
    for (const [requestId, run] of runsRef.current) {
      cancelledRef.current.add(requestId);
      run.handle?.close();
      cancelRequest({ shellApiBase, requestId }).then((result) =>
        pushEvent('cancel-ack', { requestId, ok: result?.ok !== false })
      );
    }
    runsRef.current.clear();
    if (mountedRef.current) {
      setRunCount(0);
      setStatus('cancelled');
      setQueuePosition(null);
      setEtaSeconds(null);
    }
    pushEvent('cancelled', { reason: 'user_cancelled' });
  }, [pushEvent, shellApiBase]);

  const clear = useCallback(() => {
    setMessages([]);
    setEvents([]);
    setError(null);
    if (runsRef.current.size === 0) {
      setStatus('idle');
      setTokenCount(0);
    }
  }, []);

  useEffect(() => {
    mountedRef.current = true;
    const controller = new AbortController();

    const poll = async () => {
      const result = await fetchShellHealth({ shellApiBase, signal: controller.signal });
      if (!mountedRef.current) return;
      setConnection(result?.ok ? 'online' : 'offline');
    };

    poll();
    const timer = setInterval(poll, HEALTH_POLL_MS);

    return () => {
      mountedRef.current = false;
      clearInterval(timer);
      controller.abort();
      for (const run of runsRef.current.values()) run.handle?.close();
      runsRef.current.clear();
    };
  }, [shellApiBase]);

  return {
    messages,
    events,
    status,
    connection,
    isBusy: runCount > 0,
    inFlight: runCount,
    device,
    tokenCount,
    queuePosition,
    etaSeconds,
    error,
    shellApiBase,
    send,
    burst,
    cancel,
    clear,
  };
}
