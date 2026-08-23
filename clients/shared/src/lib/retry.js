const RETRY_DEFAULTS = { attempts: 3, baseMs: 500, maxMs: 8000 };

const RETRYABLE_ERRORS = new Set([
  'worker_crashed',
  'no_ready_workers',
  'queue_timeout',
  'exec_timeout',
  'scheduler_overloaded',
]);

export function resolveRetryPolicy(override) {
  const fromWindow = typeof window !== 'undefined' ? window.MFE_CONFIG?.retry : null;
  return { ...RETRY_DEFAULTS, ...(fromWindow || {}), ...(override || {}) };
}

export function isRetryable(payload) {
  if (!payload) return false;
  if (payload.retryable === true) return true;
  return RETRYABLE_ERRORS.has(payload.error);
}

export function backoffMs(attempt, payload, policy = resolveRetryPolicy()) {
  const hintSeconds = Number(payload?.retryAfterSeconds);
  if (Number.isFinite(hintSeconds) && hintSeconds > 0) {
    return Math.min(hintSeconds * 1000, policy.maxMs);
  }
  const exponential = policy.baseMs * Math.pow(2, attempt - 1);
  // One crash knocks back every client, so jitter keeps them from colliding on the way back.
  return Math.min(exponential + Math.random() * policy.baseMs, policy.maxMs);
}

export async function withRetry(runAttempt, hooks = {}, policy = resolveRetryPolicy()) {
  let lastError;
  for (let attempt = 1; attempt <= policy.attempts; attempt += 1) {
    try {
      return await runAttempt(attempt);
    } catch (err) {
      lastError = err;
      const payload = err?.payload;
      if (!isRetryable(payload) || attempt === policy.attempts) break;
      if (hooks.cancelled?.()) break;
      const waitMs = backoffMs(attempt, payload, policy);
      hooks.onRetry?.({ attempt, of: policy.attempts, waitMs, error: payload?.error || 'unknown' });
      await new Promise((resolve) => setTimeout(resolve, waitMs));
      if (hooks.cancelled?.()) break;
    }
  }
  throw lastError;
}
