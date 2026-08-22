#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/types.h>

enum class ProcessType {
  kModelCache,
  kApiServer,
  kWorker,
};

struct ProcessInfo {
  pid_t pid = -1;
  ProcessType type = ProcessType::kWorker;
  int workerId = -1;
  std::string socketPath;
  std::string crashLog;
  std::vector<std::string> command;
};

struct SupervisorConfig {
  std::string modelPath;
  int workerCount = 2;
  std::string shmName;
  std::string supervisorSocketPath;
  std::string apiNotifySocketPath;
  int pollIntervalMs = 50;
  int modelReadyTimeoutSeconds = 30;

  std::string modelCacheBinary = "edge-model-cache";
  std::string workerBinary = "edge-inference-worker";
  std::string apiBinary = "node";
  std::string apiEntry = "api-server/server.js";
};

class CircuitBreaker {
 public:
  bool registerCrash(int workerId);

 private:
  static constexpr std::size_t kCrashThreshold = 3;
  static constexpr std::chrono::seconds kWindow = std::chrono::seconds(60);

  void pruneLocked(int workerId, std::chrono::steady_clock::time_point now);

  mutable std::unordered_map<int, std::deque<std::chrono::steady_clock::time_point>> history_;
};

class Supervisor {
 public:
  explicit Supervisor(SupervisorConfig config);
  ~Supervisor();

  bool start();
  int monitorLoop();
  void requestStop();

 private:
  bool setupSupervisorSocket();
  bool startModelCache();
  bool waitForModelReady() const;
  bool startApiServer();
  bool startWorkers();
  bool startWorker(int workerId);

  pid_t forkExec(const std::vector<std::string>& args) const;
  void reapChildren();
  void handleCrash(pid_t pid, int status);
  void shutdownChildren();
  void restartWorkersAfterModelCacheRestart();
  void cleanupSocket();
  void drainSupervisorSocket();

  void writeCrashLog(const ProcessInfo& info, int status, const std::string& reason) const;
  void writeModelConfig() const;
  bool crashLimitOpenFromDisk(const ProcessInfo& info) const;
  static void writePidFile(const std::string& name, pid_t pid);
  static void removePidFile(const std::string& name);
  static std::string pidFileNameFor(const ProcessInfo& info);

  bool notifyApiServerWorkerCrash(int workerId) const;
  bool notifyApiServerWorkerRestarted(int workerId) const;
  bool notifyApiServer(const char* type, int workerId) const;

  static std::string processTypeToString(ProcessType type);
  static std::string jsonEscape(const std::string& input);
  static std::string statusToReason(int status);

  SupervisorConfig config_;
  std::atomic<bool> running_{false};
  int supervisorServerFd_ = -1;
  CircuitBreaker circuitBreaker_;

  std::unordered_map<pid_t, ProcessInfo> processesByPid_;
  std::unordered_map<int, pid_t> workerPidById_;
};
