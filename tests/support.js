'use strict';

// Shared helpers for the test suite. This file is not itself a test.

// A promise plus its settle functions, so a test can hold a job open and
// release it exactly when it wants to.
function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((res, rej) => {
    resolve = res;
    reject = rej;
  });
  promise.catch(() => {}); // a test may leave one unsettled on purpose
  return { promise, resolve, reject };
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Polls until check() returns something truthy. Better than a fixed sleep,
// because the test finishes the moment the condition holds.
async function waitFor(check, { timeoutMs = 2000, stepMs = 5 } = {}) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const value = await check();
    if (value) {
      return value;
    }
    if (Date.now() > deadline) {
      throw new Error('waitFor timed out');
    }
    await sleep(stepMs);
  }
}

// Rejects when the abort signal fires. That is what a well behaved job does.
// A job that ignores abort is tested separately.
function abortableJob(signal, { onStart } = {}) {
  onStart?.();
  return new Promise((_resolve, reject) => {
    signal.addEventListener('abort', () => {
      const err = new Error('aborted');
      err.name = 'AbortError';
      reject(err);
    });
  });
}

module.exports = { deferred, sleep, waitFor, abortableJob };
