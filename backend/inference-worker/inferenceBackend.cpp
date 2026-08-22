#include "inferenceBackend.h"

#include "inferEngine.h"

#include <cstdlib>
#include <exception>
#include <utility>

namespace {

BackendExecution notCompiledIn(const std::string& detail) {
  BackendExecution result;
  result.fault = DeviceFault::kUnavailable;
  result.detail = detail;
  return result;
}

BackendExecution runThroughEngine(InferEngine* engine, const std::string& tier,
                                  const std::string& prompt, const TokenSink& onToken) {
  BackendExecution result;
  if (engine == nullptr) {
    result.fault = DeviceFault::kUnavailable;
    result.detail = "inference engine is not initialized";
    return result;
  }

  engine->setExecutingTier(tier);

  if (onToken) {
    engine->generateStreaming(prompt, [&](const std::string& token) {
      result.text += token;
      onToken(token);
    });
  } else {
    result.text = engine->generate(prompt);
  }

  result.fault = engine->lastFault();
  result.detail = engine->lastFaultDetail();
  return result;
}

}  // namespace

bool tierIsLlamaServed(const std::string& tier) {
  return tier == "cpu" || tier == "cuda" || tier == "rocm" || tier == "vulkan" ||
         tier == "metal" || tier == "accelerate";
}

QualcommHexagonBackend::QualcommHexagonBackend(InferEngine* engine) : engine_(engine) {}

// GGML_USE_HEXAGON is ggml's own macro, and is never defined on this platform.
bool QualcommHexagonBackend::hexagonCompiledIn() {
#if defined(GGML_USE_HEXAGON)
  return true;
#else
  return false;
#endif
}

bool QualcommHexagonBackend::routable() const {
  return hexagonCompiledIn() || routeOverrideForTest_;
}

bool QualcommHexagonBackend::executable() const {
  return routable() && engine_ != nullptr;
}

ProbeResult QualcommHexagonBackend::available() {
  return probeDevice(name_);
}

BackendExecution QualcommHexagonBackend::execute(const std::string& prompt,
                                                 const TokenSink& onToken) {
  if (!routable()) {
    return notCompiledIn(
        "qualcomm hexagon backend is not compiled into this build (GGML_HEXAGON=OFF, needs "
        "HEXAGON_SDK_ROOT and a windows arm64 target)");
  }
  if (engine_ == nullptr) {
    return notCompiledIn(
        "qualcomm hexagon backend is compiled in (GGML_USE_HEXAGON) but no inference engine is "
        "bound to the npu adapter");
  }
  return runThroughEngine(engine_, name_, prompt, onToken);
}

bool QualcommHexagonBackend::healthCheck() {
  return available() == ProbeResult::kAvailable;
}

CoreMlAneBackend::CoreMlAneBackend() = default;

ProbeResult CoreMlAneBackend::available() {
  return probeDevice(name_);
}

BackendExecution CoreMlAneBackend::execute(const std::string& prompt, const TokenSink& onToken) {
  (void)prompt;
  (void)onToken;
  return notCompiledIn(
      "core ml / ane adapter has no compiled model in this build (needs an MLModel and "
      "MLComputeUnits.cpuAndNeuralEngine on macOS)");
}

bool CoreMlAneBackend::healthCheck() {
  return available() == ProbeResult::kAvailable;
}

namespace {

std::string remoteEndpoint() {
  const char* endpoint = std::getenv("EDGE_REMOTE_ENDPOINT");
  return endpoint == nullptr ? std::string{} : std::string(endpoint);
}

}  // namespace

RemoteInferenceBackend::RemoteInferenceBackend(RemoteTransport transport)
    : transport_(std::move(transport)) {}

RemoteInferenceBackend::~RemoteInferenceBackend() {
  releaseDeviceResources();
}

ProbeResult RemoteInferenceBackend::available() {
  const ProbeResult configured = probeDevice(name_);
  if (configured != ProbeResult::kAvailable) {
    return configured;
  }
  if (!transport_) {
    return ProbeResult::kRuntimeMissing;
  }
  return ProbeResult::kAvailable;
}

BackendExecution RemoteInferenceBackend::execute(const std::string& prompt,
                                                 const TokenSink& onToken) {
  BackendExecution result;

  // Re-read per call, not cached: clearing EDGE_REMOTE_FALLBACK_ALLOWED must take effect now.
  const ProbeResult policy = probeDevice(name_);
  if (policy == ProbeResult::kPolicyDisabled) {
    result.fault = DeviceFault::kUnavailable;
    result.detail =
        "remote fallback is not opted in (EDGE_REMOTE_FALLBACK_ALLOWED is not 1), so the prompt "
        "stays on this device";
    return result;
  }
  if (policy != ProbeResult::kAvailable) {
    result.fault = DeviceFault::kUnavailable;
    result.detail =
        "remote is not configured (neither EDGE_SARVAM_API_KEY nor EDGE_REMOTE_ENDPOINT is set)";
    return result;
  }
  if (!transport_) {
    return notCompiledIn(
        "no remote transport was built for this worker (EDGE_SARVAM_API_KEY was empty at "
        "startup); the tier is configured but nothing can serve it");
  }

  RemoteRequest request;
  request.endpoint = remoteEndpoint();
  request.prompt = prompt;

  RemoteResponse response;
  sessionOpen_ = true;
  try {
    response = transport_(request);
  } catch (const std::exception& err) {
    releaseDeviceResources();
    result.fault = DeviceFault::kRuntimeError;
    result.detail = std::string("remote transport threw: ") + err.what();
    return result;
  }
  releaseDeviceResources();

  result.vendorStatus = response.status;
  const DeviceFault fault = faultFromRemoteStatus(response.status);
  if (fault != DeviceFault::kNone) {
    result.fault = fault;
    result.detail = response.error.empty()
                        ? "remote endpoint returned status " + std::to_string(response.status)
                        : response.error;
    return result;
  }

  result.text = response.text;
  if (onToken && !result.text.empty()) {
    onToken(result.text);
  }
  return result;
}

bool RemoteInferenceBackend::healthCheck() {
  return available() == ProbeResult::kAvailable;
}

bool RemoteInferenceBackend::releaseDeviceResources() {
  sessionOpen_ = false;
  return true;
}

bool RemoteInferenceBackend::deviceResourcesResident() const {
  return sessionOpen_;
}

LlamaInferenceBackend::LlamaInferenceBackend(std::string tier, InferEngine* engine)
    : name_(std::move(tier)), engine_(engine) {}

ProbeResult LlamaInferenceBackend::available() {
  return probeDevice(name_);
}

BackendExecution LlamaInferenceBackend::execute(const std::string& prompt,
                                                const TokenSink& onToken) {
  return runThroughEngine(engine_, name_, prompt, onToken);
}

bool LlamaInferenceBackend::healthCheck() {
  return available() == ProbeResult::kAvailable;
}
