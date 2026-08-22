#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "deviceBackends.h"

// Tier policy: on a device fault, drop to the next tier and keep the broken one
// out until a health check passes.
//
// Only the probe and the vendor error code change per platform, so those live in
// deviceBackends. EDGE_DEVICE_LADDER names the tiers, best first, which is why a
// Windows "npu,gpu,cpu" or macOS "ane,metal,accelerate" needs no change here.

enum class DeviceFault {
  kNone,
  kUnavailable,    // never came up: no driver, no device node
  kRemoved,        // vanished mid-session. Windows ERROR_DEVICE_REMOVED.
  kUnsupportedOp,  // backend cannot run this op. macOS kCMErrorUnsupportedOperation.
  kRuntimeError,   // anything else the backend reported
};

const char* deviceFaultName(DeviceFault fault);

struct DeviceTier {
  std::string name;
  int consecutiveFaults = 0;
  // ERROR_DEVICE_REMOVED is unrecoverable for the session, so the quarantine
  // window does not apply.
  bool sessionFatal = false;
  std::string lastFaultDetail;
  std::chrono::steady_clock::time_point quarantinedUntil{};
  // Each probe costs a syscall. /health probes every tier and is polled every
  // two seconds. So a result is reused for EDGE_DEVICE_PROBE_INTERVAL_MS.
  std::chrono::steady_clock::time_point probeCachedAt{};
  bool probeCacheValid = false;
  ProbeResult cachedProbe{};
};

class DeviceLadder {
 public:
  DeviceLadder(const std::vector<std::string>& order, int quarantineMs, int probeIntervalMs);

  // Picks the best tier that is not quarantined and passes its probe.
  // Returns false when no tier is usable. That is fatal for the worker.
  bool select();

  const std::string& active() const { return active_; }
  std::size_t activeIndex() const { return activeIndex_; }

  // Degraded means we are running below the best tier this machine had at
  // startup.
  //
  // A machine with no NPU is not degraded. It never had one. A machine that had
  // an NPU and then lost it is degraded.
  //
  // The startup baseline is what makes that difference. Without it, every
  // CPU-only machine would report degraded forever, and the flag would tell you
  // nothing at all.
  bool degraded() const { return activeIndex_ > baselineIndex_; }
  std::size_t baselineIndex() const { return baselineIndex_; }
  const std::string& degradedReason() const { return degradedReason_; }

  // Records a fault against the current tier and quarantines it.
  // Returns true when a lower tier took over. Returns false when there was none.
  bool reportFault(DeviceFault fault, const std::string& detail);

  // The check that decides whether a quarantined tier may be used again.
  bool healthCheck(const std::string& tierName) const;

  // The same check, cached for the probe interval. Keeps the reason for /health.
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
};

std::vector<std::string> parseDeviceLadder(const std::string& csv);
