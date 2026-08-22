// A retry reuses the same requestId on purpose. The api-server caches
// successful results against it, so a retry that races a late success replays
// the answer instead of running the inference twice.

const RETRY_DEFAULTS = { attempts: 3, baseMs: 500, maxMs: 8000 };

const RETRY = Object.assign(
    {},
    RETRY_DEFAULTS,
    (window.MFE_CONFIG && window.MFE_CONFIG.retry) || {}
);

const RETRYABLE_ERRORS = new Set([
    "worker_crashed",
    "no_ready_workers",
    "queue_timeout",
    "exec_timeout",
    "scheduler_overloaded",
]);

function isRetryable(payload) {
    if (!payload) return false;
    if (payload.retryable === true) return true;
    return RETRYABLE_ERRORS.has(payload.error);
}

function backoffMs(attempt, payload) {
    const hintSeconds = Number(payload && payload.retryAfterSeconds);
    if (Number.isFinite(hintSeconds) && hintSeconds > 0) {
        return Math.min(hintSeconds * 1000, RETRY.maxMs);
    }
    const exponential = RETRY.baseMs * Math.pow(2, attempt - 1);
    // Add jitter. One crash knocks back both MFEs, and without this they would
    // come back at the same moment and collide again.
    const jitter = Math.random() * RETRY.baseMs;
    return Math.min(exponential + jitter, RETRY.maxMs);
}

// runAttempt rejects with the shell's payload on err.payload, which is what
// decides whether to come back.
async function withRetry(runAttempt, hooks = {}) {
    let lastError;
    for (let attempt = 1; attempt <= RETRY.attempts; attempt += 1) {
        try {
            return await runAttempt(attempt);
        } catch (err) {
            lastError = err;
            const payload = err && err.payload;
            if (!isRetryable(payload) || attempt === RETRY.attempts) {
                break;
            }
            if (hooks.cancelled?.()) {
                break;
            }
            const waitMs = backoffMs(attempt, payload);
            hooks.onRetry?.({
                attempt,
                of: RETRY.attempts,
                waitMs,
                error: (payload && payload.error) || "unknown",
            });
            await new Promise((resolve) => setTimeout(resolve, waitMs));
            if (hooks.cancelled?.()) {
                break;
            }
        }
    }
    throw lastError;
}

window.MFE_RETRY = { withRetry, isRetryable, backoffMs, policy: RETRY };
