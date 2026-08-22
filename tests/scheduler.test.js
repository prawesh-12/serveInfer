'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const { Scheduler } = require('../backend/shell-app/scheduler');
const { deferred, sleep, waitFor, abortableJob } = require('./support');

// Fast timers everywhere. The defaults are measured in seconds and minutes,
// which no test should ever wait out.
function makeScheduler(overrides = {}) {
  return new Scheduler({
    maxSlots: 1,
    maxPerMfe: 1,
    maxQueue: 20,
    agingMs: 1000,
    defaultDurationMs: 10,
    queueTimeoutMs: 5000,
    execTimeoutMs: 5000,
    doneTtlMs: 60_000,
    doneMaxEntries: 500,
    ...overrides,
  });
}

function swallow(handle) {
  handle.promise.catch(() => {});
  return handle;
}

test('a high priority job runs before normal, and normal before low', async () => {
  const scheduler = makeScheduler();
  const started = [];
  const blocker = deferred();

  const held = scheduler.enqueue({
    requestId: 'blocker',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => blocker.promise,
  });

  const queued = ['low', 'normal', 'high'].map((priority) =>
    scheduler.enqueue({
      requestId: priority,
      mfeId: 'doc-qa',
      priority,
      execute: () => {
        started.push(priority);
        return Promise.resolve(priority);
      },
    })
  );

  blocker.resolve('ok');
  await held.promise;
  await Promise.all(queued.map((job) => job.promise));

  assert.deepEqual(started, ['high', 'normal', 'low']);
});

test('effective priority is the base score plus one point per aging interval waited', () => {
  const scheduler = makeScheduler({ agingMs: 1000 });
  const now = 100_000;

  assert.equal(scheduler.effectivePriority({ priority: 'high', createdAt: now }, now), 300);
  assert.equal(scheduler.effectivePriority({ priority: 'normal', createdAt: now }, now), 200);
  assert.equal(scheduler.effectivePriority({ priority: 'low', createdAt: now }, now), 100);

  // 3500 ms of waiting is three whole intervals. The remainder does not count.
  assert.equal(scheduler.effectivePriority({ priority: 'low', createdAt: now - 3500 }, now), 103);
});

test('a low priority job that has aged long enough overtakes a newer normal job', async () => {
  const scheduler = makeScheduler({ agingMs: 10 });
  const started = [];
  const blocker = deferred();

  const held = scheduler.enqueue({
    requestId: 'blocker',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => blocker.promise,
  });

  const low = scheduler.enqueue({
    requestId: 'low-waiter',
    mfeId: 'doc-qa',
    priority: 'low',
    execute: () => {
      started.push('low-waiter');
      return Promise.resolve();
    },
  });

  // Backdating beats sleeping for a second. Low needs 100 aging intervals more
  // than the normal job has, so with agingMs=10 that is just over a second.
  const lowJob = scheduler.queue.find((job) => job.requestId === 'low-waiter');
  lowJob.createdAt -= 1100;

  const normal = scheduler.enqueue({
    requestId: 'fresh-normal',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => {
      started.push('fresh-normal');
      return Promise.resolve();
    },
  });

  blocker.resolve('ok');
  await held.promise;
  await Promise.all([low.promise, normal.promise]);

  assert.deepEqual(started, ['low-waiter', 'fresh-normal']);
});

test('one MFE flooding the queue never holds more than maxPerMfe slots, and the other MFE still gets in', async () => {
  const scheduler = makeScheduler({ maxSlots: 4, maxPerMfe: 2 });
  const gates = [];
  let peakFlood = 0;

  const submit = (requestId, mfeId) => {
    const gate = deferred();
    gates.push(gate);
    return scheduler.enqueue({
      requestId,
      mfeId,
      priority: 'normal',
      execute: () => {
        peakFlood = Math.max(peakFlood, scheduler.perMfeActive.get('flood') || 0);
        return gate.promise;
      },
    });
  };

  const jobs = [];
  for (let i = 0; i < 6; i += 1) {
    jobs.push(submit(`flood-${i}`, 'flood'));
  }
  for (let i = 0; i < 2; i += 1) {
    jobs.push(submit(`other-${i}`, 'other'));
  }

  assert.equal(scheduler.perMfeActive.get('flood'), 2, 'the flooding MFE is capped at 2');
  assert.equal(scheduler.perMfeActive.get('other'), 2, 'the quiet MFE still gets its slots');
  assert.equal(scheduler.active.size, 4);
  assert.equal(scheduler.queue.length, 4);

  for (const gate of gates) {
    gate.resolve('ok');
  }
  await Promise.all(jobs.map((job) => job.promise));

  assert.equal(peakFlood, 2, 'the flooding MFE never exceeded its cap while draining');
  // The slot is released in a .finally(), one microtask after the job resolves.
  await waitFor(() => scheduler.active.size === 0);
});

