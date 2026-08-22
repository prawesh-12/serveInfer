'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const {
  resolveWorkerCount,
  logWorkerCountDecision,
  MAX_SANE_WORKER_COUNT,
} = require('../backend/api-server/workerCountSource');
const { WorkerPool } = require('../backend/api-server/ipc');

// Fixtures are real files, because the whole point is that this reads the one
// the supervisor wrote. They go in a temp dir that is removed afterwards.
let tmpDir = null;
let counter = 0;

function tempDir() {
  if (!tmpDir) {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'edge-wcs-'));
  }
  return tmpDir;
}

// Writes a model-config fixture and returns its path. `body` is written
// verbatim when it is a string, so a test can supply malformed JSON.
function writeModelConfig(body) {
  counter += 1;
  const filePath = path.join(tempDir(), `model-config-${counter}.json`);
  fs.writeFileSync(filePath, typeof body === 'string' ? body : JSON.stringify(body));
  return filePath;
}

function missingPath() {
  counter += 1;
  return path.join(tempDir(), `absent-${counter}.json`);
}

// The shape the supervisor actually emits (Supervisor::writeModelConfig).
function supervisorConfig(workerCount, configuredWorkerCount = 4) {
  return {
    modelPath: '/models/phi-3.gguf',
    shmName: '/edge-model-weights',
    workerCount,
    configuredWorkerCount,
    pollIntervalMs: 50,
  };
}

function collectLogger() {
  const lines = [];
  return {
    lines,
    debug: (...a) => lines.push(['debug', a.join(' ')]),
    info: (...a) => lines.push(['info', a.join(' ')]),
    warn: (...a) => lines.push(['warn', a.join(' ')]),
    error: (...a) => lines.push(['error', a.join(' ')]),
  };
}

test.after(() => {
  if (tmpDir) {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
});

test('the effective count from the model config wins over EDGE_WORKER_COUNT', () => {
  const modelConfigPath = writeModelConfig(supervisorConfig(2, 4));
  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  assert.equal(decision.workerCount, 2);
  assert.equal(decision.source, 'model-config');
  assert.equal(decision.configuredCount, 4);
  assert.equal(decision.effectiveCount, 2);
  assert.equal(decision.reason, null);
});

test('the resolved count is what WorkerPool pre-creates entries for', () => {
  const modelConfigPath = writeModelConfig(supervisorConfig(2, 4));
  const { workerCount } = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  const pool = new WorkerPool({
    workerCount,
    workerSocketPrefix: path.join(tempDir(), 'sock-'),
    connectTimeoutMs: 50,
  });

  // No pool entry exists for a worker the supervisor never started.
  assert.equal(pool.workers.size, 2);
  assert.deepEqual(
    pool.getHealth().workers.map((w) => w.id),
    [0, 1]
  );
});

test('an unconstrained host keeps the full configured count', () => {
  const modelConfigPath = writeModelConfig(supervisorConfig(4, 4));
  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  assert.equal(decision.workerCount, 4);
  assert.equal(decision.source, 'model-config');
});

test('a missing model config file falls back to EDGE_WORKER_COUNT', () => {
  const decision = resolveWorkerCount({
    configuredCount: 4,
    modelConfigPath: missingPath(),
  });

  assert.equal(decision.workerCount, 4);
  assert.equal(decision.source, 'env');
  assert.match(decision.reason, /not present/);
});

test('no configured model config path at all falls back without throwing', () => {
  for (const modelConfigPath of [undefined, null, '']) {
    const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });
    assert.equal(decision.workerCount, 4);
    assert.equal(decision.source, 'env');
  }
});

test('an unreadable model config path falls back instead of throwing', () => {
  // A directory is readable as a path but not as a file: EISDIR, not ENOENT.
  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath: tempDir() });

  assert.equal(decision.workerCount, 4);
  assert.equal(decision.source, 'env');
  assert.match(decision.reason, /unreadable/);
});

test('malformed JSON in the model config falls back', () => {
  const modelConfigPath = writeModelConfig('{"modelPath":"/m.gguf","workerCount":2');
  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  assert.equal(decision.workerCount, 4);
  assert.equal(decision.source, 'env');
  assert.match(decision.reason, /not valid JSON/);
});

test('an empty model config file falls back', () => {
  const modelConfigPath = writeModelConfig('');
  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  assert.equal(decision.workerCount, 4);
  assert.equal(decision.source, 'env');
});

test('valid JSON that is not an object falls back', () => {
  for (const body of ['null', '7', '"four"', '[2]']) {
    const decision = resolveWorkerCount({
      configuredCount: 4,
      modelConfigPath: writeModelConfig(body),
    });
    assert.equal(decision.workerCount, 4, `body ${body}`);
    assert.equal(decision.source, 'env', `body ${body}`);
  }
});

