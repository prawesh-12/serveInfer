#pragma once

#include <string>

namespace EdgeIPC {
inline const std::string SUPERVISOR_SOCK = "/tmp/edge-supervisor.sock";
inline const std::string API_NOTIFY_SOCK = "/tmp/edge-api-notify.sock";
inline const std::string WORKER_SOCK_PREFIX = "/tmp/edge-worker-";
inline const std::string SHM_NAME = "/edge-model-weights";
inline const std::string CRASH_LOG = "/tmp/edge-crash.log";

inline std::string workerSock(int id) {
  return WORKER_SOCK_PREFIX + std::to_string(id) + ".sock";
}
}  // namespace EdgeIPC
