'use strict';

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
