'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const retryPath = path.join(__dirname, '..', 'clients', 'document-qa', 'public', 'retry.js');

// A browser script that reads window.MFE_CONFIG once at load, so a new policy needs a fresh load.
function loadRetry(policy) {
  global.window = { MFE_CONFIG: policy ? { retry: policy } : undefined };
  delete require.cache[require.resolve(retryPath)];
  require(retryPath);
  return global.window.MFE_RETRY;
}

function rejectWith(error, extra = {}) {
  const err = new Error(error);
  err.payload = { error, ...extra };
  return err;
}

test.after(() => {
  delete global.window;
  delete require.cache[require.resolve(retryPath)];
});

test('the two MFEs ship the same retry helper', () => {
  const other = path.join(__dirname, '..', 'clients', 'meeting-summary', 'public', 'retry.js');
  assert.equal(fs.readFileSync(retryPath, 'utf8'), fs.readFileSync(other, 'utf8'));
});

test('the transient backend failures are all treated as retryable', () => {
  const { isRetryable } = loadRetry();
  for (const error of [
    'worker_crashed',
    'no_ready_workers',
    'queue_timeout',
    'exec_timeout',
    'scheduler_overloaded',
  ]) {
    assert.equal(isRetryable({ error }), true, `${error} should be retryable`);
  }
});

test('a payload the shell marked retryable is retried whatever the error name says', () => {
  const { isRetryable } = loadRetry();
  assert.equal(isRetryable({ error: 'something_new', retryable: true }), true);
});

test('client errors and deliberate stops are never retried', () => {
  const { isRetryable } = loadRetry();
  for (const error of ['prompt_required', 'request_cancelled', 'connection_closed']) {
    assert.equal(isRetryable({ error }), false, `${error} should not be retried`);
  }
  assert.equal(isRetryable(null), false);
  assert.equal(isRetryable(undefined), false);
  assert.equal(isRetryable({}), false);
});

test('the server retryAfterSeconds hint wins over the local backoff', () => {
  const { backoffMs } = loadRetry({ attempts: 3, baseMs: 500, maxMs: 8000 });
  assert.equal(backoffMs(1, { error: 'worker_crashed', retryAfterSeconds: 2 }), 2000);
  assert.equal(backoffMs(3, { error: 'worker_crashed', retryAfterSeconds: 2 }), 2000);
});

test('a server hint longer than maxMs is capped at maxMs', () => {
  const { backoffMs } = loadRetry({ attempts: 3, baseMs: 500, maxMs: 4000 });
  assert.equal(backoffMs(1, { retryAfterSeconds: 600 }), 4000);
});

test('a hint of zero or a non-number falls through to the local backoff', () => {
  const { backoffMs } = loadRetry({ attempts: 3, baseMs: 100, maxMs: 8000 });
  for (const payload of [{ retryAfterSeconds: 0 }, { retryAfterSeconds: 'soon' }, {}]) {
    const wait = backoffMs(1, payload);
    assert.ok(wait >= 100 && wait < 200, `expected the first local step, got ${wait}`);
  }
});

test('without a hint the wait grows exponentially and never passes maxMs', () => {
  const { backoffMs } = loadRetry({ attempts: 6, baseMs: 100, maxMs: 1000 });
  // base * 2^(attempt-1), plus up to one base of jitter.
  const bounds = [
    [1, 100, 200],
    [2, 200, 300],
    [3, 400, 500],
  ];
  for (const [attempt, low, high] of bounds) {
    const wait = backoffMs(attempt, {});
    assert.ok(wait >= low && wait < high, `attempt ${attempt} gave ${wait}`);
  }
  for (let attempt = 1; attempt <= 6; attempt += 1) {
    assert.ok(backoffMs(attempt, {}) <= 1000, 'the cap holds at every attempt');
  }
});

test('withRetry gives up after the configured number of attempts and rethrows the last error', async () => {
  const { withRetry } = loadRetry({ attempts: 3, baseMs: 1, maxMs: 2 });
  const seen = [];
  const notified = [];

  await assert.rejects(
    withRetry(
      (attempt) => {
        seen.push(attempt);
        return Promise.reject(rejectWith('worker_crashed'));
      },
      { onRetry: (info) => notified.push(info) }
    ),
    /worker_crashed/
  );

  assert.deepEqual(seen, [1, 2, 3]);
  assert.equal(notified.length, 2, 'the user is told about each wait, not about the final failure');
  assert.equal(notified[0].of, 3);
  assert.equal(notified[0].error, 'worker_crashed');
  assert.ok(notified[0].waitMs > 0);
});

test('withRetry stops at the first non-retryable error', async () => {
  const { withRetry } = loadRetry({ attempts: 5, baseMs: 1, maxMs: 2 });
  let calls = 0;

  await assert.rejects(
    withRetry(() => {
      calls += 1;
      return Promise.reject(rejectWith('prompt_required'));
    }),
    /prompt_required/
  );

  assert.equal(calls, 1, 'a bad prompt is not going to fix itself');
});

test('withRetry stops as soon as the cancelled hook says the user gave up', async () => {
  const { withRetry } = loadRetry({ attempts: 5, baseMs: 1, maxMs: 2 });
  let calls = 0;

  await assert.rejects(
    withRetry(
      () => {
        calls += 1;
        return Promise.reject(rejectWith('worker_crashed'));
      },
      { cancelled: () => true }
    ),
    /worker_crashed/
  );

  assert.equal(calls, 1, 'no backoff wait once the request is cancelled');
});

test('withRetry returns the first success without retrying', async () => {
  const { withRetry } = loadRetry({ attempts: 3, baseMs: 1, maxMs: 2 });
  let calls = 0;

  const result = await withRetry(() => {
    calls += 1;
    return calls === 1 ? Promise.reject(rejectWith('no_ready_workers')) : Promise.resolve('answer');
  });

  assert.equal(result, 'answer');
  assert.equal(calls, 2);
});

test('the defaults apply when the page ships no retry config', () => {
  const { policy } = loadRetry();
  assert.deepEqual(policy, { attempts: 3, baseMs: 500, maxMs: 8000 });
});
