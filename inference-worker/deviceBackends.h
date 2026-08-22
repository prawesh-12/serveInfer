#pragma once

#include <string>
#include <vector>

// What each tier is, and how to check whether it works.
//
// deviceLadder.cpp decides policy. Which tier to use, when to quarantine one,
// when it may come back. This file handles one tier at a time. It answers two
// questions. Is this accelerator here and working? And what does this vendor
// error code mean?
//
// Every backend below compiles on every platform. A tier that belongs to
// another OS returns kWrongPlatform. It is not missing. So a ladder copied from
// a Windows machine still runs here, and /health can say why each tier was
// skipped.

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

// Returns null when this build does not know that tier name.
const DeviceBackend* findBackend(const std::string& name);

ProbeResult probeDevice(const std::string& name);

// Every tier name this build understands. The order means nothing.
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
