'use strict';

// One JSON request on stdin, one JSON response line on stdout, then exit. The C++
// worker cannot call a Node SDK in-process, so the remote tier reaches Sarvam the
// same way the supervisor reaches hardware discovery: through a short-lived child.

const path = require('node:path');

const { loadEnv } = require('../config/env');

const rootDir = path.resolve(__dirname, '../..');

function numberOr(raw, fallback) {
  const value = Number(raw);
  return raw === undefined || raw === '' || !Number.isFinite(value) ? fallback : value;
}

function readConfig() {
  loadEnv();
  return {
    apiKey: process.env.EDGE_SARVAM_API_KEY || '',
    baseUrl: process.env.EDGE_REMOTE_ENDPOINT || '',
    model: process.env.EDGE_SARVAM_MODEL || 'sarvam-105b-conversations',
    temperature: numberOr(process.env.EDGE_SARVAM_TEMPERATURE, 0.2),
    topP: numberOr(process.env.EDGE_SARVAM_TOP_P, 1),
    maxTokens: numberOr(process.env.EDGE_SARVAM_MAX_TOKENS, 2000),
    timeoutMs: numberOr(process.env.EDGE_REMOTE_TIMEOUT_MS, 30000),
  };
}

// The SDK is a dependency of the api-server tier; this helper has no node_modules of its own.
function loadSdk() {
  try {
    return require('sarvamai');
  } catch (err) {
    if (err.code !== 'MODULE_NOT_FOUND') {
      throw err;
    }
    return require(require.resolve('sarvamai', { paths: [path.join(rootDir, 'backend', 'api-server')] }));
  }
}

function reply(status, text, error) {
  return { status, text, error };
}

function classify(err, SarvamAIError, SarvamAITimeoutError) {
  if (err instanceof SarvamAITimeoutError || err?.name === 'SarvamAITimeoutError') {
    return reply(504, '', `sarvam call timed out: ${err.message}`);
  }
  if (err instanceof SarvamAIError || typeof err?.statusCode === 'number') {
    // No statusCode means the request never reached a server, which is what 0 says.
    return reply(typeof err.statusCode === 'number' ? err.statusCode : 0, '',
      `sarvam api error: ${err.message}`);
  }
  return reply(0, '', `sarvam call failed: ${err?.message ?? String(err)}`);
}

async function call(request) {
  const config = readConfig();
  if (!config.apiKey) {
    return reply(401, '', 'EDGE_SARVAM_API_KEY is empty, so no prompt left this device');
  }

  let sdk;
  try {
    sdk = loadSdk();
  } catch (err) {
    return reply(0, '', `sarvamai sdk is not installed: ${err.message}`);
  }

  const { SarvamAIClient, SarvamAIError, SarvamAITimeoutError } = sdk;
  // The caller's endpoint wins: it is the one the operator declared consent against.
  const baseUrl = request.endpoint || config.baseUrl;
  const client = new SarvamAIClient({
    apiSubscriptionKey: config.apiKey,
    ...(baseUrl ? { baseUrl } : {}),
  });

  try {
    const response = await client.chat.completions({
      messages: [{ role: 'user', content: request.prompt }],
      model: config.model,
      temperature: config.temperature,
      top_p: config.topP,
      max_tokens: config.maxTokens,
    }, {
      timeoutInSeconds: config.timeoutMs / 1000,
      // The ladder owns retry and fallback; a hidden retry inside one rung hides the fault.
      maxRetries: 0,
    });
    const text = response?.choices?.[0]?.message?.content;
    if (typeof text !== 'string' || text === '') {
      return reply(502, '', 'sarvam returned no message content');
    }
    return reply(200, text, '');
  } catch (err) {
    return classify(err, SarvamAIError, SarvamAITimeoutError);
  }
}

function readStdin() {
  return new Promise((resolve) => {
    let raw = '';
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', (chunk) => {
      raw += chunk;
    });
    process.stdin.on('end', () => resolve(raw));
  });
}

async function main() {
  let result;
  try {
    const request = JSON.parse(await readStdin());
    result = typeof request?.prompt === 'string' && request.prompt !== ''
      ? await call(request)
      : reply(400, '', 'transport request carried no prompt');
  } catch (err) {
    result = reply(0, '', `transport request was unreadable: ${err.message}`);
  }
  // Exit on the flush: the SDK can leave a keep-alive socket that would hold the child open.
  process.stdout.write(`${JSON.stringify(result)}\n`, () => process.exit(0));
}

main();
