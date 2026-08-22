#include "deviceLadder.h"

#include "deviceBackends.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace {

// deviceBackends.cpp knows how to probe each tier. This file only decides
// policy. Which tier to use, when to quarantine one, and when it may come back.
bool probeTier(const std::string& name) {
  return probeDevice(name) == ProbeResult::kAvailable;
}

std::string trim(const std::string& in) {
  const auto begin = in.find_first_not_of(" \t");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = in.find_last_not_of(" \t");
  return in.substr(begin, end - begin + 1);
}

}  // namespace

const char* deviceFaultName(DeviceFault fault) {
  switch (fault) {
    case DeviceFault::kNone:
      return "none";
    case DeviceFault::kUnavailable:
      return "device_unavailable";
    case DeviceFault::kRemoved:
      return "device_removed";
    case DeviceFault::kUnsupportedOp:
      return "unsupported_operation";
    case DeviceFault::kRuntimeError:
      return "runtime_error";
  }
  return "unknown";
}

std::vector<std::string> parseDeviceLadder(const std::string& csv) {
  std::vector<std::string> out;
  std::stringstream stream(csv);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const std::string name = trim(item);
    if (!name.empty()) {
      out.push_back(name);
    }
  }
  if (out.empty()) {
    out.push_back("cpu");
  }
  return out;
}

DeviceLadder::DeviceLadder(const std::vector<std::string>& order, int quarantineMs,
                           int probeIntervalMs)
    : quarantine_(std::max(0, quarantineMs)), probeInterval_(std::max(0, probeIntervalMs)) {
  tiers_.reserve(order.size());
  for (const std::string& name : order) {
    DeviceTier tier;
    tier.name = name;
    tiers_.push_back(tier);
  }
}

bool DeviceLadder::usable(const DeviceTier& tier) const {
  if (tier.sessionFatal) {
    return false;
  }
  if (std::chrono::steady_clock::now() < tier.quarantinedUntil) {
    return false;
  }
  return true;
}

bool DeviceLadder::healthCheck(const std::string& tierName) const {
  return probeTier(tierName);
}

// A probe interval of 0 turns the cache off. Tests use that to stay predictable.
ProbeResult DeviceLadder::probeTierCached(std::size_t index) const {
  DeviceTier& tier = tiers_[index];
  const auto now = std::chrono::steady_clock::now();
  if (probeInterval_.count() > 0 && tier.probeCacheValid &&
      now - tier.probeCachedAt < probeInterval_) {
    return tier.cachedProbe;
  }
  tier.cachedProbe = probeDevice(tier.name);
  tier.probeCachedAt = now;
  tier.probeCacheValid = true;
  return tier.cachedProbe;
}

bool DeviceLadder::select() {
  for (std::size_t i = 0; i < tiers_.size(); ++i) {
    DeviceTier& tier = tiers_[i];
    if (!usable(tier)) {
      continue;
    }
    // The quarantine running out is not enough on its own. The health check has
    // to pass too. That is the assignment's "block retry until a driver health
    // check passes".
    if (probeTierCached(i) != ProbeResult::kAvailable) {
      continue;
    }
    activeIndex_ = i;
    active_ = tier.name;
    if (!baselineSet_) {
      // The first tier we pick is what healthy looks like on this machine.
      baselineIndex_ = i;
      baselineSet_ = true;
    }
    if (activeIndex_ <= baselineIndex_) {
      degradedReason_.clear();
    }
    return true;
  }
  active_.clear();
  return false;
}

bool DeviceLadder::reportFault(DeviceFault fault, const std::string& detail) {
  if (active_.empty() || activeIndex_ >= tiers_.size()) {
    return false;
  }

  DeviceTier& tier = tiers_[activeIndex_];
  tier.probeCacheValid = false;  // a fault is fresher news than a cached probe
  tier.consecutiveFaults += 1;
  tier.lastFaultDetail = detail;
  tier.quarantinedUntil = std::chrono::steady_clock::now() + quarantine_;
  if (fault == DeviceFault::kRemoved) {
    tier.sessionFatal = true;
  }

  const std::string faulted = tier.name;
  std::cerr << "[device-ladder] " << faulted << " faulted (" << deviceFaultName(fault) << "): "
            << detail << (tier.sessionFatal ? " [unrecoverable for this session]" : "") << '\n';

  const std::size_t previousIndex = activeIndex_;
  const std::string previousActive = active_;
  if (!select() || activeIndex_ <= previousIndex) {
    // There is nowhere to fall. Stay on the current tier. If we left the ladder
    // with no active tier, the worker would report an empty device name.
    activeIndex_ = previousIndex;
    active_ = previousActive;
    std::cerr << "[device-ladder] no lower tier available after " << faulted << " faulted\n";
    return false;
  }

  degradedReason_ = faulted + ":" + deviceFaultName(fault);
  std::cerr << "[device-ladder] fell back to " << active_ << ", latency mode degraded\n";
  return true;
}

std::string DeviceLadder::toJson() const {
  const auto now = std::chrono::steady_clock::now();
  std::string out = "{\"active\":\"" + active_ + "\",\"degraded\":" +
                    (degraded() ? "true" : "false") + ",\"reason\":\"" + degradedReason_ +
                    "\",\"baseline\":\"" +
                    (baselineSet_ ? tiers_[baselineIndex_].name : std::string{}) + "\",\"tiers\":[";
  for (std::size_t i = 0; i < tiers_.size(); ++i) {
    const DeviceTier& tier = tiers_[i];
    long long quarantineLeftMs = 0;
    if (now < tier.quarantinedUntil) {
      quarantineLeftMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(tier.quarantinedUntil - now).count();
    }
    if (i > 0) {
      out += ",";
    }
    const ProbeResult probe = probeTierCached(i);
    const DeviceBackend* backend = findBackend(tier.name);
    out += "{\"name\":\"" + tier.name + "\",\"faults\":" + std::to_string(tier.consecutiveFaults) +
           ",\"sessionFatal\":" + (tier.sessionFatal ? "true" : "false") +
           ",\"quarantineMsLeft\":" + std::to_string(quarantineLeftMs) +
           ",\"healthy\":" + (probe == ProbeResult::kAvailable ? "true" : "false") +
           ",\"probe\":\"" + probeResultName(probe) + "\",\"platform\":\"" +
           (backend != nullptr ? backend->platform : "unknown") + "\"}";
  }
  out += "]}";
  return out;
}
