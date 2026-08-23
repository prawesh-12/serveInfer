#include "backendRouter.h"

#include <iostream>
#include <utility>

BackendRouter::BackendRouter(const std::vector<std::string>& order, int quarantineMs,
                             int probeIntervalMs)
    : ladder_(order, quarantineMs, probeIntervalMs) {
  ladder_.setProbe([this](const std::string& tier) { return probeTier(tier); });
}

void BackendRouter::registerBackend(std::shared_ptr<InferenceBackend> backend) {
  if (!backend) {
    return;
  }
  const std::string name = backend->name();
  backends_[name] = std::move(backend);
  ladder_.invalidateProbeCache();
}

ProbeResult BackendRouter::probeTier(const std::string& tier) {
  InferenceBackend* backend = backendFor(tier);
  if (backend == nullptr) {
    return probeDevice(tier);
  }
  return backend->available();
}

InferenceBackend* BackendRouter::backendFor(const std::string& tier) const {
  const auto it = backends_.find(tier);
  return it == backends_.end() ? nullptr : it->second.get();
}

bool BackendRouter::select() {
  if (!ladder_.select()) {
    std::cerr << "[backend-router] no usable tier: " << ladder_.toJson() << '\n';
    return false;
  }
  std::cerr << "[backend-router] selected tier=" << ladder_.active()
            << " state=" << ladder_.tierState(ladder_.activeIndex()) << '\n';
  return true;
}

void BackendRouter::setFallbackHook(FallbackHook hook) {
  onFallback_ = std::move(hook);
}

void BackendRouter::beginSession() {
  ladder_.beginSession();
}

bool BackendRouter::attemptRecovery() {
  const std::string previous = ladder_.active();
  if (!ladder_.recoverEligibleTier()) {
    return false;
  }

  const std::string next = ladder_.active();
  std::cerr << "[backend-router] recovered previous=" << previous << " next=" << next
            << " state=" << ladder_.tierState(ladder_.activeIndex()) << '\n';

  if (onFallback_ && !onFallback_(previous, next)) {
    std::cerr << "[backend-router] recovery to " << next
              << " could not be brought up, returning to the fallback tier\n";
    if (ladder_.reportFault(DeviceFault::kRuntimeError,
                            "engine could not be brought up on recovered tier " + next) &&
        onFallback_) {
      onFallback_(next, ladder_.active());
    }
    return false;
  }
  return true;
}

RouteResult BackendRouter::route(const std::string& prompt, const TokenSink& onToken) {
  RouteResult result;
  if (ladder_.active().empty() && !select()) {
    result.fault = DeviceFault::kUnavailable;
    result.detail = "no usable tier in the ladder";
    return result;
  }

  attemptRecovery();

  // Bounded: every iteration returns or quarantines the tier it just tried.
  const std::size_t attempts = ladder_.tierCount() + 1;
  for (std::size_t i = 0; i < attempts; ++i) {
    const std::string tier = ladder_.active();
    InferenceBackend* backend = backendFor(tier);

    BackendExecution execution;
    if (backend == nullptr) {
      execution.fault = DeviceFault::kUnavailable;
      execution.detail = "no backend adapter registered for tier " + tier;
    } else if (!backend->executable()) {
      execution = backend->execute(prompt, onToken);
      if (execution.ok()) {
        execution.fault = DeviceFault::kUnavailable;
        execution.detail = "adapter for " + tier + " reported success but cannot execute";
      }
    } else {
      execution = backend->execute(prompt, onToken);
    }

    if (execution.ok()) {
      result.text = std::move(execution.text);
      result.device = tier;
      result.degraded = ladder_.degraded();
      result.degradedReason = ladder_.degradedReason();
      return result;
    }

    std::cerr << "[backend-router] tier=" << tier << " fault=" << deviceFaultName(execution.fault)
              << " vendorStatus=" << execution.vendorStatus << " detail=\"" << execution.detail
              << "\"\n";

    if (!ladder_.reportFault(execution.fault, execution.detail)) {
      // Partial text still goes back, or the final frame contradicts sent tokens.
      result.text = std::move(execution.text);
      result.device = tier;
      result.fault = execution.fault;
      result.detail = execution.detail;
      result.degraded = ladder_.degraded();
      result.degradedReason = ladder_.degradedReason();
      return result;
    }

    std::cerr << "[backend-router] fallback previous=" << tier
              << " next=" << ladder_.active()
              << " state=" << ladder_.tierState(ladder_.activeIndex()) << '\n';

    if (onFallback_ && !onFallback_(tier, ladder_.active())) {
      result.text = std::move(execution.text);
      result.device = ladder_.active();
      result.fault = execution.fault;
      result.detail = execution.detail;
      result.degraded = ladder_.degraded();
      result.degradedReason = ladder_.degradedReason();
      return result;
    }
  }

  result.fault = DeviceFault::kRuntimeError;
  result.detail = "ladder did not settle on a usable tier";
  result.device = ladder_.active();
  return result;
}
