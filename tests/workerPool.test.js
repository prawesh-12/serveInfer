'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const { spawn } = require('node:child_process');

const { WorkerPool } = require('../api-server/ipc');
const { sleep, waitFor } = require('./support');

// An AF_UNIX path has to fit in 108 bytes, so these stay in /tmp with short
// names rather than under a long temp directory.
let counter = 0;
const socketPaths = [];
const pools = [];
const servers = [];

function newPrefix() {
  counter += 1;
  const prefix = `/tmp/edge-t${process.pid}-${counter}-`;
  socketPaths.push(`${prefix}0.sock`);
  return prefix;
}

function makePool(overrides = {}) {
  const pool = new WorkerPool({
    workerCount: 1,
    workerSocketPrefix: newPrefix(),
    connectTimeoutMs: 50,
    recoveryMs: 10,
    recoveryAttempts: 3,
    ...overrides,
  });
  pools.push(pool);
  return pool;
}

function listenOn(socketPath) {
  return new Promise((resolve) => {
    const server = net.createServer();
    servers.push(server);
    server.listen(socketPath, () => resolve(server));
  });
}

test.after(async () => {
  for (const pool of pools) {
    for (const worker of pool.workers.values()) {
      clearTimeout(worker.recoveryTimer);
    }
  }
  for (const server of servers) {
    await new Promise((resolve) => server.close(resolve));
  }
  for (const socketPath of socketPaths) {
    fs.rmSync(socketPath, { force: true });
  }
});

test('probing a worker fails when its socket file is not there yet', async () => {
  const pool = makePool();
  assert.equal(await pool._probeWorker(0), false);
});

test('probing a worker fails for a stale socket file that nothing is listening on', async () => {
  const pool = makePool();
  const socketPath = pool.workers.get(0).socketPath;

  // A killed process leaves its socket inode behind. That leftover file is the
  // exact thing that used to fool the old bare-timer recovery.
  const child = spawn(
    process.execPath,
    [
      '-e',
      `require('node:net').createServer().listen(${JSON.stringify(socketPath)}, () => process.stdout.write('up'))`,
    ],
    { stdio: ['ignore', 'pipe', 'ignore'] }
  );
  await new Promise((resolve) => child.stdout.once('data', resolve));
  child.kill('SIGKILL');
  await new Promise((resolve) => child.once('exit', resolve));

  assert.ok(fs.existsSync(socketPath), 'the dead process left its socket file behind');
  assert.equal(await pool._probeWorker(0), false);
});

test('probing a worker succeeds when something is listening on its socket', async () => {
  const pool = makePool();
  await listenOn(pool.workers.get(0).socketPath);
  assert.equal(await pool._probeWorker(0), true);
});

test('a crashed worker stays out of the pool while its socket is dead, and acquiring throws no_ready_workers', async () => {
  const pool = makePool({ recoveryMs: 5 });
  pool._markWorkerCrashed(0, null);

  assert.equal(pool.workers.get(0).status, 'crashed');
  await sleep(40); // several recovery probes have run and failed by now
  assert.equal(pool.workers.get(0).status, 'crashed');

  assert.throws(
    () => pool._acquireWorker(),
    (err) => {
      assert.equal(err.code, 'no_ready_workers');
      assert.equal(err.statusCode, 503);
      assert.equal(err.details.retryAfterSeconds, 1);
      return true;
    }
  );
});

test('a crashed worker returns to ready once its socket accepts a connection again', async () => {
  const pool = makePool({ recoveryMs: 5 });
  await listenOn(pool.workers.get(0).socketPath);

  pool._markWorkerCrashed(0, null);
  await waitFor(() => pool.workers.get(0).status === 'ready');

  const worker = pool._acquireWorker();
  assert.equal(worker.workerId, 0);
  assert.equal(worker.recoveryAttempt, 0, 'the attempt counter was reset');
});

test('recovery probing gives up after recoveryAttempts and leaves the worker crashed', async () => {
  const pool = makePool({ recoveryMs: 5, recoveryAttempts: 2 });
  pool._markWorkerCrashed(0, null);

  await waitFor(() => pool.workers.get(0).recoveryAttempt === 2);
  await sleep(40); // long enough for a third probe, if one were ever scheduled

  assert.equal(pool.workers.get(0).recoveryAttempt, 2, 'no attempt past the cap');
  assert.equal(pool.workers.get(0).status, 'crashed');
});

