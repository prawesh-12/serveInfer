'use strict';

const fs = require('node:fs');

// EDGE_WORKER_COUNT is a ceiling, not a promise. The supervisor decides how
// much of it the machine can actually pay for (placeableWorkerCount) and starts
// only that many, then publishes the answer as `workerCount` in
// $EDGE_MODEL_CONFIG_PATH — see Supervisor::writeModelConfig, which runs before
// startApiServer, so the file is already on disk by the time we read it here.
//
// Reading it matters because WorkerPool pre-creates one pool entry per count.
// Entries for workers that were never started are gated out by
// _refreshWorkerReadiness (their socket never appears), but the scheduler
// upstream still admits work against the larger number and the request comes
// back 503 no_ready_workers instead of queueing.
//
// Every failure here is a fallback, never a throw: the api-server has to boot
// standalone, and a missing model config is the normal case then.

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

/**
 * Decide how many pool entries the WorkerPool should get.
 *
 * @param {object} options
 * @param {number} options.configuredCount  EDGE_WORKER_COUNT, the ceiling.
 * @param {string} [options.modelConfigPath] $EDGE_MODEL_CONFIG_PATH.
 * @returns {{workerCount:number, source:'model-config'|'env', configuredCount:number,
 *           effectiveCount:number|null, reason:string|null, clamped:boolean,
 *           modelConfigPath:string|null}}
 */
function resolveWorkerCount({ configuredCount, modelConfigPath } = {}) {
  const ceiling = Number(configuredCount);
  const base = {
    configuredCount: ceiling,
    effectiveCount: null,
    clamped: false,
    modelConfigPath: modelConfigPath || null,
  };

  // Without a trustworthy ceiling there is nothing to clamp against and
  // nothing to fall back to; hand the value straight to WorkerPool, whose
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

  // The effective count can never legitimately exceed the ceiling it was
  // derived from. If it does, the two sides disagree about EDGE_WORKER_COUNT;
  // trust the ceiling, because over-provisioning the pool is the failure mode
  // this whole module exists to remove.
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

// One line at startup, so an operator can see why the pool is smaller than
// EDGE_WORKER_COUNT without reading the supervisor's stderr.
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
