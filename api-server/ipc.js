'use strict';

const fs = require('node:fs');
const net = require('node:net');

class WorkerPoolError extends Error {
  constructor(message, code, statusCode = 500, details = {}) {
    super(message);
    this.name = 'WorkerPoolError';
    this.code = code;
    this.statusCode = statusCode;
    this.details = details;
  }
}

class WorkerPool {
  constructor(options = {}) {
    this.workerCount = Number(options.workerCount);
    this.workerSocketPrefix = options.workerSocketPrefix;
    this.connectTimeoutMs = Number(options.connectTimeoutMs);
    if (!Number.isInteger(this.workerCount) || this.workerCount <= 0) {
      throw new Error('WorkerPool requires a positive workerCount');
    }
    if (!this.workerSocketPrefix) {
      throw new Error('WorkerPool requires workerSocketPrefix');
    }
    if (!Number.isFinite(this.connectTimeoutMs) || this.connectTimeoutMs <= 0) {
      throw new Error('WorkerPool requires a positive connectTimeoutMs');
    }
    this.workers = new Map();
    this.inFlight = new Map();
    this.startTime = Date.now();

    for (let i = 0; i < this.workerCount; i += 1) {
      this.workers.set(i, {
        workerId: i,
        socketPath: `${this.workerSocketPrefix}${i}.sock`,
        status: 'starting',
      });
    }
  }

  getHealth() {
    const workers = Array.from(this.workers.values()).map((w) => ({
      id: w.workerId,
      status: w.status,
      socketPath: w.socketPath,
    }));
    const activeSlots = workers.filter((w) => w.status === 'busy').length;
    return {
      workers,
      activeSlots,
      uptimeMs: Date.now() - this.startTime,
    };
  }

  handleSupervisorMessage(msg) {
    if (!msg || typeof msg !== 'object') {
      return;
    }
    if (msg.type === 'worker_crashed' && Number.isInteger(msg.workerId)) {
      this._markWorkerCrashed(msg.workerId, msg.requestId || null);
      return;
    }
    if ((msg.type === 'worker_ready' || msg.type === 'worker_restarted') && Number.isInteger(msg.workerId)) {
      const worker = this.workers.get(msg.workerId);
      if (worker && worker.status !== 'busy') {
        worker.status = 'ready';
      }
    }
  }

  async runInference({ prompt, requestId, mfeId }) {
    const worker = this._acquireWorker();
    return this._runRequest({
      worker,
      requestId,
      prompt,
      mfeId,
      stream: false,
    });
  }

  runStreamingInference({ prompt, requestId, mfeId, onToken }) {
    const worker = this._acquireWorker();
    let cleanupDone = false;
    let resolvePromise;
    let rejectPromise;

    const streamPromise = new Promise((resolve, reject) => {
      resolvePromise = resolve;
      rejectPromise = reject;
    });

    const client = net.createConnection({ path: worker.socketPath });
    const connectTimer = setTimeout(() => {
      client.destroy(
        new WorkerPoolError(
          `worker ${worker.workerId} connect timeout`,
          'worker_connect_timeout',
          503,
          { workerId: worker.workerId }
        )
      );
    }, this.connectTimeoutMs);

    let done = false;
    let buffer = '';

    const cleanup = () => {
      if (cleanupDone) {
        return;
      }
      cleanupDone = true;
      clearTimeout(connectTimer);
      this.inFlight.delete(requestId);
      if (this.workers.get(worker.workerId)?.status !== 'crashed') {
        this.workers.get(worker.workerId).status = 'ready';
      }
    };

    const failInFlight = (err) => {
      if (done) {
        return;
      }
      done = true;
      cleanup();
      if (!client.destroyed) {
        client.destroy();
      }
      rejectPromise(err);
    };

    this.inFlight.set(requestId, {
      workerId: worker.workerId,
      fail: failInFlight,
    });

    client.on('connect', () => {
      clearTimeout(connectTimer);
      const payload = {
        type: 'infer',
        requestId,
        prompt,
        mfeId,
        stream: true,
      };
      client.write(`${JSON.stringify(payload)}\n`);
    });

    client.on('data', (chunk) => {
      buffer += chunk.toString('utf8');
      while (true) {
        const index = buffer.indexOf('\n');
        if (index < 0) {
          break;
        }
        const line = buffer.slice(0, index).trim();
        buffer = buffer.slice(index + 1);
        if (!line) {
          continue;
        }
        let msg;
        try {
          msg = JSON.parse(line);
        } catch (err) {
          failInFlight(
            new WorkerPoolError('invalid worker JSON frame', 'worker_bad_json', 502, {
              workerId: worker.workerId,
            })
          );
          return;
        }
        if (msg.type === 'token') {
          onToken?.(String(msg.token ?? ''));
          continue;
        }
        if (msg.type === 'error') {
          failInFlight(
            new WorkerPoolError(String(msg.error || 'worker_error'), 'worker_error', 502, {
              workerId: worker.workerId,
            })
          );
          return;
        }
        if (msg.type === 'result') {
          if (done) {
            return;
          }
          done = true;
          cleanup();
          if (!client.destroyed) {
            client.end();
          }
          resolvePromise({
            text: String(msg.text ?? ''),
            device: String(msg.device ?? 'cpu'),
            degraded: Boolean(msg.degraded),
          });
          return;
        }
      }
    });

    client.on('error', (err) => {
      failInFlight(
        new WorkerPoolError(err.message || 'worker socket error', 'worker_socket_error', 502, {
          workerId: worker.workerId,
        })
      );
    });

    client.on('close', () => {
      if (!done) {
        failInFlight(
          new WorkerPoolError('worker closed before result', 'worker_closed', 502, {
            workerId: worker.workerId,
          })
        );
      }
    });

    return {
      promise: streamPromise,
      cancel: () => failInFlight(new WorkerPoolError('stream_cancelled', 'stream_cancelled', 499)),
      workerId: worker.workerId,
    };
  }

