#pragma once

#include <string>
#include <vector>

enum class DeviceFault;

enum class ProbeResult {
  kAvailable,
  kWrongPlatform,
  kRuntimeMissing,   // right OS, the vendor runtime is not installed
  kDeviceMissing,    // the runtime is there, the hardware is not
  kPolicyDisabled,
};

const char* probeResultName(ProbeResult result);

struct DeviceBackend {
  const char* name;
  const char* platform;     // "linux", "windows", "macos", "any"
  const char* runtime;
  const char* note;
  ProbeResult (*probe)();
};

const DeviceBackend* findBackend(const std::string& name);

ProbeResult probeDevice(const std::string& name);

std::vector<std::string> knownBackends();

DeviceFault faultFromDxgiStatus(unsigned long status);
DeviceFault faultFromWindowsError(unsigned long code);
DeviceFault faultFromQnnStatus(long status);
DeviceFault faultFromCoreMediaStatus(long status);
DeviceFault faultFromCoreMlError(long code);

DeviceFault faultFromRemoteStatus(long status);

// CPU_AVAILABLE, GPU_UNAVAILABLE, NPU_UNHEALTHY, ANE_UNSUPPORTED and so on.
std::string backendAvailabilityState(const std::string& tier, ProbeResult probe,
                                     DeviceFault lastFault, bool quarantined, bool sessionFatal);
