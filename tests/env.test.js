'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');

const envSourcePath = path.join(__dirname, '..', 'backend', 'config', 'env.js');
const tempDirs = [];

// Each test compiles env.js itself with its own `process` and `__dirname`: parseEnvLine is
// private and the real module caches after the first load.
function loadEnvModule({ rootDir, processEnv = {} } = {}) {
  const dir = rootDir ?? fs.mkdtempSync(path.join(os.tmpdir(), 'edge-env-'));
  if (!rootDir) {
    tempDirs.push(dir);
  }
  const source = `${fs.readFileSync(envSourcePath, 'utf8')}
module.exports.parseEnvLine = parseEnvLine;
`;
  const compiled = vm.compileFunction(
    source,
    ['exports', 'require', 'module', '__filename', '__dirname', 'process'],
    { filename: envSourcePath }
  );
  const fakeModule = { exports: {} };
  compiled(
    fakeModule.exports,
    require,
    fakeModule,
    envSourcePath,
    // env.js lives at backend/config/, and resolves the repo root two levels up.
    path.join(dir, 'backend', 'config'),
    { env: processEnv }
  );
  return { env: fakeModule.exports, dir, processEnv };
}

function writeEnvFiles(dir, { example, dotenv }) {
  if (example !== undefined) {
    fs.writeFileSync(path.join(dir, '.env.example'), example);
  }
  if (dotenv !== undefined) {
    fs.writeFileSync(path.join(dir, '.env'), dotenv);
  }
}

