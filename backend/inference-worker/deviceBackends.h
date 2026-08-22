#pragma once

#include <string>
#include <vector>

// Per-tier mechanism: is this accelerator here, and what does its error code
// mean. Policy lives in deviceLadder.
//
// Every backend compiles on every platform. A tier belonging to another OS
// returns kWrongPlatform rather than being absent, so a ladder copied from a
// Windows machine still runs here and /health can say why it was skipped.

enum class DeviceFault;  // deviceLadder.h

enum class ProbeResult {
  kAvailable,
  kWrongPlatform,    // this tier belongs to a different OS
  kRuntimeMissing,   // right OS, but the vendor runtime is not installed
  kDeviceMissing,    // the runtime is there, the hardware is not
  kPolicyDisabled,   // it works, but we were told not to use it
};

const char* probeResultName(ProbeResult result);

struct DeviceBackend {
  const char* name;
  const char* platform;     // "linux", "windows", "macos", "any"
  const char* runtime;      // the library or device node it needs
  const char* note;         // one line for a human reading /health
  ProbeResult (*probe)();
};

const DeviceBackend* findBackend(const std::string& name);

ProbeResult probeDevice(const std::string& name);

std::vector<std::string> knownBackends();

// Vendor error code translation.
//
// These compile on every platform on purpose. This mapping is the one part of
// A3 and A4 you can read and test without owning the hardware. Putting it
// behind a platform #if would make it untestable.
DeviceFault faultFromDxgiStatus(unsigned long status);
DeviceFault faultFromWindowsError(unsigned long code);
DeviceFault faultFromQnnStatus(long status);
DeviceFault faultFromCoreMediaStatus(long status);
DeviceFault faultFromCoreMlError(long code);
