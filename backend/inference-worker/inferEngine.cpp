#include "inferEngine.h"

#if defined(EDGE_USE_LLAMA)
#include "llama.h"
#endif

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

void InferEngine::recordFault(DeviceFault fault, const std::string& detail) {
  lastFault_ = fault;
  lastFaultDetail_ = detail;
}

namespace {

DeviceFault faultByName(const std::string& name) {
  if (name == "removed") {
    return DeviceFault::kRemoved;
  }
  if (name == "unsupported") {
    return DeviceFault::kUnsupportedOp;
  }
  if (name == "runtime") {
    return DeviceFault::kRuntimeError;
  }
  return DeviceFault::kNone;
}

}  // namespace

// EDGE_SIMULATE_DEVICE_FAULT is "[<tier>:]<fault>", and fires at most once per worker: a
// fault that repeated would break the tier it fell back to as well, and again after a respawn.
DeviceFault InferEngine::injectedFault() {
  if (faultInjected_) {
    return DeviceFault::kNone;
  }
  const char* raw = std::getenv("EDGE_SIMULATE_DEVICE_FAULT");
  if (raw == nullptr || raw[0] == '\0') {
    return DeviceFault::kNone;
  }

  std::string spec(raw);
  const std::size_t colon = spec.find(':');
  if (colon != std::string::npos) {
    // An empty target is a typo, not a wildcard: ":removed" must not fire on an unnamed tier.
    const std::string target = spec.substr(0, colon);
    if (target.empty() || target != executingTier_) {
      return DeviceFault::kNone;
    }
    spec.erase(0, colon + 1);
  }

  const DeviceFault fault = faultByName(spec);
  if (fault != DeviceFault::kNone) {
    faultInjected_ = true;
  }
  return fault;
}

void InferEngine::freeModelAndContext() {
#if defined(EDGE_USE_LLAMA)
  // Context first: it holds the ggml_backend instances whose teardown frees the CUDA pool.
  if (ctx_ != nullptr) {
    llama_free(static_cast<llama_context*>(ctx_));
    ctx_ = nullptr;
  }
  if (model_ != nullptr) {
    llama_model_free(static_cast<llama_model*>(model_));
    model_ = nullptr;
  }
#endif
}

bool InferEngine::releaseDeviceResources() {
  freeModelAndContext();
  return model_ == nullptr && ctx_ == nullptr;
}

// Freeing model and context never releases the CUDA primary context; llama.cpp cannot.
bool InferEngine::deviceResourcesResident() const {
  return deviceInitialized_;
}

bool InferEngine::reloadOn(bool forceCpu) {
  freeModelAndContext();
  cfg_.forceCpu = forceCpu;
  gpuOk_ = !forceCpu;
  clearFault();
#if defined(EDGE_USE_LLAMA)
  return loadModel();
#else
  return true;
#endif
}

InferEngine::InferEngine(InferConfig cfg) : cfg_(std::move(cfg)) {}

InferEngine::~InferEngine() {
  freeModelAndContext();
#if defined(EDGE_USE_LLAMA)
  llama_backend_free();
#endif
}

bool InferEngine::init() {
#if defined(EDGE_USE_LLAMA)
  if (cfg_.modelPath.empty()) {
    std::cerr << "[infer-engine] model path is empty\n";
    return false;
  }

  llama_backend_init();
  return loadModel();
#else
  return true;
#endif
}

bool InferEngine::loadModel() {
#if defined(EDGE_USE_LLAMA)
  llama_model_params mparams = llama_model_default_params();
  if (cfg_.forceCpu) {
    gpuOk_ = false;
  }
  mparams.n_gpu_layers = gpuOk_ ? cfg_.gpuLayers : 0;

  if (gpuOk_) {
    // Point of no return: touching a CUDA device pins its primary context for the process.
    deviceInitialized_ = true;
  }

  llama_model* model = llama_model_load_from_file(cfg_.modelPath.c_str(), mparams);
  if (model == nullptr && gpuOk_) {
    std::cerr << "[infer-engine] GPU model load failed, retrying on CPU\n";
    gpuOk_ = false;
    mparams.n_gpu_layers = 0;
    model = llama_model_load_from_file(cfg_.modelPath.c_str(), mparams);
  }
  if (model == nullptr) {
    std::cerr << "[infer-engine] failed to load model: " << cfg_.modelPath << '\n';
    return false;
  }

  llama_context_params cparams = llama_context_default_params();
  cparams.n_ctx = cfg_.ctxSize;
  cparams.n_batch = 512;
  cparams.n_ubatch = 512;

  llama_context* ctx = llama_init_from_model(model, cparams);
  if (ctx == nullptr) {
    // model_ is assigned only after this succeeds; an earlier assignment leaked a live model.
    std::cerr << "[infer-engine] failed to create llama context\n";
    llama_model_free(model);
    return false;
  }

  model_ = model;
  ctx_ = ctx;
  std::cerr << "[infer-engine] loaded model on " << (gpuOk_ ? "cuda" : "cpu") << '\n';
  return true;
#else
  return false;
#endif
}