test('enqueueing past maxQueue throws scheduler_overloaded with status 429', async () => {
  const scheduler = makeScheduler({ maxQueue: 2 });
  const blocker = deferred();

  const held = scheduler.enqueue({
    requestId: 'blocker',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => blocker.promise,
  });
  const waiting = ['q1', 'q2'].map((requestId) =>
    swallow(
      scheduler.enqueue({
        requestId,
        mfeId: 'doc-qa',
        priority: 'normal',
        execute: () => Promise.resolve(),
      })
    )
  );

  assert.throws(
    () =>
      scheduler.enqueue({
        requestId: 'q3',
        mfeId: 'doc-qa',
        priority: 'normal',
        execute: () => Promise.resolve(),
      }),
    (err) => {
      assert.equal(err.code, 'scheduler_overloaded');
      assert.equal(err.status, 429);
      assert.equal(err.details.maxQueue, 2);
      return true;
    }
  );

  blocker.resolve('ok');
  await held.promise;
  await Promise.all(waiting.map((job) => job.promise));
});

test('a job that waits longer than the queue timeout is rejected with queue_timeout and status 408', async () => {
  const scheduler = makeScheduler({ queueTimeoutMs: 25 });
  const events = [];
  const blocker = deferred();

  const held = scheduler.enqueue({
    requestId: 'blocker',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => blocker.promise,
  });
  const victim = scheduler.enqueue({
    requestId: 'victim',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => Promise.resolve('should never run'),
    onStatus: (event) => events.push(event),
  });

  await assert.rejects(victim.promise, (err) => {
    assert.equal(err.code, 'queue_timeout');
    assert.equal(err.status, 408);
    assert.equal(err.details.retryable, true);
    return true;
  });

  const timeoutEvent = events.find((event) => event.state === 'timeout');
  assert.ok(timeoutEvent, 'the client is told the wait timed out');
  assert.ok(timeoutEvent.waitedMs >= 0);
  assert.equal(scheduler.getQueueStatus('victim').state, 'timeout');

  blocker.resolve('ok');
  await held.promise;
});

test('a running job that honours its abort signal is killed by the execution timeout and frees its slot', async () => {
  const scheduler = makeScheduler({ execTimeoutMs: 25 });
  const events = [];

  const wedged = scheduler.enqueue({
    requestId: 'wedged',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: ({ signal }) => abortableJob(signal),
    onStatus: (event) => events.push(event),
  });

  await assert.rejects(wedged.promise, (err) => {
    assert.equal(err.code, 'exec_timeout');
    assert.equal(err.status, 504);
    assert.equal(err.details.retryable, true);
    return true;
  });

  const timeoutEvent = events.find((event) => event.state === 'timeout');
  assert.equal(timeoutEvent.phase, 'execution', 'phase separates this from a queue timeout');
  assert.equal(scheduler.active.size, 0, 'the slot went back');
  assert.equal(scheduler.perMfeActive.size, 0);
  assert.equal(scheduler.getQueueStatus('wedged').state, 'timeout');
  assert.equal(scheduler.getQueueStatus('wedged').phase, 'execution');
});

test('the execution timeout reclaims the slot even when the job ignores its abort signal', async () => {
  // The timeout releases the slot itself rather than waiting on the job's own
  // promise chain. A job that never settles is exactly what this timeout is for,
  // so waiting for it to settle would defeat the purpose.
  const scheduler = makeScheduler({ execTimeoutMs: 20 });
  const events = [];

  const deaf = scheduler.enqueue({
    requestId: 'deaf',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => new Promise(() => {}),
    onStatus: (event) => events.push(event),
  });

  const error = await deaf.promise.catch((err) => err);
  assert.equal(error.code, 'exec_timeout');
  assert.equal(error.status, 504);
  assert.ok(events.some((event) => event.state === 'timeout' && event.phase === 'execution'));
  assert.equal(scheduler.active.size, 0, 'the slot is released');
  assert.equal(scheduler.getQueueStatus('deaf').state, 'timeout');

  // and the reclaimed slot actually takes new work
  const next = await scheduler.enqueue({
    requestId: 'next',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: async () => 'ran',
  }).promise;
  assert.equal(next, 'ran');
});

