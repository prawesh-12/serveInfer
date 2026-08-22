'use strict';

class SchedulerError extends Error {
  constructor(message, code, details = {}) {
    super(message);
    this.name = 'SchedulerError';
    this.code = code;
    this.details = details;
  }
}

class Scheduler {
  constructor(options = {}) {
    this.maxSlots = Number(options.maxSlots ?? 4);
    this.maxPerMfe = Number(options.maxPerMfe ?? 2);
    this.maxQueue = Number(options.maxQueue ?? 20);
    this.agingMs = Number(options.agingMs ?? 15_000);
    this.defaultDurationMs = Number(options.defaultDurationMs ?? 8_000);
    this.queueTimeoutMs = Number(options.queueTimeoutMs ?? 30_000);
    this.execTimeoutMs = Number(options.execTimeoutMs ?? 120_000);
    this.doneTtlMs = Number(options.doneTtlMs ?? 300_000);
    this.doneMaxEntries = Number(options.doneMaxEntries ?? 500);

    this.queue = [];
    this.active = new Map();
    this.done = new Map();
    this.perMfeActive = new Map();
    this.avgDurationMs = this.defaultDurationMs;
  }

  enqueue({ requestId, mfeId, priority, type, execute, onStatus }) {
    if (this.queue.length >= this.maxQueue) {
      const error = new SchedulerError('scheduler_overloaded', 'scheduler_overloaded', {
        requestId,
        maxQueue: this.maxQueue,
      });
      error.status = 429;
      throw error;
    }

    const createdAt = Date.now();
    const abortController = new AbortController();
    let resolvePromise;
    let rejectPromise;
    const promise = new Promise((resolve, reject) => {
      resolvePromise = resolve;
      rejectPromise = reject;
    });

    const job = {
      requestId,
      mfeId,
      priority: this.normalizePriority(priority),
      type: type || 'infer',
      execute,
      onStatus,
      createdAt,
      abortController,
      resolve: resolvePromise,
      reject: rejectPromise,
      queueTimer: null,
      execTimer: null,
      execTimedOut: false,
    };

    job.queueTimer = setTimeout(() => {
      const queueIndex = this.queue.findIndex((item) => item.requestId === requestId);
      if (queueIndex < 0) {
        return;
      }
      this.queue.splice(queueIndex, 1);
      const waitedMs = Date.now() - job.createdAt;
      this._notify(job, 'timeout', {
        waitedMs,
        retryable: true,
      });
      const error = new SchedulerError('queue_timeout', 'queue_timeout', {
        requestId,
        waitedMs,
        retryable: true,
      });
      error.status = 408;
      job.reject(error);
      this._recordDone(requestId, {
        state: 'timeout',
        requestId,
        waitedMs,
        retryable: true,
        finishedAt: Date.now(),
      });
    }, this.queueTimeoutMs);

    this.queue.push(job);
    this._notify(job, 'queued', {
      position: this.getQueuePosition(requestId),
      estimatedWaitMs: this._estimateWaitMs(this.getQueuePosition(requestId)),
    });
    this._schedule();

    return {
      requestId,
      promise,
      cancel: () => this.cancel(requestId),
    };
  }

  cancel(requestId) {
    const queueIndex = this.queue.findIndex((job) => job.requestId === requestId);
    if (queueIndex >= 0) {
      const [job] = this.queue.splice(queueIndex, 1);
      const error = new SchedulerError('request_cancelled', 'request_cancelled', {
        requestId,
        state: 'queued',
      });
      clearTimeout(job.queueTimer);
      clearTimeout(job.execTimer);
      this._notify(job, 'cancelled', {
        reason: 'user_cancelled',
      });
      job.reject(error);
      this._recordDone(requestId, {
        state: 'cancelled',
        requestId,
        finishedAt: Date.now(),
      });
      return { cancelled: true, state: 'queued' };
    }

    const activeJob = this.active.get(requestId);
    if (activeJob) {
      activeJob.abortController.abort();
      return { cancelled: true, state: 'active' };
    }

    const doneState = this.done.get(requestId);
    if (doneState) {
      return { cancelled: false, state: doneState.state };
    }
    return { cancelled: false, state: 'not_found' };
  }

  getQueueStatus(requestId) {
    const activeJob = this.active.get(requestId);
    if (activeJob) {
      return {
        requestId,
        state: 'active',
        position: 0,
        estimatedWaitMs: 0,
      };
    }

    const queueIndex = this.queue.findIndex((job) => job.requestId === requestId);
    if (queueIndex >= 0) {
      const position = this.getQueuePosition(requestId);
      return {
        requestId,
        state: 'queued',
        position,
        estimatedWaitMs: this._estimateWaitMs(position),
      };
    }

    if (this.done.has(requestId)) {
      return this.done.get(requestId);
    }

    return {
      requestId,
      state: 'not_found',
      position: -1,
      estimatedWaitMs: -1,
    };
  }

  getSnapshot() {
    return {
      limits: {
        maxSlots: this.maxSlots,
        maxPerMfe: this.maxPerMfe,
        maxQueue: this.maxQueue,
      },
      activeCount: this.active.size,
      queueLength: this.queue.length,
      activeByMfe: Object.fromEntries(this.perMfeActive),
    };
  }

  normalizePriority(priority) {
    const value = String(priority || 'normal').toLowerCase();
    if (value === 'high') return 'high';
    if (value === 'low') return 'low';
    return 'normal';
  }

  basePriority(priority) {
    if (priority === 'high') return 300;
    if (priority === 'normal') return 200;
    return 100;
  }

