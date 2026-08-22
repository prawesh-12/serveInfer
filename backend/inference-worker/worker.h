#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backendRouter.h"
#include "deviceLadder.h"
#include "inferEngine.h"

struct InferenceJob {
  std::string requestId;
  std::string prompt;
  bool stream = false;
};

struct WorkerConfig {
  int workerId = 0;
  std::string socketPath;
  std::string supervisorSocketPath;
  std::string shmName;
  std::string modelPath;
  std::size_t modelSizeBytes = 0;
  int heartbeatIntervalMs = 50;
  bool forceCpu = false;
  int maxTokens = 512;
  float temperature = 0.8f;
  int gpuLayers = 99;
  int seed = 42;
  std::vector<std::string> deviceLadder{"cuda", "npu", "ane", "cpu", "remote"};
  int deviceQuarantineMs = 60000;
  int deviceProbeIntervalMs = 5000;

  // "cuda" or "cpu". Empty is the standalone case, where the ladder chooses.
  std::string assignedBackend;

  // "reexec" is the only way to give the CUDA primary context back.
  bool reexecAfterDeviceClassFallback = true;
};

class Worker {
 public:
  explicit Worker(WorkerConfig config);
  ~Worker();

  bool init();
  int run();
  void requestStop();

  int exitCode() const {
    return exitCode_;
  }

 private:
  bool attachSharedMemory();
  bool selectDevice();
  bool onTierChanged(const std::string& previous, const std::string& next);
  std::string generateWithFallback(const std::string& prompt);
  void streamWithFallback(const std::string& prompt,
                          const std::function<void(const std::string&)>& onToken,
                          std::string& merged);
  std::string deviceResultFields() const;
  bool setupSocketServer();
  bool parseJob(const std::string& raw, InferenceJob& out, std::string& error) const;
  void handleClient(int clientFd);

  void startHeartbeat();
  void stopHeartbeat();
  void heartbeatLoop();

  static bool sendAll(int fd, const std::string& data);
  static std::string jsonEscape(const std::string& input);

  WorkerConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cudaAvailable_{false};
  std::string activeDevice_{"cpu"};
  std::unique_ptr<BackendRouter> router_;
  std::shared_ptr<QualcommHexagonBackend> hexagonBackend_;
  int exitCode_ = 0;
  // A fallback rebuilds the engine in place, so only one escalation may run at a time.
  std::mutex engineMutex_;
  InferEngine* engine_ = nullptr;

  int serverFd_ = -1;
  int shmFd_ = -1;
  void* shmPtr_ = nullptr;
  std::size_t shmSize_ = 0;

  std::thread heartbeatThread_;
  // Epoch ms when the current request started, 0 when idle. The heartbeat thread reads it.
  std::atomic<long long> requestStartedAtMs_{0};
};
