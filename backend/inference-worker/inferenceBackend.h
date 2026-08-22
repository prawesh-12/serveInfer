#pragma once

#include <functional>
#include <string>

#include "deviceBackends.h"
#include "deviceLadder.h"
#include "gpuResourceOwner.h"

using TokenSink = std::function<void(const std::string&)>;

struct BackendExecution {
  std::string text;
  DeviceFault fault = DeviceFault::kNone;
  std::string detail;
  long vendorStatus = 0;

  bool ok() const {
    return fault == DeviceFault::kNone;
  }
};

class InferenceBackend {
 public:
  virtual ~InferenceBackend() = default;

  // Must match a tier name in EDGE_DEVICE_LADDER.
  virtual const std::string& name() const = 0;

  virtual ProbeResult available() = 0;

  // onToken may be empty, which means "do not stream".
  virtual BackendExecution execute(const std::string& prompt, const TokenSink& onToken) = 0;

  virtual bool healthCheck() = 0;

  virtual bool executable() const = 0;
};

class InferEngine;

class QualcommHexagonBackend : public InferenceBackend {
 public:
  explicit QualcommHexagonBackend(InferEngine* engine = nullptr);

  const std::string& name() const override {
    return name_;
  }
  ProbeResult available() override;
  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override;
  bool healthCheck() override;
  bool executable() const override;

  void bindEngine(InferEngine* engine) {
    engine_ = engine;
  }

  static bool hexagonCompiledIn();

  bool routable() const;
  // Only lifts the build gate; an unbound engine still refuses.
  void setRouteEnabledForTest(bool enabled) {
    routeOverrideForTest_ = enabled;
  }

 private:
  std::string name_ = "npu";
  InferEngine* engine_ = nullptr;
  bool routeOverrideForTest_ = false;
};

class CoreMlAneBackend : public InferenceBackend {
 public:
  CoreMlAneBackend();

  const std::string& name() const override {
    return name_;
  }
  ProbeResult available() override;
  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override;
  bool healthCheck() override;
  bool executable() const override {
    return false;
  }

 private:
  std::string name_ = "ane";
};

struct RemoteRequest {
  std::string endpoint;
  std::string prompt;
};

struct RemoteResponse {
  // HTTP-style status, or 0 when the call never reached a server at all.
  long status = 0;
  std::string text;
  std::string error;
};

using RemoteTransport = std::function<RemoteResponse(const RemoteRequest&)>;

RemoteTransport makeRemoteTransport();

class RemoteInferenceBackend : public InferenceBackend, public GpuResourceOwner {
 public:
  explicit RemoteInferenceBackend(RemoteTransport transport = {});
  ~RemoteInferenceBackend() override;

  const std::string& name() const override {
    return name_;
  }
  ProbeResult available() override;
  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override;
  bool healthCheck() override;
  bool executable() const override {
    return static_cast<bool>(transport_);
  }

  bool releaseDeviceResources() override;
  bool deviceResourcesResident() const override;

 private:
  std::string name_ = "remote";
  RemoteTransport transport_;
  bool sessionOpen_ = false;
};

class LlamaInferenceBackend : public InferenceBackend {
 public:
  LlamaInferenceBackend(std::string tier, InferEngine* engine);

  const std::string& name() const override {
    return name_;
  }
  ProbeResult available() override;
  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override;
  bool healthCheck() override;
  bool executable() const override {
    return true;
  }

 private:
  std::string name_;
  InferEngine* engine_;
};

// "npu" is absent on purpose: forceCpu = (tier != "cuda") would run it on the CPU.
bool tierIsLlamaServed(const std::string& tier);

// In no Apple SDK header; the value is CoreMedia's kCMBaseObjectError_UnsupportedOperation.
constexpr long kCMErrorUnsupportedOperation = -12782;
