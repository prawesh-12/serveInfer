#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "deviceLadder.h"
#include "inferenceBackend.h"

struct RouteResult {
  std::string text;
  std::string device;
  bool degraded = false;
  std::string degradedReason;
  DeviceFault fault = DeviceFault::kNone;
  std::string detail;

  bool ok() const {
    return fault == DeviceFault::kNone;
  }
};

class BackendRouter {
 public:
  BackendRouter(const std::vector<std::string>& order, int quarantineMs, int probeIntervalMs);

  void registerBackend(std::shared_ptr<InferenceBackend> backend);

  bool select();
  void beginSession();

  bool attemptRecovery();

  const std::string& active() const {
    return ladder_.active();
  }
  DeviceLadder& ladder() {
    return ladder_;
  }
  const DeviceLadder& ladder() const {
    return ladder_;
  }

  // Returning false aborts routing with the fault that caused the move.
  using FallbackHook = std::function<bool(const std::string& previous, const std::string& next)>;
  void setFallbackHook(FallbackHook hook);

  RouteResult route(const std::string& prompt, const TokenSink& onToken);

 private:
  InferenceBackend* backendFor(const std::string& tier) const;
  ProbeResult probeTier(const std::string& tier);

  DeviceLadder ladder_;
  std::map<std::string, std::shared_ptr<InferenceBackend>> backends_;
  FallbackHook onFallback_;
};
