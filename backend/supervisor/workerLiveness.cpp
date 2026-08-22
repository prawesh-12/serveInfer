#include "workerLiveness.h"

#include <cstdlib>

namespace {

bool extractNumber(const std::string& line, const std::string& key, long long& out) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) {
    return false;
  }
  const char* start = line.c_str() + at + needle.size();
  char* end = nullptr;
  const long long parsed = std::strtoll(start, &end, 10);
  if (end == start) {
    return false;
  }
  out = parsed;
  return true;
}

}  // namespace

const char* workerLivenessName(WorkerLiveness liveness) {
  switch (liveness) {
    case WorkerLiveness::kHealthy:
      return "healthy";
    case WorkerLiveness::kStarting:
      return "starting";
    case WorkerLiveness::kNoHeartbeat:
      return "no_heartbeat";
    case WorkerLiveness::kStuckRequest:
      return "stuck_request";
  }
  return "unknown";
}

WorkerLiveness classifyWorker(long long nowMs, const WorkerHealth& health,
                              const LivenessLimits& limits) {
  if (health.lastHeartbeatMs <= 0) {
    if (limits.startupGraceMs <= 0 || nowMs - health.spawnedAtMs <= limits.startupGraceMs) {
      return WorkerLiveness::kStarting;
    }
    return WorkerLiveness::kNoHeartbeat;
  }

  if (limits.heartbeatTimeoutMs > 0 && nowMs - health.lastHeartbeatMs > limits.heartbeatTimeoutMs) {
    return WorkerLiveness::kNoHeartbeat;
  }

  if (limits.stuckRequestMs > 0 && health.busyMs > limits.stuckRequestMs) {
    return WorkerLiveness::kStuckRequest;
  }

  return WorkerLiveness::kHealthy;
}

bool parseHeartbeat(const std::string& line, int& workerId, long long& busyMs) {
  if (line.find("\"type\":\"heartbeat\"") == std::string::npos) {
    return false;
  }
  long long id = 0;
  if (!extractNumber(line, "workerId", id) || id < 0) {
    return false;
  }
  workerId = static_cast<int>(id);
  long long busy = 0;
  busyMs = extractNumber(line, "busyMs", busy) ? busy : 0;
  return true;
}
