#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "deviceLadder.h"
#include "gpuResourceOwner.h"

struct InferConfig {
  std::string modelPath;
  int gpuLayers = 99;
  int ctxSize = 2048;
  int maxTokens = 512;
  float temperature = 0.8f;
  int seed = 42;
  bool forceCpu = false;
};

class InferEngine : public GpuResourceOwner {
 public:
  explicit InferEngine(InferConfig cfg);
  ~InferEngine() override;

  bool init();
  bool isUsingGPU() const {
    return gpuOk_;
  }

  bool reloadOn(bool forceCpu);

  bool releaseDeviceResources() override;
  bool deviceResourcesResident() const override;

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

  // The tier this engine is executing for. EDGE_SIMULATE_DEVICE_FAULT matches against it.
  void setExecutingTier(std::string tier) {
    executingTier_ = std::move(tier);
  }

  std::string generate(const std::string& prompt);
  void generateStreaming(const std::string& prompt,
                         const std::function<void(const std::string&)>& onToken);

 private:
  bool loadModel();
  void freeModelAndContext();
  std::vector<int32_t> tokenize(const std::string& text);
  void runDecodeLoop(const std::function<void(const std::string&)>& onToken);

  void recordFault(DeviceFault fault, const std::string& detail);
  DeviceFault injectedFault();

  InferConfig cfg_;
  bool gpuOk_ = true;
  bool deviceInitialized_ = false;
  DeviceFault lastFault_ = DeviceFault::kNone;
  std::string lastFaultDetail_;
  std::string executingTier_;
  bool faultInjected_ = false;
  void* model_ = nullptr;  // llama_model*
  void* ctx_ = nullptr;    // llama_context*
};

