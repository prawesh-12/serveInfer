'use strict';

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');

const rootDir = path.resolve(__dirname, '..');
const helperPath = path.join(rootDir, 'backend/remote/sarvamTransport.js');

// Stands in for the sarvamai package. NODE_PATH puts it ahead of the real SDK, so
// nothing in this file loads the vendor client, needs a key or opens a socket.
const stubSource = `'use strict';

class SarvamAIError extends Error {
  constructor({ message, statusCode }) {
    super(message);
    this.name = 'SarvamAIError';
    this.statusCode = statusCode;
  }
}

class SarvamAITimeoutError extends Error {
  constructor(message) {
    super(message);
    this.name = 'SarvamAITimeoutError';
  }
}

class SarvamAIClient {
  constructor(clientOptions) {
    this.clientOptions = clientOptions;
  }

  get chat() {
    return {
      completions: async (request, requestOptions) => {
        require('node:fs').writeFileSync(
          process.env.STUB_SARVAM_RECORD,
          JSON.stringify({ clientOptions: this.clientOptions, request, requestOptions }),
        );
        if (process.env.STUB_SARVAM_MODE === 'error') {
          throw new SarvamAIError({ message: 'Rate limit exceeded', statusCode: 429 });
        }
        if (process.env.STUB_SARVAM_MODE === 'timeout') {
          throw new SarvamAITimeoutError('Timeout exceeded when calling POST /v1/chat/completions.');
        }
        return {
          id: 'stub-1',
          object: 'chat.completion',
          created: 0,
          model: request.model,
          choices: [{
            index: 0,
            finish_reason: 'stop',
            message: { role: 'assistant', content: \`stubbed reply to: \${request.messages[0].content}\` },
          }],
        };
      },
    };
  }
}

module.exports = { SarvamAIClient, SarvamAIError, SarvamAITimeoutError };
`;

function makeStub() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'edge-sarvam-'));
  const moduleDir = path.join(dir, 'node_modules', 'sarvamai');
  fs.mkdirSync(moduleDir, { recursive: true });
  fs.writeFileSync(path.join(moduleDir, 'package.json'), '{"name":"sarvamai","main":"index.js"}');
  fs.writeFileSync(path.join(moduleDir, 'index.js'), stubSource);
  return { dir, recordPath: path.join(dir, 'seen.json') };
}

// Runs the helper exactly the way the C++ transport does: one JSON line in, one out.
function runHelper(request, { mode = 'ok', apiKey = 'test-key-never-sent', env = {} } = {}) {
  const stub = makeStub();
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [helperPath], {
      cwd: rootDir,
      env: {
        PATH: process.env.PATH,
        NODE_PATH: path.join(stub.dir, 'node_modules'),
        STUB_SARVAM_MODE: mode,
        STUB_SARVAM_RECORD: stub.recordPath,
        EDGE_SARVAM_API_KEY: apiKey,
        EDGE_REMOTE_ENDPOINT: '',
        ...env,
      },
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    let stdout = '';
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
    });
    child.on('error', reject);
    child.on('close', () => {
      const seen = fs.existsSync(stub.recordPath)
        ? JSON.parse(fs.readFileSync(stub.recordPath, 'utf8'))
        : null;
      fs.rmSync(stub.dir, { recursive: true, force: true });
      resolve({ raw: stdout, response: JSON.parse(stdout), seen });
    });

    child.stdin.end(`${JSON.stringify(request)}\n`);
  });
}

test('a successful call returns the model text and the shipped generation defaults (stubbed sdk, no network)', async () => {
  const { response, seen } = await runHelper({
    prompt: 'summarise the meeting',
    endpoint: 'https://example.invalid/v1',
  });

  assert.deepStrictEqual(response, {
    status: 200,
    text: 'stubbed reply to: summarise the meeting',
    error: '',
  });
  assert.strictEqual(seen.request.model, 'sarvam-105b-conversations');
  assert.strictEqual(seen.request.temperature, 0.2);
  assert.strictEqual(seen.request.top_p, 1);
  assert.strictEqual(seen.request.max_tokens, 2000);
  assert.deepStrictEqual(seen.request.messages, [{ role: 'user', content: 'summarise the meeting' }]);
  assert.strictEqual(seen.clientOptions.apiSubscriptionKey, 'test-key-never-sent');
  assert.strictEqual(seen.clientOptions.baseUrl, 'https://example.invalid/v1');
  // The ladder owns retry, so the SDK must not quietly retry inside one rung.
  assert.strictEqual(seen.requestOptions.maxRetries, 0);
});

test('an api failure carries its status through and returns no text (stubbed sdk, no network)', async () => {
  const { response } = await runHelper({ prompt: 'anything', endpoint: '' }, { mode: 'error' });

  assert.strictEqual(response.status, 429);
  assert.strictEqual(response.text, '');
  assert.match(response.error, /sarvam api error: Rate limit exceeded/);
});

test('a timed out call is a gateway timeout, not an empty answer (stubbed sdk, no network)', async () => {
  const { response } = await runHelper({ prompt: 'anything', endpoint: '' }, { mode: 'timeout' });

  assert.strictEqual(response.status, 504);
  assert.strictEqual(response.text, '');
  assert.match(response.error, /sarvam call timed out/);
});

test('a missing api key is refused before the sdk is ever loaded (stubbed sdk, no network)', async () => {
  const { response, seen } = await runHelper({ prompt: 'secret prompt', endpoint: '' }, { apiKey: '' });

  assert.strictEqual(response.status, 401);
  assert.strictEqual(response.text, '');
  assert.match(response.error, /EDGE_SARVAM_API_KEY is empty/);
  assert.strictEqual(seen, null);
});

test('a request carrying no prompt never reaches the sdk (stubbed sdk, no network)', async () => {
  const { response, seen } = await runHelper({ endpoint: '' });

  assert.strictEqual(response.status, 400);
  assert.strictEqual(response.text, '');
  assert.strictEqual(seen, null);
});

test('the configured model and sampling values override the shipped defaults (stubbed sdk, no network)', async () => {
  const { seen } = await runHelper({ prompt: 'hello', endpoint: '' }, {
    env: {
      EDGE_SARVAM_MODEL: 'sarvam-m',
      EDGE_SARVAM_TEMPERATURE: '0.9',
      EDGE_SARVAM_TOP_P: '0.5',
      EDGE_SARVAM_MAX_TOKENS: '64',
      EDGE_REMOTE_TIMEOUT_MS: '4000',
    },
  });

  assert.strictEqual(seen.request.model, 'sarvam-m');
  assert.strictEqual(seen.request.temperature, 0.9);
  assert.strictEqual(seen.request.top_p, 0.5);
  assert.strictEqual(seen.request.max_tokens, 64);
  assert.strictEqual(seen.requestOptions.timeoutInSeconds, 4);
});
