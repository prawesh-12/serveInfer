#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "deviceBackends.h"

enum class DeviceFault {
  kNone,
  kUnavailable,
  kRemoved,        // Windows ERROR_DEVICE_REMOVED; session-fatal, no quarantine
  kUnsupportedOp,  // macOS kCMErrorUnsupportedOperation: this op, not this device
  kRuntimeError,
};

const char* deviceFaultName(DeviceFault fault);

struct DeviceTier {
  std::string name;
  int consecutiveFaults = 0;
  DeviceFault lastFault = DeviceFault::kNone;
  bool sessionFatal = false;
  std::string lastFaultDetail;
  std::chrono::steady_clock::time_point quarantinedUntil{};
  std::chrono::steady_clock::time_point probeCachedAt{};
  bool probeCacheValid = false;
  ProbeResult cachedProbe{};
};

using TierProbe = std::function<ProbeResult(const std::string&)>;

class DeviceLadder {
 public:
  DeviceLadder(const std::vector<std::string>& order, int quarantineMs, int probeIntervalMs);

  void setProbe(TierProbe probe);
  void invalidateProbeCache();

  std::size_t tierCount() const { return tiers_.size(); }

  bool select();

  const std::string& active() const { return active_; }
  std::size_t activeIndex() const { return activeIndex_; }

  bool degraded() const { return activeIndex_ > baselineIndex_; }
  std::size_t baselineIndex() const { return baselineIndex_; }
  const std::string& degradedReason() const { return degradedReason_; }

  // A session is the worker process: a removed device needs a respawn to return.
  void beginSession();

  // Only ever moves up the ladder; reportFault owns the other direction.
  bool recoverEligibleTier();

  std::string tierState(std::size_t index) const;

  DeviceFault lastFault(std::size_t index) const;

  // Returns tierCount() when the name is not on the ladder.
  std::size_t indexOf(const std::string& name) const;

  // Returns true when a lower tier took over.
  bool reportFault(DeviceFault fault, const std::string& detail);

  bool healthCheck(const std::string& tierName) const;

  ProbeResult probeTierCached(std::size_t index) const;

  std::string toJson() const;

 private:
  bool usable(const DeviceTier& tier) const;

  mutable std::vector<DeviceTier> tiers_;
  std::size_t activeIndex_ = 0;
  std::size_t baselineIndex_ = 0;
  bool baselineSet_ = false;
  std::string active_;
  std::string degradedReason_;
  std::chrono::milliseconds quarantine_;
  std::chrono::milliseconds probeInterval_;
  TierProbe probe_;
};

std::vector<std::string> parseDeviceLadder(const std::string& csv);
