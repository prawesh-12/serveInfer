#include "testHarness.h"

#include "../deviceBackends.h"
#include "../deviceLadder.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string probeName(ProbeResult result) {
  return probeResultName(result);
}

std::string faultName(DeviceFault fault) {
  return deviceFaultName(fault);
}

// Without this, one test leaks policy state into the next.
class EnvGuard {
 public:
  explicit EnvGuard(const char* name) : name_(name) {
    const char* current = std::getenv(name);
    had_ = current != nullptr;
    if (had_) {
      previous_ = current;
    }
  }

  ~EnvGuard() {
    if (had_) {
      setenv(name_, previous_.c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  void set(const char* value) const {
    setenv(name_, value, 1);
  }

  void clear() const {
    unsetenv(name_);
  }

 private:
  const char* name_;
  bool had_ = false;
  std::string previous_;
};

}  // namespace

EDGE_TEST(cpu_is_always_available, "the cpu tier probes as available on every build") {
  CHECK_EQ(probeName(probeDevice("cpu")), std::string("available"));
}

EDGE_TEST(unknown_tier_reads_as_runtime_missing,
          "a tier name this build has never heard of reads as runtime_missing") {
  CHECK_EQ(probeName(probeDevice("quantum-annealer")), std::string("runtime_missing"));
  CHECK_EQ(probeName(probeDevice("")), std::string("runtime_missing"));
}

EDGE_TEST(foreign_platform_tiers_say_so,
          "tiers belonging to another operating system report wrong_platform, not an error") {
#if defined(__linux__)
  for (const char* name : {"npu", "directml", "gpu", "ane", "metal", "accelerate"}) {
    CHECK_EQ(probeName(probeDevice(name)), std::string("wrong_platform"));
  }
#endif
  // On Windows and macOS these tiers probe real hardware, so there is nothing safe to assert.
  CHECK(true);
}

EDGE_TEST(find_backend_resolves_known_names_only,
          "findBackend describes a known tier and returns null for anything else") {
  const DeviceBackend* cpu = findBackend("cpu");
  CHECK(cpu != nullptr);
  CHECK_EQ(std::string(cpu->name), std::string("cpu"));
  CHECK_EQ(std::string(cpu->platform), std::string("any"));
  CHECK(cpu->probe != nullptr);
  CHECK(std::string(cpu->note).size() > 0);

  CHECK(findBackend("nonsense") == nullptr);
  CHECK(findBackend("") == nullptr);
}

EDGE_TEST(known_backends_cover_the_documented_ladders,
          "knownBackends lists every tier the Linux, Windows and macOS ladders name") {
  const std::vector<std::string> names = knownBackends();
  CHECK(names.size() >= 11);
  for (const char* wanted : {"cpu", "cuda", "rocm", "vulkan", "npu", "directml", "gpu", "ane",
                             "metal", "accelerate", "remote"}) {
    bool found = false;
    for (const std::string& name : names) {
      if (name == wanted) {
        found = true;
        break;
      }
    }
    CHECK(found);
    CHECK(findBackend(wanted) != nullptr);
  }
}

EDGE_TEST(probe_result_names_are_stable,
          "every probe result has a stable name for /health to print") {
  CHECK_EQ(probeName(ProbeResult::kAvailable), std::string("available"));
  CHECK_EQ(probeName(ProbeResult::kWrongPlatform), std::string("wrong_platform"));
  CHECK_EQ(probeName(ProbeResult::kRuntimeMissing), std::string("runtime_missing"));
  CHECK_EQ(probeName(ProbeResult::kDeviceMissing), std::string("device_missing"));
  CHECK_EQ(probeName(ProbeResult::kPolicyDisabled), std::string("policy_disabled"));
}

EDGE_TEST(remote_tier_needs_an_endpoint_and_an_opt_in,
          "the remote tier stays off until both an endpoint and an explicit opt-in are set") {
  const EnvGuard endpoint("EDGE_REMOTE_ENDPOINT");
  const EnvGuard allowed("EDGE_REMOTE_FALLBACK_ALLOWED");

  endpoint.clear();
  allowed.clear();
  CHECK_EQ(probeName(probeDevice("remote")), std::string("runtime_missing"));

  endpoint.set("https://example.invalid/infer");
  CHECK_EQ(probeName(probeDevice("remote")), std::string("policy_disabled"));

  allowed.set("yes");
  CHECK_EQ(probeName(probeDevice("remote")), std::string("policy_disabled"));

  allowed.set("1");
  CHECK_EQ(probeName(probeDevice("remote")), std::string("available"));

  endpoint.set("");
  CHECK_EQ(probeName(probeDevice("remote")), std::string("runtime_missing"));
}

EDGE_TEST(dxgi_removal_family_is_session_fatal,
          "a removed, hung or reset DXGI adapter maps to a removed device") {
  CHECK_EQ(faultName(faultFromDxgiStatus(0x887A0005ul)), std::string("device_removed"));
  CHECK_EQ(faultName(faultFromDxgiStatus(0x887A0006ul)), std::string("device_removed"));
  CHECK_EQ(faultName(faultFromDxgiStatus(0x887A0007ul)), std::string("device_removed"));
}

EDGE_TEST(dxgi_unsupported_is_not_a_removal,
          "DXGI_ERROR_UNSUPPORTED quarantines the tier instead of killing it") {
  CHECK_EQ(faultName(faultFromDxgiStatus(0x887A0004ul)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromDxgiStatus(0x887A0020ul)), std::string("runtime_error"));
  CHECK_EQ(faultName(faultFromDxgiStatus(0x887A0022ul)), std::string("runtime_error"));
  CHECK_EQ(faultName(faultFromDxgiStatus(0x12345678ul)), std::string("runtime_error"));
}

EDGE_TEST(win32_device_codes_map_to_the_right_severity,
          "the Win32 device error codes map to removal, unavailable and unsupported") {
  CHECK_EQ(faultName(faultFromWindowsError(1617ul)), std::string("device_removed"));
  CHECK_EQ(faultName(faultFromWindowsError(1167ul)), std::string("device_unavailable"));
  CHECK_EQ(faultName(faultFromWindowsError(50ul)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromWindowsError(31ul)), std::string("runtime_error"));
  CHECK_EQ(faultName(faultFromWindowsError(9999ul)), std::string("runtime_error"));
}

EDGE_TEST(an_hresult_passed_to_the_win32_mapper_still_routes_to_dxgi,
          "an HRESULT handed to the Win32 mapper by mistake is still read as a removal") {
  CHECK_EQ(faultName(faultFromWindowsError(0x887A0005ul)), std::string("device_removed"));
  CHECK_EQ(faultName(faultFromWindowsError(0x887A0004ul)), std::string("unsupported_operation"));
}

EDGE_TEST(qnn_device_family_is_a_removal,
          "the QNN device error family is a removal and the graph family is not") {
  CHECK_EQ(faultName(faultFromQnnStatus(5000L)), std::string("device_removed"));
  CHECK_EQ(faultName(faultFromQnnStatus(5042L)), std::string("device_removed"));
  CHECK_EQ(faultName(faultFromQnnStatus(3000L)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromQnnStatus(3999L)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromQnnStatus(1001L)), std::string("runtime_error"));
  CHECK_EQ(faultName(faultFromQnnStatus(4000L)), std::string("runtime_error"));
}

EDGE_TEST(core_media_never_reports_a_removal,
          "no CoreMedia status is ever treated as a removed device") {
  CHECK_EQ(faultName(faultFromCoreMediaStatus(-12782L)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromCoreMediaStatus(-12000L)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromCoreMediaStatus(-11999L)), std::string("runtime_error"));
  CHECK_EQ(faultName(faultFromCoreMediaStatus(-13000L)), std::string("runtime_error"));

  for (long status = -14000L; status <= 200L; status += 7L) {
    CHECK(faultFromCoreMediaStatus(status) != DeviceFault::kRemoved);
  }
}

EDGE_TEST(core_ml_never_reports_a_removal,
          "no Core ML model error is ever treated as a removed device") {
  CHECK_EQ(faultName(faultFromCoreMlError(3L)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromCoreMlError(6L)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromCoreMlError(9L)), std::string("unsupported_operation"));
  CHECK_EQ(faultName(faultFromCoreMlError(1L)), std::string("runtime_error"));

  for (long code = -50L; code <= 200L; code += 1L) {
    CHECK(faultFromCoreMlError(code) != DeviceFault::kRemoved);
  }
}

EDGE_TEST(zero_means_success_everywhere,
          "every vendor mapper reads zero as success rather than a fault") {
  CHECK_EQ(faultName(faultFromDxgiStatus(0ul)), std::string("none"));
  CHECK_EQ(faultName(faultFromWindowsError(0ul)), std::string("none"));
  CHECK_EQ(faultName(faultFromQnnStatus(0L)), std::string("none"));
  CHECK_EQ(faultName(faultFromCoreMediaStatus(0L)), std::string("none"));
  CHECK_EQ(faultName(faultFromCoreMlError(0L)), std::string("none"));
}