  effectivePriority(job, now) {
    const ageBoost = Math.floor((now - job.createdAt) / this.agingMs);
    return this.basePriority(job.priority) + ageBoost;
  }

  getQueuePosition(requestId) {
    const now = Date.now();
    const ordered = [...this.queue].sort((a, b) => {
      const p = this.effectivePriority(b, now) - this.effectivePriority(a, now);
      if (p !== 0) return p;
      return a.createdAt - b.createdAt;
    });
    const idx = ordered.findIndex((job) => job.requestId === requestId);
    return idx >= 0 ? idx + 1 : -1;
  }

  _estimateWaitMs(position) {
    if (position <= 0) return 0;
    const slots = Math.max(1, this.maxSlots);
    const batchesAhead = Math.ceil(position / slots);
    return batchesAhead * this.avgDurationMs;
  }

  _canRun(job) {
    if (this.active.size >= this.maxSlots) return false;
    const current = this.perMfeActive.get(job.mfeId) || 0;
    return current < this.maxPerMfe;
  }

  _pickNextJob() {
    const now = Date.now();
    const candidates = this.queue.filter((job) => this._canRun(job));
    if (candidates.length === 0) return null;
    candidates.sort((a, b) => {
      const p = this.effectivePriority(b, now) - this.effectivePriority(a, now);
      if (p !== 0) return p;
      return a.createdAt - b.createdAt;
    });
    return candidates[0];
  }

  _schedule() {
    while (this.active.size < this.maxSlots) {
      const job = this._pickNextJob();
      if (!job) return;
      const idx = this.queue.findIndex((item) => item.requestId === job.requestId);
      if (idx >= 0) {
        this.queue.splice(idx, 1);
      }
      this._run(job);
    }
  }

  _run(job) {
    const startedAt = Date.now();
    clearTimeout(job.queueTimer);
    this.active.set(job.requestId, job);
    this.perMfeActive.set(job.mfeId, (this.perMfeActive.get(job.mfeId) || 0) + 1);
    this._notify(job, 'started', { requestId: job.requestId });

    // Two things can end a job. It finishes, or the execution timeout fires.
    // Whichever happens first has to free the slot.
    //
    // Freeing it only in .finally() would not work. A job that never finishes
    // would hold its slot forever, and that is the exact case this timeout is
    // here to catch.
    let released = false;
    const releaseSlot = () => {
      if (released) {
        return;
      }
      released = true;
      clearTimeout(job.execTimer);
      const duration = Date.now() - startedAt;
      this.avgDurationMs = Math.round((this.avgDurationMs * 0.7) + (duration * 0.3));
      this.active.delete(job.requestId);
      const current = this.perMfeActive.get(job.mfeId) || 0;
      if (current <= 1) {
        this.perMfeActive.delete(job.mfeId);
      } else {
        this.perMfeActive.set(job.mfeId, current - 1);
      }
      this._schedule();
    };

    job.execTimer = setTimeout(() => {
      job.execTimedOut = true;
      const ranMs = Date.now() - startedAt;
      this._notify(job, 'timeout', {
        phase: 'execution',
        ranMs,
        retryable: true,
      });
      job.abortController.abort();

      const error = new SchedulerError('exec_timeout', 'exec_timeout', {
        requestId: job.requestId,
        ranMs,
        retryable: true,
      });
      error.status = 504;
      job.reject(error);
      this._recordDone(job.requestId, {
        requestId: job.requestId,
        state: 'timeout',
        phase: 'execution',
        ranMs,
        retryable: true,
        finishedAt: Date.now(),
      });
      releaseSlot();
    }, this.execTimeoutMs);

    Promise.resolve()
      .then(() => job.execute({ signal: job.abortController.signal }))
      .then((result) => {
        if (job.execTimedOut) {
          return; // the timeout already rejected this job and recorded it
        }
        job.resolve(result);
        this._recordDone(job.requestId, {
          requestId: job.requestId,
          state: 'done',
          finishedAt: Date.now(),
        });
      })
      .catch((err) => {
        if (job.execTimedOut) {
          return;
        }
        const cancelled = job.abortController.signal.aborted;
        if (cancelled) {
          job.reject(
            err instanceof Error
              ? err
              : new SchedulerError('request_cancelled', 'request_cancelled', {
                requestId: job.requestId,
              })
          );
          this._recordDone(job.requestId, {
            requestId: job.requestId,
            state: 'cancelled',
            finishedAt: Date.now(),
          });
        } else {
          job.reject(err);
          this._recordDone(job.requestId, {
            requestId: job.requestId,
            state: 'failed',
            finishedAt: Date.now(),
            message: err instanceof Error ? err.message : 'unknown_error',
          });
        }
      })
      .finally(() => {
        releaseSlot();
      });
  }

  _recordDone(requestId, entry) {
    this.done.set(requestId, entry);
    this._evictDone();
  }

  // The shell runs for a long time. Without this, the map never stops growing.
  _evictDone() {
    const cutoff = Date.now() - this.doneTtlMs;
    for (const [id, entry] of this.done) {
      if ((entry.finishedAt ?? 0) >= cutoff) {
        break; // insertion order is finish order, so the rest are newer
      }
      this.done.delete(id);
    }
    while (this.done.size > this.doneMaxEntries) {
      this.done.delete(this.done.keys().next().value);
    }
  }

  _notify(job, state, payload = {}) {
    if (typeof job.onStatus === 'function') {
      job.onStatus({
        state,
        requestId: job.requestId,
        ...payload,
      });
    }
  }
}

module.exports = {
  Scheduler,
  SchedulerError,
};