  _acquireWorker() {
    this._refreshWorkerReadiness();
    const worker = Array.from(this.workers.values()).find((w) => w.status === 'ready');
    if (!worker) {
      throw new WorkerPoolError('no ready workers', 'no_ready_workers', 503, {
        retryAfterSeconds: 1,
      });
    }
    worker.status = 'busy';
    return worker;
  }

  _refreshWorkerReadiness() {
    for (const worker of this.workers.values()) {
      if (worker.status === 'busy' || worker.status === 'crashed') {
        continue;
      }
      worker.status = fs.existsSync(worker.socketPath) ? 'ready' : 'starting';
    }
  }

  _markWorkerCrashed(workerId, requestId) {
    const worker = this.workers.get(workerId);
    if (!worker) {
      return;
    }
    worker.status = 'crashed';

    let affected = null;
    if (requestId && this.inFlight.has(requestId)) {
      affected = [{ requestId, ...this.inFlight.get(requestId) }];
    } else {
      affected = Array.from(this.inFlight.entries())
        .filter(([, v]) => v.workerId === workerId)
        .map(([rid, v]) => ({ requestId: rid, ...v }));
    }

    for (const entry of affected) {
      entry.fail(
        new WorkerPoolError('worker crashed while request in-flight', 'worker_crashed', 503, {
          requestId: entry.requestId,
          workerId,
          retryAfterSeconds: 2,
        })
      );
    }

    setTimeout(() => {
      const current = this.workers.get(workerId);
      if (current && current.status === 'crashed') {
        current.status = 'ready';
      }
    }, 2000);
  }

  _runRequest({ worker, requestId, prompt, mfeId, stream }) {
    return new Promise((resolve, reject) => {
      const client = net.createConnection({ path: worker.socketPath });
      const connectTimer = setTimeout(() => {
        client.destroy(
          new WorkerPoolError(
            `worker ${worker.workerId} connect timeout`,
            'worker_connect_timeout',
            503,
            { workerId: worker.workerId }
          )
        );
      }, this.connectTimeoutMs);

      let done = false;
      let buffer = '';

      const cleanup = () => {
        clearTimeout(connectTimer);
        this.inFlight.delete(requestId);
        if (this.workers.get(worker.workerId)?.status !== 'crashed') {
          this.workers.get(worker.workerId).status = 'ready';
        }
      };

      const finalizeError = (err) => {
        if (done) {
          return;
        }
        done = true;
        cleanup();
        reject(err);
      };

      const failInFlight = (err) => {
        if (!client.destroyed) {
          client.destroy();
        }
        finalizeError(err);
      };

      this.inFlight.set(requestId, {
        workerId: worker.workerId,
        fail: failInFlight,
      });

      client.on('connect', () => {
        clearTimeout(connectTimer);
        const payload = {
          type: 'infer',
          requestId,
          prompt,
          mfeId,
          stream,
        };
        client.write(`${JSON.stringify(payload)}\n`);
      });

      client.on('data', (chunk) => {
        buffer += chunk.toString('utf8');
        while (true) {
          const index = buffer.indexOf('\n');
          if (index < 0) {
            break;
          }
          const line = buffer.slice(0, index).trim();
          buffer = buffer.slice(index + 1);
          if (!line) {
            continue;
          }

          let msg;
          try {
            msg = JSON.parse(line);
          } catch (err) {
            finalizeError(
              new WorkerPoolError('invalid worker JSON frame', 'worker_bad_json', 502, {
                workerId: worker.workerId,
              })
            );
            return;
          }

          if (msg.type === 'error') {
            finalizeError(
              new WorkerPoolError(String(msg.error || 'worker_error'), 'worker_error', 502, {
                workerId: worker.workerId,
              })
            );
            return;
          }

          if (msg.type === 'result') {
            if (done) {
              return;
            }
            done = true;
            cleanup();
            if (!client.destroyed) {
              client.end();
            }
            resolve({
              text: String(msg.text ?? ''),
              device: String(msg.device ?? 'cpu'),
              degraded: Boolean(msg.degraded),
            });
            return;
          }
        }
      });

      client.on('error', (err) => {
        finalizeError(
          new WorkerPoolError(err.message || 'worker socket error', 'worker_socket_error', 502, {
            workerId: worker.workerId,
          })
        );
      });

      client.on('close', () => {
        if (!done) {
          finalizeError(
            new WorkerPoolError('worker closed before result', 'worker_closed', 502, {
              workerId: worker.workerId,
            })
          );
        }
      });
    });
  }
}

module.exports = {
  WorkerPool,
  WorkerPoolError,
};
