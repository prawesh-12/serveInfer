'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { RequestRegistry } = require('../backend/api-server/requestRegistry');
const { deferred, sleep, waitFor } = require('./support');

const tempDirs = [];

function newInflightPath() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'edge-registry-'));
  tempDirs.push(dir);
  return path.join(dir, 'inflight.json');
}

// The registry writes to a temp file and then renames it. Both steps are async.
// So a reader has to wait for the rename, not read the moment a call returns.
async function readInflight(inflightPath) {
  return waitFor(() => {
    try {
      return JSON.parse(fs.readFileSync(inflightPath, 'utf8'));
    } catch {
      return null;
    }
  });
}

test.after(() => {
  for (const dir of tempDirs) {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('two concurrent submissions of one requestId run the work once and both get the same result', async () => {
  const registry = new RequestRegistry({
    inflightPath: newInflightPath(),
    idempotencyTtlMs: 60_000,
  });
  let calls = 0;
  const gate = deferred();
  const execute = () => {
    calls += 1;
    return gate.promise;
  };

  const first = registry.run({ requestId: 'same-id', mfeId: 'doc-qa', execute });
  const second = registry.run({ requestId: 'same-id', mfeId: 'doc-qa', execute });

  assert.equal(first.replay, false);
  assert.equal(second.replay, true, 'the second caller is told it joined an existing run');
  assert.equal(second.state, 'inflight');

  gate.resolve({ text: 'one answer' });
  const [a, b] = await Promise.all([first.promise, second.promise]);

  assert.equal(calls, 1, 'only one inference ran');
  assert.deepEqual(a, b);
  assert.deepEqual(a, { text: 'one answer' });
});

test('a replay after success returns the cached result and does not run the work again', async () => {
  const registry = new RequestRegistry({
    inflightPath: newInflightPath(),
    idempotencyTtlMs: 60_000,
  });
  let calls = 0;

  const first = registry.run({
    requestId: 'cached',
    mfeId: 'doc-qa',
    execute: () => {
      calls += 1;
      return Promise.resolve({ text: 'first answer' });
    },
  });
  await first.promise;

  const replay = registry.run({
    requestId: 'cached',
    mfeId: 'doc-qa',
    execute: () => {
      calls += 1;
      return Promise.resolve({ text: 'second answer' });
    },
  });

  assert.equal(replay.replay, true);
  assert.equal(replay.state, 'completed');
  assert.deepEqual(await replay.promise, { text: 'first answer' });
  assert.equal(calls, 1, 'the replayed prompt never reached a worker');
});

test('a failed run is deliberately not cached, so the same id can be retried', async () => {
  const registry = new RequestRegistry({
    inflightPath: newInflightPath(),
    idempotencyTtlMs: 60_000,
  });
  let calls = 0;

  const failing = registry.run({
    requestId: 'retry-me',
    mfeId: 'doc-qa',
    execute: () => {
      calls += 1;
      return Promise.reject(new Error('worker_crashed'));
    },
  });
  await assert.rejects(failing.promise, /worker_crashed/);

  assert.equal(registry.lookup('retry-me').state, 'new', 'the failure left no trace');

  const retry = registry.run({
    requestId: 'retry-me',
    mfeId: 'doc-qa',
    execute: () => {
      calls += 1;
      return Promise.resolve({ text: 'worked this time' });
    },
  });

  assert.equal(retry.replay, false, 'a retry after a failure is a fresh run, not a replay');
  assert.deepEqual(await retry.promise, { text: 'worked this time' });
  assert.equal(calls, 2);
});

test('a cached result stops being replayed once the idempotency TTL passes', async () => {
  const registry = new RequestRegistry({
    inflightPath: newInflightPath(),
    idempotencyTtlMs: 20,
  });
  let calls = 0;
  const execute = () => {
    calls += 1;
    return Promise.resolve({ text: `answer ${calls}` });
  };

  await registry.run({ requestId: 'expiring', mfeId: 'doc-qa', execute }).promise;
  await sleep(35);

  const afterTtl = registry.run({ requestId: 'expiring', mfeId: 'doc-qa', execute });
  assert.equal(afterTtl.replay, false, 'the id runs fresh once the window closes');
  assert.deepEqual(await afterTtl.promise, { text: 'answer 2' });
  assert.equal(calls, 2);
});

test('the in-flight file names the open requests and is empty again once they settle', async () => {
  const inflightPath = newInflightPath();
  const registry = new RequestRegistry({ inflightPath, idempotencyTtlMs: 60_000 });
  const gate = deferred();

  const open = registry.run({
    requestId: 'open-one',
    mfeId: 'meeting-summary',
    stream: true,
    execute: () => gate.promise,
  });

  const whileOpen = await waitFor(async () => {
    const parsed = await readInflight(inflightPath);
    return parsed.inflight.length === 1 ? parsed : null;
  });
  assert.equal(whileOpen.inflight[0].requestId, 'open-one');
  assert.equal(whileOpen.inflight[0].mfeId, 'meeting-summary');
  assert.equal(whileOpen.inflight[0].stream, true);
  assert.equal(whileOpen.pid, process.pid);

  gate.resolve({ text: 'done' });
  await open.promise;

  const afterSettle = await waitFor(async () => {
    const parsed = await readInflight(inflightPath);
    return parsed.inflight.length === 0 ? parsed : null;
  });
  assert.deepEqual(afterSettle.inflight, []);
  assert.deepEqual(registry.snapshot().inflight, []);
});

test('a fresh registry reports what the previous process left in flight as orphans', async () => {
  const inflightPath = newInflightPath();
  const first = new RequestRegistry({ inflightPath, idempotencyTtlMs: 60_000 });
  const gate = deferred();

  first.run({ requestId: 'never-finished', mfeId: 'doc-qa', execute: () => gate.promise });
  await waitFor(async () => {
    const parsed = await readInflight(inflightPath);
    return parsed.inflight.length === 1;
  });

  // No settle and no clean shutdown: exactly what a crash leaves behind.
  const second = new RequestRegistry({ inflightPath, idempotencyTtlMs: 60_000 });
  assert.equal(second.orphans.length, 1);
  assert.equal(second.orphans[0].requestId, 'never-finished');
  assert.deepEqual(second.snapshot().orphanedFromPreviousRun, second.orphans);

  gate.resolve({ text: 'late' });
});

test('a registry with no previous file reports no orphans', () => {
  const registry = new RequestRegistry({
    inflightPath: newInflightPath(),
    idempotencyTtlMs: 60_000,
  });
  assert.deepEqual(registry.orphans, []);
});

test('lookup reports new, then inflight, then completed with the result attached', async () => {
  const registry = new RequestRegistry({
    inflightPath: newInflightPath(),
    idempotencyTtlMs: 60_000,
  });
  const gate = deferred();

  assert.deepEqual(registry.lookup('tracked'), { state: 'new' });

  const handle = registry.run({
    requestId: 'tracked',
    mfeId: 'doc-qa',
    execute: () => gate.promise,
  });
  assert.deepEqual(registry.lookup('tracked'), { state: 'inflight' });

  gate.resolve({ text: 'finished' });
  await handle.promise;

  assert.deepEqual(registry.lookup('tracked'), {
    state: 'completed',
    result: { text: 'finished' },
  });
});

test('overlapping writes still publish valid JSON, not a mix of two payloads', async () => {
  // Every write used to go to the same .tmp path. Two writes at once meant two
  // renames, and the published file could end up a mix of both. readInflightFile
  // then fails to parse it and reports no orphans, without saying anything.
  const inflightPath = newInflightPath();
  const registry = new RequestRegistry({ inflightPath, idempotencyTtlMs: 60_000 });
  const jobs = [];
  for (let i = 0; i < 40; i += 1) {
    jobs.push(
      registry.run({
        requestId: `race-${i}`,
        mfeId: 'doc-qa',
        execute: () => sleep(i % 5),
      }).promise
    );
  }
  await Promise.all(jobs);
  // Writes are async and serialised, so the file may not exist yet on the first
  // look. Tolerate that rather than racing it.
  await waitFor(() => {
    try {
      return JSON.parse(fs.readFileSync(inflightPath, 'utf8')).inflight.length === 0;
    } catch {
      return false;
    }
  });

  const parsed = JSON.parse(fs.readFileSync(inflightPath, 'utf8'));
  assert.deepEqual(parsed.inflight, []);

  // Writes are serialised, so the last one can still be renaming when the
  // published file already reads as empty. Wait for the steady state rather than
  // asserting on a moment mid-write.
  const dir = path.dirname(inflightPath);
  const base = path.basename(inflightPath);
  const leftovers = () =>
    fs.readdirSync(dir).filter((name) => name.startsWith(`${base}.`) && name.endsWith('.tmp'));
  await waitFor(() => leftovers().length === 0);
  assert.deepEqual(leftovers(), [], 'no temp files left behind');
});
