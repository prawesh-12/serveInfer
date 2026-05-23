#pragma once

#include <cstdlib>
#include <string>

namespace EdgeIPC {
inline std::string envValue(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
}

inline std::string supervisorSock() {
  return envValue("EDGE_SUPERVISOR_SOCK");
}

inline std::string apiNotifySock() {
  return envValue("EDGE_API_NOTIFY_SOCK");
}

inline std::string workerSockPrefix() {
  return envValue("EDGE_WORKER_SOCKET_PREFIX");
}

inline std::string shmName() {
  return envValue("EDGE_SHM_NAME");
}

inline std::string crashLog() {
  return envValue("EDGE_CRASH_LOG");
}

inline std::string modelConfigPath() {
  return envValue("EDGE_MODEL_CONFIG_PATH");
}

inline std::string workerSock(int id) {
  return workerSockPrefix() + std::to_string(id) + ".sock";
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
