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
bool libraryLoads(const char* name) {
  HMODULE handle = LoadLibraryA(name);
  if (handle == nullptr) {
    return false;
  }
  FreeLibrary(handle);
  return true;
}
#endif

ProbeResult probeCpu() {
  return ProbeResult::kAvailable;
}

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

ProbeResult probeNpu() {
#if defined(_WIN32)
  if (!libraryLoads("QnnHtp.dll")) {
    return ProbeResult::kRuntimeMissing;
  }
  if (!libraryLoads("dxcore.dll")) {
    return ProbeResult::kRuntimeMissing;
  }
  // Optimistic: a full check needs DXCoreCreateAdapterFactory and the D3D12 headers.
  return ProbeResult::kAvailable;
#else
  return ProbeResult::kWrongPlatform;
#endif
}

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

ProbeResult probeAne() {
#if defined(__APPLE__)
  // An Intel Mac has Core ML but no Neural Engine, so the framework alone is not enough.
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

// Needs an endpoint AND an explicit opt-in: this one sends the transcript off the device.
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
    {"npu", "windows", "QnnHtp.dll, dxcore.dll", "Qualcomm Hexagon NPU", probeNpu},
    {"directml", "windows", "DirectML.dll, d3d12.dll", "Windows GPU tier below the NPU", probeDirectMl},
    {"gpu", "windows", "DirectML.dll, d3d12.dll", "alias for directml", probeDirectMl},
    {"ane", "macos", "CoreML.framework", "Apple Neural Engine", probeAne},
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
    // The old device object stays invalid even after a driver reset.
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
      // An HRESULT passed to the Win32 mapper by mistake.
      if ((code & 0xFFFF0000ul) == 0x887A0000ul) {
        return faultFromDxgiStatus(code);
      }
      return DeviceFault::kRuntimeError;
  }
}

DeviceFault faultFromQnnStatus(long status) {
  if (status == 0) {
    return DeviceFault::kNone;
  }
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

// The -12000 family means "this operation, on this object", never a missing device.
DeviceFault faultFromCoreMediaStatus(long status) {
  if (status == 0) {
    return DeviceFault::kNone;
  }
  if (status <= -12000 && status > -13000) {
    return DeviceFault::kUnsupportedOp;
  }
  return DeviceFault::kRuntimeError;
}

// Nothing maps to kRemoved, or a passing 503 would make remote session-fatal.
DeviceFault faultFromRemoteStatus(long status) {
  if (status >= 200 && status < 300) {
    return DeviceFault::kNone;
  }
  if (status == 0) {
    return DeviceFault::kRuntimeError;
  }
  switch (status) {
    case 400:
    case 413:
    case 415:
    case 422:
      return DeviceFault::kUnsupportedOp;
    case 401:
    case 403:
    case 404:
      return DeviceFault::kUnavailable;
    default:
      return DeviceFault::kRuntimeError;
  }
}

namespace {
std::string backendClassName(const std::string& tier) {
  if (tier == "cpu" || tier == "accelerate") {
    return "CPU";
  }
  if (tier == "cuda" || tier == "rocm" || tier == "vulkan" || tier == "directml" ||
      tier == "gpu" || tier == "metal") {
    return "GPU";
  }
  if (tier == "npu") {
    return "NPU";
  }
  if (tier == "ane") {
    return "ANE";
  }
  if (tier == "remote") {
    return "REMOTE";
  }
  std::string upper = tier;
  for (char& ch : upper) {
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
  }
  return upper.empty() ? "UNKNOWN" : upper;
}
}  // namespace

std::string backendAvailabilityState(const std::string& tier, ProbeResult probe,
                                     DeviceFault lastFault, bool quarantined, bool sessionFatal) {
  const std::string prefix = backendClassName(tier);
  const bool benched = quarantined || sessionFatal;
  if (benched && lastFault == DeviceFault::kUnsupportedOp) {
    return prefix + "_UNSUPPORTED";
  }
  if (benched) {
    return prefix + "_UNHEALTHY";
  }
  if (probe == ProbeResult::kAvailable) {
    return prefix + "_AVAILABLE";
  }
  return prefix + "_UNAVAILABLE";
}

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