test('cancelling a queued job rejects it, reports state queued, and stops its queue timer', async () => {
  const scheduler = makeScheduler({ queueTimeoutMs: 20 });
  const events = [];
  const blocker = deferred();

  const held = scheduler.enqueue({
    requestId: 'blocker',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => blocker.promise,
  });
  const waiting = scheduler.enqueue({
    requestId: 'waiting',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => Promise.resolve('should never run'),
    onStatus: (event) => events.push(event),
  });

  assert.deepEqual(scheduler.cancel('waiting'), { cancelled: true, state: 'queued' });
  await assert.rejects(waiting.promise, (err) => err.code === 'request_cancelled');

  // Well past the queue timeout. A leaked timer would fire a second event here.
  await sleep(45);
  assert.equal(events.filter((event) => event.state === 'timeout').length, 0);
  assert.equal(events.filter((event) => event.state === 'cancelled').length, 1);
  assert.equal(scheduler.getQueueStatus('waiting').state, 'cancelled');

  blocker.resolve('ok');
  await held.promise;
});

test('cancelling a running job aborts it and records it as cancelled', async () => {
  const scheduler = makeScheduler();
  const running = scheduler.enqueue({
    requestId: 'running',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: ({ signal }) => abortableJob(signal),
  });

  await waitFor(() => scheduler.active.has('running'));
  assert.deepEqual(scheduler.cancel('running'), { cancelled: true, state: 'active' });

  await assert.rejects(running.promise);
  assert.equal(scheduler.getQueueStatus('running').state, 'cancelled');
  assert.equal(scheduler.active.size, 0);
});

test('cancelling an unknown request id reports not_found', () => {
  const scheduler = makeScheduler();
  assert.deepEqual(scheduler.cancel('never-seen'), { cancelled: false, state: 'not_found' });
});

test('finished requests are evicted from the done map once their TTL passes', async () => {
  const scheduler = makeScheduler({ maxSlots: 2, doneTtlMs: 20 });

  await scheduler.enqueue({
    requestId: 'old',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => Promise.resolve('ok'),
  }).promise;

  assert.ok(scheduler.done.has('old'));
  await sleep(35);

  await scheduler.enqueue({
    requestId: 'fresh',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => Promise.resolve('ok'),
  }).promise;

  assert.equal(scheduler.done.has('old'), false, 'the stale entry went');
  assert.ok(scheduler.done.has('fresh'));
});

test('the done map never grows past doneMaxEntries, and drops the oldest first', async () => {
  const scheduler = makeScheduler({ doneMaxEntries: 3 });

  for (let i = 0; i < 5; i += 1) {
    await scheduler.enqueue({
      requestId: `job-${i}`,
      mfeId: 'doc-qa',
      priority: 'normal',
      execute: () => Promise.resolve('ok'),
    }).promise;
  }

  assert.equal(scheduler.done.size, 3);
  assert.deepEqual([...scheduler.done.keys()], ['job-2', 'job-3', 'job-4']);
});

test('getQueueStatus reports queued, active, done and not_found', async () => {
  const scheduler = makeScheduler();
  const blocker = deferred();

  const held = scheduler.enqueue({
    requestId: 'active-one',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => blocker.promise,
  });
  const waiting = scheduler.enqueue({
    requestId: 'queued-one',
    mfeId: 'doc-qa',
    priority: 'normal',
    execute: () => Promise.resolve('ok'),
  });

  const activeStatus = scheduler.getQueueStatus('active-one');
  assert.equal(activeStatus.state, 'active');
  assert.equal(activeStatus.position, 0);

  const queuedStatus = scheduler.getQueueStatus('queued-one');
  assert.equal(queuedStatus.state, 'queued');
  assert.equal(queuedStatus.position, 1);
  assert.ok(queuedStatus.estimatedWaitMs > 0);

  assert.equal(scheduler.getQueueStatus('nobody').state, 'not_found');
  assert.equal(scheduler.getQueueStatus('nobody').position, -1);

  blocker.resolve('ok');
  await held.promise;
  await waiting.promise;

  assert.equal(scheduler.getQueueStatus('active-one').state, 'done');
});
