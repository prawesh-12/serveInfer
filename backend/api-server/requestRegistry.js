'use strict';

const fs = require('node:fs');

class RequestRegistry {
  constructor({ inflightPath, idempotencyTtlMs }) {
    this.inflightPath = inflightPath;
    this.idempotencyTtlMs = Number(idempotencyTtlMs);
    this.inflight = new Map();
    this.completed = new Map();
    this.orphans = RequestRegistry.readInflightFile(inflightPath);
    this.writeSeq = 0;
    this.writeInFlight = false;
    this.writePending = false;
    this._persist();
  }

  static readInflightFile(inflightPath) {
    try {
      const raw = fs.readFileSync(inflightPath, 'utf8').trim();
      if (!raw) return [];
      const parsed = JSON.parse(raw);
      return Array.isArray(parsed?.inflight) ? parsed.inflight : [];
    } catch {
      return [];
    }
  }

  snapshot() {
    this._evictCompleted();
    return {
      inflight: Array.from(this.inflight.entries()).map(([requestId, v]) => ({
        requestId,
        mfeId: v.mfeId,
        stream: v.stream,
        startedAt: v.startedAt,
      })),
      completedCount: this.completed.size,
      orphanedFromPreviousRun: this.orphans,
    };
  }

  lookup(requestId) {
    this._evictCompleted();
    if (this.inflight.has(requestId)) {
      return { state: 'inflight' };
    }
    const finished = this.completed.get(requestId);
    if (finished) {
      return { state: 'completed', result: finished.result };
    }
    return { state: 'new' };
  }

  run({ requestId, mfeId, stream = false, execute }) {
    this._evictCompleted();

    const finished = this.completed.get(requestId);
    if (finished) {
      return { replay: true, state: 'completed', promise: Promise.resolve(finished.result) };
    }

    const open = this.inflight.get(requestId);
    if (open) {
      return { replay: true, state: 'inflight', promise: open.promise };
    }

    const startedAt = Date.now();
    const promise = Promise.resolve()
      .then(() => execute())
      .then((result) => {
        this._settle(requestId, { result });
        return result;
      })
      .catch((err) => {
        // Not cached on purpose: caching it would make the id un-retryable.
        this.inflight.delete(requestId);
        this._persist();
        throw err;
      });

    // Swallow the rejection on this stored copy. Otherwise a replay that nobody
    // awaits would crash the process as an unhandled rejection.
    promise.catch(() => {});

    this.inflight.set(requestId, { mfeId, stream, startedAt, promise });
    this._persist();
    return { replay: false, state: 'new', promise };
  }

  _settle(requestId, outcome) {
    this.inflight.delete(requestId);
    this.completed.set(requestId, { finishedAt: Date.now(), ...outcome });
    this._persist();
  }


  _evictCompleted() {
    const cutoff = Date.now() - this.idempotencyTtlMs;
    for (const [id, entry] of this.completed) {
      if (entry.finishedAt >= cutoff) {
        break; // insertion order is finish order
      }
      this.completed.delete(id);
    }
  }

  // Serialised with a unique temp name per write. Two writes to one temp path
  // publish a spliced file, which readInflightFile then silently reads as empty.
  _persist() {
    if (this.writeInFlight) {
      this.writePending = true;
      return;
    }
    this.writeInFlight = true;
    this.writePending = false;

    const payload = JSON.stringify({
      pid: process.pid,
      updatedAt: new Date().toISOString(),
      inflight: Array.from(this.inflight.entries()).map(([requestId, v]) => ({
        requestId,
        mfeId: v.mfeId,
        stream: v.stream,
        startedAt: v.startedAt,
      })),
    });

    this.writeSeq += 1;
    const tmp = `${this.inflightPath}.${process.pid}.${this.writeSeq}.tmp`;
    const finish = () => {
      this.writeInFlight = false;
      if (this.writePending) {
        this._persist();
      }
    };
    fs.writeFile(tmp, `${payload}\n`, (err) => {
      if (err) {
        finish();
        return;
      }
      fs.rename(tmp, this.inflightPath, () => finish());
    });
  }
}

module.exports = { RequestRegistry };
