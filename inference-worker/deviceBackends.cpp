#include "deviceBackends.h"

#include "deviceLadder.h"

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

bool pathReadable(const char* path) {
#if defined(_WIN32)
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
  return access(path, R_OK) == 0;
#endif
}

bool anyPathReadable(const char* const* paths, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (pathReadable(paths[i])) {
      return true;
    }
  }
  return false;
}

#if defined(_WIN32)
// A tier is not usable just because a file is on disk. The DLL has to load.
// A half-installed vendor runtime is what turns into a device fault on the
// first inference, instead of a clean refusal at startup.
bool libraryLoads(const char* name) {
  HMODULE handle = LoadLibraryA(name);
  if (handle == nullptr) {
    return false;
  }
  FreeLibrary(handle);
  return true;
}
#endif

// ---------------------------------------------------------------- CPU

ProbeResult probeCpu() {
  return ProbeResult::kAvailable;
}

// ---------------------------------------------------------------- Linux

ProbeResult probeCuda() {
#if defined(__linux__)
  static const char* nodes[] = {"/dev/nvidiactl", "/dev/nvidia0"};
  if (!anyPathReadable(nodes, 2)) {
    return ProbeResult::kDeviceMissing;
  }
  return ProbeResult::kAvailable;
#elif defined(_WIN32)
  return libraryLoads("nvcuda.dll") ? ProbeResult::kAvailable : ProbeResult::kRuntimeMissing;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

ProbeResult probeRocm() {
#if defined(__linux__)
  static const char* nodes[] = {"/dev/kfd"};
  return anyPathReadable(nodes, 1) ? ProbeResult::kAvailable : ProbeResult::kDeviceMissing;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

ProbeResult probeVulkan() {
#if defined(__linux__)
  static const char* nodes[] = {"/dev/dri/renderD128", "/dev/dri/card0"};
  if (!anyPathReadable(nodes, 2)) {
    return ProbeResult::kDeviceMissing;
  }
  static const char* libs[] = {"/usr/lib/x86_64-linux-gnu/libvulkan.so.1",
                               "/usr/lib64/libvulkan.so.1", "/usr/lib/libvulkan.so.1"};
  return anyPathReadable(libs, 3) ? ProbeResult::kAvailable : ProbeResult::kRuntimeMissing;
#elif defined(_WIN32)
  return libraryLoads("vulkan-1.dll") ? ProbeResult::kAvailable : ProbeResult::kRuntimeMissing;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

// ------------------------------------------------- Windows on ARM64 (A3)

// Qualcomm Hexagon NPU, reached through the QNN SDK.
//
// Two things have to be true. The HTP backend library has to load, and DXCore
// has to find a compute adapter. Checking only the library is the mistake that
// makes ERROR_DEVICE_REMOVED look like a surprise mid-inference, when really
// the device was never there.
ProbeResult probeNpu() {
#if defined(_WIN32)
  if (!libraryLoads("QnnHtp.dll")) {
    return ProbeResult::kRuntimeMissing;
  }
  if (!libraryLoads("dxcore.dll")) {
    return ProbeResult::kRuntimeMissing;
  }
  // A full check would call DXCoreCreateAdapterFactory and ask for
  // DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_ML. Loading dxcore is as far as we
  // can go without linking the D3D12 headers into this worker.
  return ProbeResult::kAvailable;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

// DirectML is the middle tier on Windows. Slower than the NPU, still on-device.
ProbeResult probeDirectMl() {
#if defined(_WIN32)
  if (!libraryLoads("DirectML.dll")) {
    return ProbeResult::kRuntimeMissing;
  }
  return libraryLoads("d3d12.dll") ? ProbeResult::kAvailable : ProbeResult::kRuntimeMissing;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

// ------------------------------------------------------------ macOS (A4)

ProbeResult probeAne() {
#if defined(__APPLE__)
  // The ANE is reached through Core ML, and only Apple silicon has one. An
  // Intel Mac has the framework but no Neural Engine behind it. So we check for
  // both the framework and the hardware service.
  static const char* framework[] = {"/System/Library/Frameworks/CoreML.framework/CoreML"};
  if (!anyPathReadable(framework, 1)) {
    return ProbeResult::kRuntimeMissing;
  }
  static const char* ane[] = {"/System/Library/PrivateFrameworks/ANECompiler.framework",
                              "/usr/lib/libane.dylib"};
  return anyPathReadable(ane, 2) ? ProbeResult::kAvailable : ProbeResult::kDeviceMissing;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

ProbeResult probeMetal() {
#if defined(__APPLE__)
  static const char* framework[] = {"/System/Library/Frameworks/Metal.framework/Metal"};
  return anyPathReadable(framework, 1) ? ProbeResult::kAvailable : ProbeResult::kRuntimeMissing;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

ProbeResult probeAccelerate() {
#if defined(__APPLE__)
  static const char* framework[] = {"/System/Library/Frameworks/Accelerate.framework/Accelerate"};
  return anyPathReadable(framework, 1) ? ProbeResult::kAvailable : ProbeResult::kRuntimeMissing;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

// ----------------------------------------------------------------- Remote

// The remote tier is last on every ladder, and off unless you turn it on.
//
// Falling back to a cloud API means the user's transcript leaves the device.
// That is a policy decision. An on-device runtime should not make it on its own
// just because a driver hiccuped. So this needs an endpoint and an explicit
// opt-in, not one or the other.
ProbeResult probeRemote() {
  const char* endpoint = std::getenv("EDGE_REMOTE_ENDPOINT");
  if (endpoint == nullptr || endpoint[0] == '\0') {
    return ProbeResult::kRuntimeMissing;
  }
  const char* allow = std::getenv("EDGE_REMOTE_FALLBACK_ALLOWED");
  if (allow == nullptr || std::strcmp(allow, "1") != 0) {
    return ProbeResult::kPolicyDisabled;
  }
  return ProbeResult::kAvailable;
}

const DeviceBackend kBackends[] = {
    {"cpu", "any", "none", "always present, the floor of every ladder", probeCpu},
    {"cuda", "linux", "/dev/nvidia*, nvcuda.dll", "NVIDIA GPU", probeCuda},
    {"rocm", "linux", "/dev/kfd", "AMD GPU", probeRocm},
    {"vulkan", "any", "libvulkan", "portable GPU compute", probeVulkan},
    {"npu", "windows", "QnnHtp.dll, dxcore.dll", "Qualcomm Hexagon NPU (A3)", probeNpu},
    {"directml", "windows", "DirectML.dll, d3d12.dll", "Windows GPU tier below the NPU", probeDirectMl},
    {"gpu", "windows", "DirectML.dll, d3d12.dll", "alias for directml", probeDirectMl},
    {"ane", "macos", "CoreML.framework", "Apple Neural Engine (A4)", probeAne},
    {"metal", "macos", "Metal.framework", "Apple GPU tier", probeMetal},
    {"accelerate", "macos", "Accelerate.framework", "Apple CPU tier", probeAccelerate},
    {"remote", "any", "EDGE_REMOTE_ENDPOINT", "cloud API, opt-in only", probeRemote},
};

constexpr std::size_t kBackendCount = sizeof(kBackends) / sizeof(kBackends[0]);

}  // namespace

const char* probeResultName(ProbeResult result) {
  switch (result) {
    case ProbeResult::kAvailable:
      return "available";
    case ProbeResult::kWrongPlatform:
      return "wrong_platform";
    case ProbeResult::kRuntimeMissing:
      return "runtime_missing";
    case ProbeResult::kDeviceMissing:
      return "device_missing";
    case ProbeResult::kPolicyDisabled:
      return "policy_disabled";
  }
  return "unknown";
}

const DeviceBackend* findBackend(const std::string& name) {
  for (std::size_t i = 0; i < kBackendCount; ++i) {
    if (name == kBackends[i].name) {
      return &kBackends[i];
    }
  }
  return nullptr;
}

ProbeResult probeDevice(const std::string& name) {
  const DeviceBackend* backend = findBackend(name);
  if (backend == nullptr) {
    return ProbeResult::kRuntimeMissing;
  }
  return backend->probe();
}

std::vector<std::string> knownBackends() {
  std::vector<std::string> out;
  out.reserve(kBackendCount);
  for (std::size_t i = 0; i < kBackendCount; ++i) {
    out.emplace_back(kBackends[i].name);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Vendor error code translation.
//
// You can read and test all of this on any machine, so none of it sits behind a
// platform #if. Give it the code the vendor API returned. It answers the one
// question the ladder cares about. Is this tier gone for the session? Is it
// only unable to run this one graph? Or did something transient go wrong?
//
// A note on accuracy. The DXGI and Win32 constants below are stable documented
// values, so they are written out here. The QNN and Core ML numbers are not.
// Guessing an SDK constant is worse than leaving it out. Those two functions
// dispatch on values that a Windows ARM64 or macOS build must take from the
// real vendor header. The mapping itself is the design decision, and it is
// complete and tested.
// ---------------------------------------------------------------------------

namespace {
constexpr unsigned long kDxgiDeviceRemoved = 0x887A0005ul;
constexpr unsigned long kDxgiDeviceHung = 0x887A0006ul;
constexpr unsigned long kDxgiDeviceReset = 0x887A0007ul;
constexpr unsigned long kDxgiDriverInternalError = 0x887A0020ul;
constexpr unsigned long kDxgiNotCurrentlyAvailable = 0x887A0022ul;
constexpr unsigned long kDxgiUnsupported = 0x887A0004ul;

constexpr unsigned long kWinErrorNotSupported = 50ul;
constexpr unsigned long kWinErrorGenFailure = 31ul;
constexpr unsigned long kWinErrorDeviceNotConnected = 1167ul;
constexpr unsigned long kWinErrorDeviceRemoved = 1617ul;
}  // namespace

DeviceFault faultFromDxgiStatus(unsigned long status) {
  switch (status) {
    case 0ul:
      return DeviceFault::kNone;
    // You cannot trust a removed or hung adapter again in this process. The old
    // device object stays invalid even after a driver reset. That is why the
    // assignment calls this unrecoverable for the session.
    case kDxgiDeviceRemoved:
    case kDxgiDeviceHung:
    case kDxgiDeviceReset:
      return DeviceFault::kRemoved;
    case kDxgiUnsupported:
      return DeviceFault::kUnsupportedOp;
    case kDxgiNotCurrentlyAvailable:
    case kDxgiDriverInternalError:
      return DeviceFault::kRuntimeError;
    default:
      return DeviceFault::kRuntimeError;
  }
}

DeviceFault faultFromWindowsError(unsigned long code) {
  switch (code) {
    case 0ul:
      return DeviceFault::kNone;
    case kWinErrorDeviceRemoved:
      return DeviceFault::kRemoved;
    case kWinErrorDeviceNotConnected:
      return DeviceFault::kUnavailable;
    case kWinErrorNotSupported:
      return DeviceFault::kUnsupportedOp;
    case kWinErrorGenFailure:
      return DeviceFault::kRuntimeError;
    default:
      // A caller may pass an HRESULT to the Win32 mapper by mistake. Send it to
      // the DXGI mapper instead of calling a removed device a generic error.
      if ((code & 0xFFFF0000ul) == 0x887A0000ul) {
        return faultFromDxgiStatus(code);
      }
      return DeviceFault::kRuntimeError;
  }
}

// Qualcomm QNN. `status` is a QnnStatus, and zero means success.
// The family numbers come from QnnCommon.h on the target SDK.
DeviceFault faultFromQnnStatus(long status) {
  if (status == 0) {
    return DeviceFault::kNone;
  }
  // QNN groups errors into families by their base number. Graph, backend,
  // device, memory. Only the device family means the accelerator is gone. The
  // rest are software failures, and a lower tier can retry them.
  const long family = status / 1000 * 1000;
  switch (family) {
    case 5000:  // QNN_DEVICE_ERROR family
      return DeviceFault::kRemoved;
    case 3000:  // QNN_GRAPH_ERROR family: this graph, not this device
      return DeviceFault::kUnsupportedOp;
    default:
      return DeviceFault::kRuntimeError;
  }
}

// CoreMedia OSStatus. This is the family kCMErrorUnsupportedOperation belongs
// to. Apple keeps them in the -12000 range. The whole family means "this
// operation, on this object". None of them means the hardware is gone.
DeviceFault faultFromCoreMediaStatus(long status) {
  if (status == 0) {
    return DeviceFault::kNone;
  }
  if (status <= -12000 && status > -13000) {
    return DeviceFault::kUnsupportedOp;
  }
  return DeviceFault::kRuntimeError;
}

// Core ML MLModelError. A model that will not compile for the ANE stays that
// way, but that says nothing about the device. So we quarantine the tier rather
// than kill it. A different model may run there fine.
DeviceFault faultFromCoreMlError(long code) {
  if (code == 0) {
    return DeviceFault::kNone;
  }
  switch (code) {
    case 3:   // MLModelErrorFeatureType
    case 6:   // MLModelErrorModelDecryptionKeyFetch
    case 9:   // MLModelErrorModelCollection
      return DeviceFault::kUnsupportedOp;
    default:
      return DeviceFault::kRuntimeError;
  }
}
