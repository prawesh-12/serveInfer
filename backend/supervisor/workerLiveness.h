#pragma once

#include <cstdint>
#include <string>

enum class WorkerLiveness {
  kHealthy,
  kStarting,
  kNoHeartbeat,
  kStuckRequest,
};

const char* workerLivenessName(WorkerLiveness liveness);

struct LivenessLimits {
  // Loading a 2.4GB model beats before the first heartbeat, so silence is only fatal after this.
  long long startupGraceMs = 120000;
  long long heartbeatTimeoutMs = 15000;
  // The heartbeat runs on its own thread, so only elapsed request time can show a wedged decode.
  long long stuckRequestMs = 180000;
};

struct WorkerHealth {
  long long spawnedAtMs = 0;
  long long lastHeartbeatMs = 0;
  long long busyMs = 0;
};

WorkerLiveness classifyWorker(long long nowMs, const WorkerHealth& health, const LivenessLimits& limits);

bool parseHeartbeat(const std::string& line, int& workerId, long long& busyMs);
