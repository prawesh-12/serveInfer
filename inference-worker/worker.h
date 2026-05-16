#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

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
  std::size_t modelSizeBytes = 0;
  int heartbeatIntervalMs = 50;
  bool forceCpu = false;
};

class Worker {
 public:
  explicit Worker(WorkerConfig config);
  ~Worker();

  bool init();
  int run();
  void requestStop();

 private:
  bool attachSharedMemory();
  bool selectDevice();
  bool setupSocketServer();
  bool parseJob(const std::string& raw, InferenceJob& out, std::string& error) const;
  void handleClient(int clientFd);

  std::string inferBlocking(const std::string& prompt);
  std::vector<std::string> inferStreamingTokens(const std::string& prompt);

  void startHeartbeat();
  void stopHeartbeat();
  void heartbeatLoop();

  static bool sendAll(int fd, const std::string& data);
  static std::string jsonEscape(const std::string& input);

  WorkerConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cudaAvailable_{false};
  std::string activeDevice_{"cpu"};

  int serverFd_ = -1;
  int shmFd_ = -1;
  void* shmPtr_ = nullptr;
  std::size_t shmSize_ = 0;

  std::thread heartbeatThread_;
};
