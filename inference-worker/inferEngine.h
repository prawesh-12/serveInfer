#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "deviceLadder.h"

struct InferConfig {
  std::string modelPath;
  int gpuLayers = 99;
  int ctxSize = 2048;
  int maxTokens = 512;
  float temperature = 0.8f;
  int seed = 42;
  bool forceCpu = false;
};

class InferEngine {
 public:
  explicit InferEngine(InferConfig cfg);
  ~InferEngine();

  bool init();
  bool isUsingGPU() const {
    return gpuOk_;
  }

  // Reloads the model on a different tier after the ladder moved down. The
  // bytes come from /dev/shm, so this is much cheaper than a cold start.
  bool reloadOn(bool forceCpu);

  // Set when the last generate call hit a real backend failure, not just a
  // short answer. The worker turns this into a ladder fault.
  DeviceFault lastFault() const {
    return lastFault_;
  }
  const std::string& lastFaultDetail() const {
    return lastFaultDetail_;
  }
  void clearFault() {
    lastFault_ = DeviceFault::kNone;
    lastFaultDetail_.clear();
  }

  std::string generate(const std::string& prompt);
  void generateStreaming(const std::string& prompt,
                         const std::function<void(const std::string&)>& onToken);

 private:
  bool loadModel();
  std::vector<int32_t> tokenize(const std::string& text);
  void runDecodeLoop(const std::function<void(const std::string&)>& onToken);

  void recordFault(DeviceFault fault, const std::string& detail);
  DeviceFault injectedFault() const;

  InferConfig cfg_;
  bool gpuOk_ = true;
  DeviceFault lastFault_ = DeviceFault::kNone;
  std::string lastFaultDetail_;
  void* model_ = nullptr;  // llama_model*
  void* ctx_ = nullptr;    // llama_context*
};

