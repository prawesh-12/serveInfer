'use strict';

const fs = require('node:fs');

// EDGE_WORKER_COUNT is a ceiling; the supervisor publishes the count it could
// actually place as `workerCount` in $EDGE_MODEL_CONFIG_PATH, and WorkerPool
// pre-creates one pool entry per count, so the larger number over-provisions it.

// A count larger than this is not a constrained-host answer, it is a corrupt
// file. Fall back rather than pre-create thousands of pool entries.
const MAX_SANE_WORKER_COUNT = 1024;

function readModelConfig(modelConfigPath) {
  if (!modelConfigPath) {
    return { ok: false, reason: 'no EDGE_MODEL_CONFIG_PATH set' };
  }
  let raw;
  try {
    raw = fs.readFileSync(modelConfigPath, 'utf8');
  } catch (err) {
    const reason =
      err.code === 'ENOENT'
        ? 'model config not present'
        : `model config unreadable (${err.code || err.message})`;
    return { ok: false, reason };
  }
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return { ok: false, reason: 'model config is not valid JSON' };
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return { ok: false, reason: 'model config is not a JSON object' };
  }
  return { ok: true, config: parsed };
}

// Every failure here is a fallback, never a throw: the api-server has to boot
// standalone, and a missing model config is the normal case then.
function resolveWorkerCount({ configuredCount, modelConfigPath } = {}) {
  const ceiling = Number(configuredCount);
  const base = {
    configuredCount: ceiling,
    effectiveCount: null,
    clamped: false,
    modelConfigPath: modelConfigPath || null,
  };

  // Without a trustworthy ceiling there is nothing to clamp against; WorkerPool's
  // constructor is the one place that gets to reject it.
  if (!Number.isInteger(ceiling) || ceiling <= 0) {
    return {
      ...base,
      workerCount: ceiling,
      source: 'env',
      reason: 'EDGE_WORKER_COUNT is not a positive integer',
    };
  }

  const read = readModelConfig(modelConfigPath);
  if (!read.ok) {
    return { ...base, workerCount: ceiling, source: 'env', reason: read.reason };
  }

  if (!Object.prototype.hasOwnProperty.call(read.config, 'workerCount')) {
    return {
      ...base,
      workerCount: ceiling,
      source: 'env',
      reason: 'model config has no workerCount key',
    };
  }

  const effective = read.config.workerCount;
  if (typeof effective !== 'number' || !Number.isInteger(effective)) {
    return {
      ...base,
      workerCount: ceiling,
      source: 'env',
      reason: `model config workerCount is not an integer (${JSON.stringify(effective)})`,
    };
  }
  if (effective <= 0) {
    return {
      ...base,
      workerCount: ceiling,
      source: 'env',
      reason: `model config workerCount is not positive (${effective})`,
    };
  }
  if (effective > MAX_SANE_WORKER_COUNT) {
    return {
      ...base,
      workerCount: ceiling,
      source: 'env',
      reason: `model config workerCount ${effective} exceeds the sane maximum ${MAX_SANE_WORKER_COUNT}`,
    };
  }

  // An effective count above the ceiling it was derived from means the two sides
  // disagree about EDGE_WORKER_COUNT; trust the ceiling and never over-provision.
  if (effective > ceiling) {
    return {
      ...base,
      workerCount: ceiling,
      effectiveCount: effective,
      clamped: true,
      source: 'env',
      reason: `model config workerCount ${effective} exceeds the configured ceiling ${ceiling}`,
    };
  }

  return {
    ...base,
    workerCount: effective,
    effectiveCount: effective,
    source: 'model-config',
    reason: null,
  };
}

function logWorkerCountDecision(logger, decision) {
  if (!logger) {
    return;
  }
  const where = decision.modelConfigPath || '<unset>';
  if (decision.source === 'model-config') {
    const note =
      decision.workerCount < decision.configuredCount
        ? ' (capacity-limited: fewer workers were started than configured)'
        : '';
    logger.info(
      `worker pool size ${decision.workerCount} from effective count in ${where} ` +
        `(configured EDGE_WORKER_COUNT=${decision.configuredCount})${note}`
    );
    return;
  }
  logger.warn(
    `worker pool size ${decision.workerCount} from EDGE_WORKER_COUNT ` +
      `(configured=${decision.configuredCount}, effective from ${where} unavailable: ${decision.reason})`
  );
}

module.exports = {
  MAX_SANE_WORKER_COUNT,
  resolveWorkerCount,
  logWorkerCountDecision,
};
