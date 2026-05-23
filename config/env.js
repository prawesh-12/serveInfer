'use strict';

const fs = require('node:fs');
const path = require('node:path');

const rootDir = path.resolve(__dirname, '..');
const initialKeys = new Set(Object.keys(process.env));
let loaded = false;

function parseEnvLine(line) {
  const trimmed = line.trim();
  if (!trimmed || trimmed.startsWith('#') || !trimmed.includes('=')) {
    return null;
  }

  const idx = trimmed.indexOf('=');
  const key = trimmed.slice(0, idx).trim();
  let value = trimmed.slice(idx + 1).trim();
  if (!key) {
    return null;
  }

  const quote = value[0];
  if ((quote === '"' || quote === "'") && value[value.length - 1] === quote) {
    value = value.slice(1, -1);
  }
  return { key, value };
}

function loadFile(filename, overrideLoadedValues) {
  const filePath = path.join(rootDir, filename);
  if (!fs.existsSync(filePath)) {
    return;
  }

  const lines = fs.readFileSync(filePath, 'utf8').split(/\r?\n/);
  for (const line of lines) {
    const parsed = parseEnvLine(line);
    if (!parsed || initialKeys.has(parsed.key)) {
      continue;
    }
    if (overrideLoadedValues || process.env[parsed.key] === undefined) {
      process.env[parsed.key] = parsed.value;
    }
  }
}

function loadEnv() {
  if (loaded) {
    return;
  }
  loadFile('.env.example', false);
  loadFile('.env', true);
  loaded = true;
}

function requiredEnv(name) {
  loadEnv();
  const value = process.env[name];
  if (value === undefined || value === '') {
    throw new Error(`Missing required environment variable: ${name}`);
  }
  return value;
}

function numberEnv(name) {
  const raw = requiredEnv(name);
  const value = Number(raw);
  if (!Number.isFinite(value)) {
    throw new Error(`Environment variable ${name} must be a number`);
  }
  return value;
}

module.exports = {
  loadEnv,
  numberEnv,
  requiredEnv,
  rootDir,
};
