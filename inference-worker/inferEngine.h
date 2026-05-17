#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

  std::string generate(const std::string& prompt);
  void generateStreaming(const std::string& prompt,
                         const std::function<void(const std::string&)>& onToken);

 private:
  bool loadModel();
  std::vector<int32_t> tokenize(const std::string& text);
  void runDecodeLoop(const std::function<void(const std::string&)>& onToken);

  InferConfig cfg_;
  bool gpuOk_ = true;
  void* model_ = nullptr;  // llama_model*
  void* ctx_ = nullptr;    // llama_context*
};