test('a model config with no workerCount key falls back', () => {
  const modelConfigPath = writeModelConfig({
    modelPath: '/models/phi-3.gguf',
    shmName: '/edge-model-weights',
    pollIntervalMs: 50,
  });
  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  assert.equal(decision.workerCount, 4);
  assert.equal(decision.source, 'env');
  assert.match(decision.reason, /no workerCount key/);
});

test('zero, negative and non-integer workerCount values all fall back', () => {
  const bad = [0, -1, -4, 2.5, Number.NaN, '2', null, true, [2], { n: 2 }];
  for (const value of bad) {
    const decision = resolveWorkerCount({
      configuredCount: 4,
      modelConfigPath: writeModelConfig(supervisorConfig(value, 4)),
    });
    assert.equal(decision.workerCount, 4, `value ${JSON.stringify(value)}`);
    assert.equal(decision.source, 'env', `value ${JSON.stringify(value)}`);
  }
});

test('an absurd workerCount falls back rather than pre-creating the pool', () => {
  const modelConfigPath = writeModelConfig(
    supervisorConfig(MAX_SANE_WORKER_COUNT + 1, MAX_SANE_WORKER_COUNT + 1)
  );
  const decision = resolveWorkerCount({
    configuredCount: MAX_SANE_WORKER_COUNT + 1,
    modelConfigPath,
  });

  assert.equal(decision.workerCount, MAX_SANE_WORKER_COUNT + 1);
  assert.equal(decision.source, 'env');
  assert.match(decision.reason, /sane maximum/);
});

test('the effective count never exceeds the configured ceiling', () => {
  const modelConfigPath = writeModelConfig(supervisorConfig(9, 9));
  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  assert.equal(decision.workerCount, 4);
  assert.equal(decision.clamped, true);
  assert.equal(decision.effectiveCount, 9);
  assert.equal(decision.source, 'env');
  assert.match(decision.reason, /exceeds the configured ceiling/);
});

test('a bad EDGE_WORKER_COUNT is passed through for WorkerPool to reject', () => {
  const modelConfigPath = writeModelConfig(supervisorConfig(2, 4));
  const decision = resolveWorkerCount({ configuredCount: 0, modelConfigPath });

  assert.equal(decision.source, 'env');
  assert.equal(decision.workerCount, 0);
  assert.throws(
    () =>
      new WorkerPool({
        workerCount: decision.workerCount,
        workerSocketPrefix: path.join(tempDir(), 'sock-'),
        connectTimeoutMs: 50,
      }),
    /positive workerCount/
  );
});

test('the decision is logged once, naming the winning source and both numbers', () => {
  const fromConfig = collectLogger();
  logWorkerCountDecision(
    fromConfig,
    resolveWorkerCount({
      configuredCount: 4,
      modelConfigPath: writeModelConfig(supervisorConfig(2, 4)),
    })
  );
  assert.equal(fromConfig.lines.length, 1);
  const [configLevel, configLine] = fromConfig.lines[0];
  assert.equal(configLevel, 'info');
  assert.match(configLine, /worker pool size 2/);
  assert.match(configLine, /EDGE_WORKER_COUNT=4/);
  assert.match(configLine, /capacity-limited/);

  const fromEnv = collectLogger();
  logWorkerCountDecision(
    fromEnv,
    resolveWorkerCount({ configuredCount: 4, modelConfigPath: missingPath() })
  );
  assert.equal(fromEnv.lines.length, 1);
  const [envLevel, envLine] = fromEnv.lines[0];
  assert.equal(envLevel, 'warn');
  assert.match(envLine, /worker pool size 4/);
  assert.match(envLine, /EDGE_WORKER_COUNT/);
  assert.match(envLine, /not present/);
});

test('the scheduler slot limit is a different number and is untouched by this', () => {
  const modelConfigPath = writeModelConfig(supervisorConfig(2, 4));
  const before = process.env.EDGE_MAX_SLOTS;

  const decision = resolveWorkerCount({ configuredCount: 4, modelConfigPath });

  assert.equal(decision.workerCount, 2);
  // Resolving the pool size must not reach into the scheduler's config.
  assert.equal(process.env.EDGE_MAX_SLOTS, before);

  const source = fs.readFileSync(
    path.join(__dirname, '..', 'backend', 'api-server', 'workerCountSource.js'),
    'utf8'
  );
  assert.equal(source.includes('EDGE_MAX_SLOTS'), false);
  assert.equal(source.includes('EDGE_MAX_PER_MFE'), false);
});