test.after(() => {
  for (const dir of tempDirs) {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('parseEnvLine skips blank lines, comments and lines with no equals sign', () => {
  const { env } = loadEnvModule();
  assert.equal(env.parseEnvLine(''), null);
  assert.equal(env.parseEnvLine('   '), null);
  assert.equal(env.parseEnvLine('# EDGE_PORT=1234'), null);
  assert.equal(env.parseEnvLine('   # indented comment'), null);
  assert.equal(env.parseEnvLine('EDGE_PORT_WITH_NO_VALUE'), null);
  assert.equal(env.parseEnvLine('=orphan-value'), null);
});

test('parseEnvLine trims the key and value and strips one layer of quotes', () => {
  const { env } = loadEnvModule();
  assert.deepEqual(env.parseEnvLine('  EDGE_PORT = 3000  '), { key: 'EDGE_PORT', value: '3000' });
  assert.deepEqual(env.parseEnvLine('EDGE_NAME="quoted value"'), {
    key: 'EDGE_NAME',
    value: 'quoted value',
  });
  assert.deepEqual(env.parseEnvLine("EDGE_NAME='quoted value'"), {
    key: 'EDGE_NAME',
    value: 'quoted value',
  });
  assert.deepEqual(env.parseEnvLine('EDGE_NAME="unbalanced'), {
    key: 'EDGE_NAME',
    value: '"unbalanced',
  });
});

test('parseEnvLine keeps equals signs that appear inside the value', () => {
  const { env } = loadEnvModule();
  assert.deepEqual(env.parseEnvLine('EDGE_QUERY=a=b=c'), { key: 'EDGE_QUERY', value: 'a=b=c' });
  assert.deepEqual(env.parseEnvLine('EDGE_ORIGINS=http://127.0.0.1:5001?x=1'), {
    key: 'EDGE_ORIGINS',
    value: 'http://127.0.0.1:5001?x=1',
  });
  assert.deepEqual(env.parseEnvLine('EDGE_EMPTY='), { key: 'EDGE_EMPTY', value: '' });
});

test('requiredEnv throws when a variable is missing, and when it is set but empty', () => {
  const { env, dir } = loadEnvModule();
  writeEnvFiles(dir, { example: 'EDGE_SET=value\nEDGE_EMPTY=\n' });

  assert.equal(env.requiredEnv('EDGE_SET'), 'value');
  assert.throws(() => env.requiredEnv('EDGE_ABSENT'), /Missing required environment variable: EDGE_ABSENT/);
  assert.throws(() => env.requiredEnv('EDGE_EMPTY'), /Missing required environment variable: EDGE_EMPTY/);
});

test('numberEnv throws when the value is not a number', () => {
  const { env, dir } = loadEnvModule();
  writeEnvFiles(dir, { example: 'EDGE_SLOTS=4\nEDGE_LABEL=four\n' });

  assert.equal(env.numberEnv('EDGE_SLOTS'), 4);
  assert.throws(() => env.numberEnv('EDGE_LABEL'), /EDGE_LABEL must be a number/);
});

test('.env overrides .env.example, and a variable already in the environment beats both files', () => {
  const { env, dir, processEnv } = loadEnvModule({
    processEnv: { EDGE_FROM_SHELL: 'shell-wins' },
  });
  writeEnvFiles(dir, {
    example: 'EDGE_ONLY_EXAMPLE=example\nEDGE_SHARED=from-example\nEDGE_FROM_SHELL=file-loses\n',
    dotenv: 'EDGE_SHARED=from-dotenv\nEDGE_FROM_SHELL=dotenv-loses\n',
  });

  env.loadEnv();

  assert.equal(processEnv.EDGE_ONLY_EXAMPLE, 'example', 'a tracked default still loads');
  assert.equal(processEnv.EDGE_SHARED, 'from-dotenv', '.env is the last word between the files');
  assert.equal(processEnv.EDGE_FROM_SHELL, 'shell-wins', 'neither file can overwrite the shell');
});

test('loadEnv is a no-op when neither file exists, and requiredEnv then throws', () => {
  const { env } = loadEnvModule();
  env.loadEnv();
  assert.throws(() => env.requiredEnv('EDGE_ANYTHING'), /Missing required environment variable/);
});

test('the repo .env.example carries every variable the shipped config reads', () => {
  // .env is gitignored, so a value added only there crashes a fresh clone.
  const example = fs.readFileSync(path.join(__dirname, '..', '.env.example'), 'utf8');
  const { env } = loadEnvModule();
  const keys = new Set(
    example
      .split(/\r?\n/)
      .map((line) => env.parseEnvLine(line))
      .filter(Boolean)
      .map((entry) => entry.key)
  );

  for (const name of [
    'EDGE_MAX_SLOTS',
    'EDGE_MAX_PER_MFE',
    'EDGE_MAX_QUEUE',
    'EDGE_AGING_MS',
    'EDGE_QUEUE_TIMEOUT_MS',
    'EDGE_EXEC_TIMEOUT_MS',
    'EDGE_DONE_TTL_MS',
    'EDGE_DONE_MAX_ENTRIES',
    'EDGE_WORKER_COUNT',
    'EDGE_INFLIGHT_PATH',
    'EDGE_IDEMPOTENCY_TTL_MS',
    'EDGE_DEVICE_LADDER',
    'EDGE_ALLOWED_MFE_ORIGINS',
  ]) {
    assert.ok(keys.has(name), `${name} is missing from .env.example`);
  }
});

test('the shipped caps keep the per-MFE fairness rule meaningful', () => {
  // Equal values let one MFE hold every slot; more slots than workers turns a queue wait into a 503.
  const example = fs.readFileSync(path.join(__dirname, '..', '.env.example'), 'utf8');
  const { env } = loadEnvModule();
  const values = new Map(
    example
      .split(/\r?\n/)
      .map((line) => env.parseEnvLine(line))
      .filter(Boolean)
      .map((entry) => [entry.key, entry.value])
  );

  const slots = Number(values.get('EDGE_MAX_SLOTS'));
  const perMfe = Number(values.get('EDGE_MAX_PER_MFE'));
  const workers = Number(values.get('EDGE_WORKER_COUNT'));

  assert.ok(perMfe < slots, `EDGE_MAX_PER_MFE (${perMfe}) must be under EDGE_MAX_SLOTS (${slots})`);
  assert.ok(slots <= workers, `EDGE_MAX_SLOTS (${slots}) must not exceed EDGE_WORKER_COUNT (${workers})`);
});