test('a supervisor worker_ready message brings a crashed worker back and clears its recovery timer', async () => {
  const pool = makePool({ recoveryMs: 1000 });
  pool._markWorkerCrashed(0, null);
  assert.ok(pool.workers.get(0).recoveryTimer, 'a recovery probe was pending');

  pool.handleSupervisorMessage({ type: 'worker_ready', workerId: 0 });

  const worker = pool.workers.get(0);
  assert.equal(worker.status, 'ready');
  assert.equal(worker.recoveryTimer, null);
  assert.equal(worker.recoveryAttempt, 0);
});

test('a worker crash fails its in-flight requests with worker_crashed, status 503 and a retry hint', () => {
  const pool = makePool();
  const failures = [];
  pool.inFlight.set('req-1', { workerId: 0, fail: (err) => failures.push(err) });
  pool.inFlight.set('req-2', { workerId: 0, fail: (err) => failures.push(err) });

  pool._markWorkerCrashed(0, null);

  assert.equal(failures.length, 2, 'every request on that worker was failed');
  for (const err of failures) {
    assert.equal(err.code, 'worker_crashed');
    assert.equal(err.statusCode, 503);
    assert.equal(err.details.retryAfterSeconds, 2);
    assert.equal(err.details.workerId, 0);
  }
});

test('a healthy worker whose socket exists is handed out by _acquireWorker', async () => {
  const pool = makePool();
  assert.equal(pool.workers.get(0).status, 'starting');
  assert.throws(() => pool._acquireWorker(), (err) => err.code === 'no_ready_workers');

  await listenOn(pool.workers.get(0).socketPath);
  const worker = pool._acquireWorker();
  assert.equal(worker.status, 'busy', 'one request per worker, so it is busy while held');
});

test('a supervisor worker_restarted message re-arms the probe instead of trusting the claim', async () => {
  // The supervisor has started a replacement, but its socket is not there yet.
  // Trusting the message alone would hand out a worker that cannot take a
  // connection.
  const pool = makePool({ recoveryMs: 10, recoveryAttempts: 2 });
  pool._markWorkerCrashed(0, null);
  await waitFor(() => pool.workers.get(0).recoveryAttempt >= 2);

  pool.handleSupervisorMessage({ type: 'worker_restarted', workerId: 0 });
  assert.equal(pool.workers.get(0).status, 'crashed', 'still crashed until a probe says otherwise');
  assert.equal(pool.workers.get(0).recoveryAttempt, 1, 'the attempt budget is reset');

  // once something is actually listening, the probe lets it back in
  await listenOn(pool.workers.get(0).socketPath);
  pool.handleSupervisorMessage({ type: 'worker_restarted', workerId: 0 });
  await waitFor(() => pool.workers.get(0).status === 'ready');
});

test('a worker whose socket file is stale is quarantined after its first failed request', async () => {
  // _refreshWorkerReadiness marks a worker ready if its socket file exists, and
  // a leftover file passes that check. The first failed request proves the file
  // was lying. The worker goes out then, instead of being handed out again.
  const pool = makePool({ recoveryMs: 10, recoveryAttempts: 2 });
  const socketPath = pool.workers.get(0).socketPath;
  fs.writeFileSync(socketPath, '');

  const worker = pool._acquireWorker();
  assert.equal(worker.status, 'busy', 'existsSync alone let it through');

  const error = await pool
    ._runRequest({ worker, requestId: 'stale-1', prompt: 'x', mfeId: 'doc-qa', stream: false })
    .catch((err) => err);
  assert.ok(WorkerPool.isTransportFailure(error.code), `expected a transport failure, got ${error.code}`);
  assert.equal(pool.workers.get(0).status, 'crashed');
  assert.throws(() => pool._acquireWorker(), (err) => err.code === 'no_ready_workers');
});

test('isTransportFailure separates a dead socket from a worker that answered with an error', () => {
  assert.ok(WorkerPool.isTransportFailure('worker_connect_timeout'));
  assert.ok(WorkerPool.isTransportFailure('worker_socket_error'));
  assert.ok(WorkerPool.isTransportFailure('worker_closed'));
  // these came back over a working socket, so the worker is not the problem
  assert.ok(!WorkerPool.isTransportFailure('worker_error'));
  assert.ok(!WorkerPool.isTransportFailure('worker_bad_json'));
  assert.ok(!WorkerPool.isTransportFailure('no_ready_workers'));
  assert.ok(!WorkerPool.isTransportFailure(undefined));
});