std::vector<int32_t> InferEngine::tokenize(const std::string& text) {
#if defined(EDGE_USE_LLAMA)
  llama_model* model = static_cast<llama_model*>(model_);
  const llama_vocab* vocab = llama_model_get_vocab(model);
  int n = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), nullptr, 0, true, true);
  if (n >= 0) {
    return {};
  }
  n = -n;
  std::vector<int32_t> tokens(static_cast<std::size_t>(n));
  const int written = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(), n,
                                     true, true);
  if (written <= 0) {
    return {};
  }
  tokens.resize(static_cast<std::size_t>(written));
  return tokens;
#else
  return {};
#endif
}

void InferEngine::runDecodeLoop(const std::function<void(const std::string&)>& onToken) {
#if defined(EDGE_USE_LLAMA)
  llama_model* model = static_cast<llama_model*>(model_);
  const llama_vocab* vocab = llama_model_get_vocab(model);
  llama_context* ctx = static_cast<llama_context*>(ctx_);

  const auto samplerParams = llama_sampler_chain_default_params();
  llama_sampler* sampler = llama_sampler_chain_init(samplerParams);
  llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
  llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.9f, 1));
  llama_sampler_chain_add(sampler, llama_sampler_init_temp(cfg_.temperature));
  llama_sampler_chain_add(sampler, llama_sampler_init_dist(cfg_.seed));

  int generated = 0;
  int attempts = 0;
  while (generated < cfg_.maxTokens && attempts < cfg_.maxTokens * 4) {
    ++attempts;
    llama_token token = llama_sampler_sample(sampler, ctx, -1);
    if (llama_vocab_is_eog(vocab, token)) {
      if (generated == 0) {
        continue;
      }
      break;
    }

    char piece[256];
    const int len = llama_token_to_piece(vocab, token, piece, static_cast<int32_t>(sizeof(piece)), 0, true);
    if (len > 0) {
      onToken(std::string(piece, static_cast<std::size_t>(len)));
    }

    llama_batch batch = llama_batch_get_one(&token, 1);
    if (llama_decode(ctx, batch) != 0) {
      break;
    }
    ++generated;
  }

  llama_sampler_free(sampler);
  llama_memory_clear(llama_get_memory(ctx), false);
#else
  (void)onToken;
#endif
}

std::string InferEngine::generate(const std::string& prompt) {
  clearFault();
  const DeviceFault injected = injectedFault();
  if (injected != DeviceFault::kNone) {
    recordFault(injected, "EDGE_SIMULATE_DEVICE_FAULT");
    return "[error: simulated device fault]";
  }
#if defined(EDGE_USE_LLAMA)
  llama_context* ctx = static_cast<llama_context*>(ctx_);
  auto tokens = tokenize(prompt);
  if (tokens.empty()) {
    return "[error: prompt tokenize failed]";
  }

  llama_batch batch = llama_batch_get_one(reinterpret_cast<llama_token*>(tokens.data()),
                                          static_cast<int32_t>(tokens.size()));
  const int rc = llama_decode(ctx, batch);
  if (rc != 0) {
    // llama has no device-fault signal; a failed decode is the closest thing to one.
    recordFault(DeviceFault::kRuntimeError, "llama_decode rc=" + std::to_string(rc));
    return "[error: prompt eval failed]";
  }

  std::string result;
  runDecodeLoop([&](const std::string& piece) {
    result += piece;
  });
  if (result.empty()) {
    recordFault(DeviceFault::kRuntimeError, "empty model output");
    return "[error: empty model output]";
  }
  return result;
#else
  if (prompt.empty()) {
    return "Empty prompt received.";
  }
  return "Inference response: " + prompt;
#endif
}

void InferEngine::generateStreaming(const std::string& prompt,
                                    const std::function<void(const std::string&)>& onToken) {
  clearFault();
  const DeviceFault injected = injectedFault();
  if (injected != DeviceFault::kNone) {
    recordFault(injected, "EDGE_SIMULATE_DEVICE_FAULT");
    return;
  }
#if defined(EDGE_USE_LLAMA)
  llama_context* ctx = static_cast<llama_context*>(ctx_);
  auto tokens = tokenize(prompt);
  if (tokens.empty()) {
    onToken("[error: prompt tokenize failed]");
    return;
  }

  llama_batch batch = llama_batch_get_one(reinterpret_cast<llama_token*>(tokens.data()),
                                          static_cast<int32_t>(tokens.size()));
  if (llama_decode(ctx, batch) != 0) {
    onToken("[error: prompt eval failed]");
    return;
  }

  runDecodeLoop(onToken);
#else
  onToken(generate(prompt));
#endif
}
