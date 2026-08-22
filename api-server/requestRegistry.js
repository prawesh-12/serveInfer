'use strict';

const fs = require('node:fs');

// This class does two things.
//
// First, it tracks which requests are open right now, and writes them to a file.
// If the process dies, that file says what was in flight. The old
// "last request" file could not do this. It only ever held the most recent id,
// so under load it told you almost nothing.
//
// Second, it remembers finished requests for a while. If a client gets a 503 and
// retries with the same requestId, it gets the original answer back. We do not
// run the same inference twice.
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

  // Reads the requests the previous process still had open when it died.
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

  // A stream cannot re-send tokens it already sent. So the streaming route asks
  // what state an id is in first, and decides for itself what to do.
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

  // Returns { replay, state, promise }.
  // `replay` is true when we have seen this requestId before. The caller uses it
  // to label the response. No work is repeated either way.
  run({ requestId, mfeId, stream = false, execute }) {
    this._evictCompleted();

    const finished = this.completed.get(requestId);
    if (finished) {
      return { replay: true, state: 'completed', promise: Promise.resolve(finished.result) };
    }

    const open = this.inflight.get(requestId);
    if (open) {
      // Attach to the run that is already going. The same id never takes two
      // workers at once.
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
        // We do not cache failures, on purpose. A failed run produced nothing
        // worth keeping. Worse, caching the error would mean the client could
        // never retry that id.
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

  // Only successes get here, so lookup and run never replay a failure.

  _evictCompleted() {
    const cutoff = Date.now() - this.idempotencyTtlMs;
    for (const [id, entry] of this.completed) {
      if (entry.finishedAt >= cutoff) {
        break; // insertion order is finish order
      }
      this.completed.delete(id);
    }
  }

  // One write at a time, and a unique temp name for each one.
  //
  // Two writes to the same temp path followed by two renames is not atomic. The
  // published file can end up a mix of both. readInflightFile then fails to
  // parse it and quietly reports no orphans at all, which is worse than a crash
  // because nobody notices.
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
