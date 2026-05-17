#pragma once

#include <string>

namespace EdgeIPC {
inline const std::string SUPERVISOR_SOCK = "/tmp/edge-supervisor.sock";
inline const std::string API_NOTIFY_SOCK = "/tmp/edge-api-notify.sock";
inline const std::string WORKER_SOCK_PREFIX = "/tmp/edge-worker-";
inline const std::string SHM_NAME = "/edge-model-weights";
inline const std::string CRASH_LOG = "/tmp/edge-crash.log";
inline const std::string MODEL_CONFIG = "/tmp/edge-model-config.json";
inline const std::string LAST_REQUEST = "/tmp/edge-last-request.json";

inline std::string workerSock(int id) {
  return WORKER_SOCK_PREFIX + std::to_string(id) + ".sock";
}

inline std::string shmMetaName(const std::string& shmName) {
  return shmName + ".meta";
}

inline std::string shmFilePath(const std::string& shmName) {
  if (shmName.empty()) {
    return {};
  }
  if (shmName[0] == '/') {
    return "/dev/shm/" + shmName.substr(1);
  }
  return "/dev/shm/" + shmName;
}
}  // namespace EdgeIPC
