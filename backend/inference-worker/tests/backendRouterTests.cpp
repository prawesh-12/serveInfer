#include "testHarness.h"

#include "../backendRouter.h"
#include "../deviceBackends.h"
#include "../inferenceBackend.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class FakeBackend : public InferenceBackend {
 public:
  explicit FakeBackend(std::string name) : name_(std::move(name)) {}

  const std::string& name() const override {
    return name_;
  }

  ProbeResult available() override {
    ++probeCalls;
    return probeResult;
  }

  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override {
    ++executeCalls;
    BackendExecution result;
    if (nextFault != DeviceFault::kNone) {
      result.fault = nextFault;
      result.detail = faultDetail;
      result.vendorStatus = vendorStatus;
      return result;
    }
    result.text = reply.empty() ? name_ + " ran: " + prompt : reply;
    if (onToken) {
      onToken(result.text);
    }
    return result;
  }

  bool healthCheck() override {
    ++healthCheckCalls;
    return probeResult == ProbeResult::kAvailable;
  }

  bool executable() const override {
    return executable_;
  }

  void setExecutable(bool value) {
    executable_ = value;
  }

  ProbeResult probeResult = ProbeResult::kAvailable;
  DeviceFault nextFault = DeviceFault::kNone;
  std::string faultDetail;
  long vendorStatus = 0;
  std::string reply;
  int probeCalls = 0;
  int executeCalls = 0;
  int healthCheckCalls = 0;

 private:
  std::string name_;
  bool executable_ = true;
};

struct Rig {
  std::unique_ptr<BackendRouter> router;
  std::vector<std::shared_ptr<FakeBackend>> backends;

  FakeBackend& operator[](const std::string& name) const {
    for (const auto& backend : backends) {
      if (backend->name() == name) {
        return *backend;
      }
    }
    throw std::runtime_error("no fake backend named " + name);
  }
};

// probeIntervalMs 0 turns the probe cache off, so a health check answers now.
Rig makeRig(const std::vector<std::string>& order, int quarantineMs) {
  Rig rig;
  rig.router = std::make_unique<BackendRouter>(order, quarantineMs, 0);
  for (const std::string& name : order) {
    auto backend = std::make_shared<FakeBackend>(name);
    rig.backends.push_back(backend);
    rig.router->registerBackend(backend);
  }
  return rig;
}

}  // namespace

EDGE_TEST(npu_1_is_available_when_its_adapter_probes_available,
          "the npu tier is selected when its adapter reports available (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("npu"));
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("NPU_AVAILABLE"));
  CHECK_EQ(rig.router->ladder().tierState(1), std::string("CPU_AVAILABLE"));
}

EDGE_TEST(npu_2_inference_succeeds_on_the_npu,
          "a healthy npu runs the request and reports no degradation (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());

  const RouteResult routed = rig.router->route("summarise this", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("npu"));
  CHECK(!routed.degraded);
  CHECK_EQ(rig["npu"].executeCalls, 1);
  CHECK_EQ(rig["cpu"].executeCalls, 0);
}

EDGE_TEST(npu_3_error_device_removed_is_classified_as_a_removal,
          "the QNN device error family maps to device_removed, unlike its graph family") {
  CHECK_EQ(std::string(deviceFaultName(faultFromQnnStatus(5001))), std::string("device_removed"));
  CHECK_EQ(std::string(deviceFaultName(faultFromQnnStatus(3002))),
           std::string("unsupported_operation"));
  CHECK_EQ(std::string(deviceFaultName(faultFromWindowsError(1617))), std::string("device_removed"));
}

EDGE_TEST(npu_4_a_removal_makes_the_tier_unhealthy,
          "ERROR_DEVICE_REMOVED moves the npu into NPU_UNHEALTHY (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());

  rig["npu"].nextFault = faultFromWindowsError(1617);
  rig["npu"].faultDetail = "ERROR_DEVICE_REMOVED";
  rig["npu"].vendorStatus = 1617;

  const RouteResult routed = rig.router->route("summarise this", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("NPU_UNHEALTHY"));
}

EDGE_TEST(npu_5_subsequent_requests_skip_the_unhealthy_npu,
          "later requests never touch the npu again while it is unhealthy (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());

  rig["npu"].nextFault = DeviceFault::kRemoved;
  rig["npu"].faultDetail = "ERROR_DEVICE_REMOVED";
  CHECK(rig.router->route("first", nullptr).ok());
  const int callsAtRemoval = rig["npu"].executeCalls;

  for (int i = 0; i < 5; ++i) {
    const RouteResult routed = rig.router->route("later", nullptr);
    CHECK(routed.ok());
    CHECK_EQ(routed.device, std::string("cpu"));
  }
  CHECK_EQ(rig["npu"].executeCalls, callsAtRemoval);
}

EDGE_TEST(npu_6_the_request_is_served_by_the_cpu_fallback,
          "the request that hit the removal is still answered, on the cpu (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  rig["cpu"].reply = "answered on cpu";
  rig["npu"].nextFault = DeviceFault::kRemoved;
  rig["npu"].faultDetail = "ERROR_DEVICE_REMOVED";

  const RouteResult routed = rig.router->route("summarise this", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK_EQ(routed.text, std::string("answered on cpu"));
  CHECK(routed.degraded);
  CHECK_EQ(routed.degradedReason, std::string("npu:device_removed"));
}

EDGE_TEST(npu_7_a_failing_health_check_does_not_bring_the_npu_back,
          "a new session whose npu health check fails leaves the tier out (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRemoved;
  rig["npu"].faultDetail = "ERROR_DEVICE_REMOVED";
  CHECK(rig.router->route("first", nullptr).ok());

  rig["npu"].probeResult = ProbeResult::kDeviceMissing;
  rig.router->beginSession();
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("cpu"));
}

EDGE_TEST(npu_8_the_npu_remains_unavailable_after_the_failed_check,
          "the tier reads NPU_UNAVAILABLE and requests keep going to the cpu (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRemoved;
  CHECK(rig.router->route("first", nullptr).ok());
  rig["npu"].probeResult = ProbeResult::kDeviceMissing;
  rig.router->beginSession();
  CHECK(rig.router->select());

  CHECK_EQ(rig.router->ladder().tierState(0), std::string("NPU_UNAVAILABLE"));
  const RouteResult routed = rig.router->route("again", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
}

EDGE_TEST(npu_9_a_passing_health_check_is_what_gates_the_return,
          "the npu is only reconsidered once its own health check passes (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRemoved;
  CHECK(rig.router->route("first", nullptr).ok());

  rig["npu"].probeResult = ProbeResult::kDeviceMissing;
  rig.router->beginSession();
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("cpu"));

  rig["npu"].probeResult = ProbeResult::kAvailable;
  CHECK(rig.router->ladder().healthCheck("npu"));
}

EDGE_TEST(npu_10_becomes_eligible_again_after_recovery,
          "a recovered npu is selected again and the ladder stops reporting degraded (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRemoved;
  CHECK(rig.router->route("first", nullptr).ok());
  CHECK(rig.router->ladder().degraded());

  rig["npu"].nextFault = DeviceFault::kNone;
  rig["npu"].probeResult = ProbeResult::kAvailable;
  rig.router->beginSession();
  CHECK(rig.router->select());

  CHECK_EQ(rig.router->active(), std::string("npu"));
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("NPU_AVAILABLE"));
  CHECK(!rig.router->ladder().degraded());
}

EDGE_TEST(npu_11_runs_inference_again_after_recovery,
          "a recovered npu serves requests again, not degraded (fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRemoved;
  CHECK(rig.router->route("first", nullptr).ok());

  rig["npu"].nextFault = DeviceFault::kNone;
  rig["npu"].reply = "npu is back";
  rig.router->beginSession();
  CHECK(rig.router->select());

  const RouteResult routed = rig.router->route("again", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("npu"));
  CHECK_EQ(routed.text, std::string("npu is back"));
  CHECK(!routed.degraded);
}

EDGE_TEST(npu_removal_is_final_within_one_session,
          "a removed npu stays out for the whole session no matter how many requests arrive "
          "(fake, no hardware)") {
  Rig rig = makeRig({"npu", "cpu"}, 0);
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRemoved;
  CHECK(rig.router->route("first", nullptr).ok());

  rig["npu"].nextFault = DeviceFault::kNone;
  rig["npu"].probeResult = ProbeResult::kAvailable;
  for (int i = 0; i < 3; ++i) {
    CHECK(rig.router->select());
    CHECK_EQ(rig.router->active(), std::string("cpu"));
  }
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("NPU_UNHEALTHY"));
}

EDGE_TEST(ane_1_success_runs_on_the_neural_engine,
          "a healthy ane serves the request and is not degraded (fake, no hardware)") {
  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  CHECK(rig.router->select());
  rig["ane"].reply = "ane output";

  const RouteResult routed = rig.router->route("transcribe", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("ane"));
  CHECK_EQ(routed.text, std::string("ane output"));
  CHECK(!routed.degraded);
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("ANE_AVAILABLE"));
}

EDGE_TEST(ane_2_unsupported_operation_is_not_read_as_a_removal,
          "kCMErrorUnsupportedOperation maps to unsupported_operation, never device_removed") {
  CHECK_EQ(std::string(deviceFaultName(faultFromCoreMediaStatus(kCMErrorUnsupportedOperation))),
           std::string("unsupported_operation"));
  CHECK_EQ(kCMErrorUnsupportedOperation, -12782L);
  for (long status = -12000; status > -13000; status -= 137) {
    CHECK(faultFromCoreMediaStatus(status) != DeviceFault::kRemoved);
  }
}

EDGE_TEST(ane_3_falls_back_to_the_metal_gpu,
          "an unsupported op moves the work to metal and says so (fake, no hardware)") {
  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  rig["ane"].faultDetail = "kCMErrorUnsupportedOperation";
  rig["ane"].vendorStatus = kCMErrorUnsupportedOperation;
  rig["metal"].reply = "metal output";

  const RouteResult routed = rig.router->route("transcribe", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("metal"));
  CHECK_EQ(routed.text, std::string("metal output"));
  CHECK_EQ(routed.degradedReason, std::string("ane:unsupported_operation"));
}

EDGE_TEST(ane_4_metal_unavailable_is_skipped_entirely,
          "a metal tier that fails its probe is never tried (fake, no hardware)") {
  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  rig["metal"].probeResult = ProbeResult::kRuntimeMissing;
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  rig["cpu"].reply = "cpu output";

  const RouteResult routed = rig.router->route("transcribe", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK_EQ(rig["metal"].executeCalls, 0);
  CHECK_EQ(rig.router->ladder().tierState(1), std::string("GPU_UNAVAILABLE"));
}

EDGE_TEST(ane_5_metal_failure_falls_through_to_the_cpu,
          "a metal tier that faults hands the work to the cpu (fake, no hardware)") {
  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  rig["metal"].nextFault = DeviceFault::kRuntimeError;
  rig["metal"].faultDetail = "command buffer failed";
  rig["cpu"].reply = "cpu output";

  const RouteResult routed = rig.router->route("transcribe", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK_EQ(rig["ane"].executeCalls, 1);
  CHECK_EQ(rig["metal"].executeCalls, 1);
  CHECK_EQ(rig["cpu"].executeCalls, 1);
}

EDGE_TEST(ane_6_remote_is_the_last_resort_and_only_when_it_is_configured,
          "remote serves only after every local tier failed, and only if it probes available "
          "(fake, no hardware or network)") {
  Rig configured = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  CHECK(configured.router->select());
  configured["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  configured["metal"].nextFault = DeviceFault::kRuntimeError;
  configured["cpu"].nextFault = DeviceFault::kRuntimeError;
  configured["remote"].reply = "cloud output";

  const RouteResult routed = configured.router->route("transcribe", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("remote"));
  CHECK_EQ(routed.text, std::string("cloud output"));

  Rig unconfigured = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  unconfigured["remote"].probeResult = ProbeResult::kPolicyDisabled;
  CHECK(unconfigured.router->select());
  unconfigured["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  unconfigured["metal"].nextFault = DeviceFault::kRuntimeError;
  unconfigured["cpu"].nextFault = DeviceFault::kRuntimeError;

  const RouteResult refused = unconfigured.router->route("transcribe", nullptr);
  CHECK(!refused.ok());
  CHECK_EQ(unconfigured["remote"].executeCalls, 0);
}

EDGE_TEST(ane_7_the_same_unsupported_op_is_never_retried_on_the_ane,
          "the ane is tried once per unsupported op, not once per request (fake, no hardware)") {
  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);

  for (int i = 0; i < 4; ++i) {
    const RouteResult routed = rig.router->route("the same graph", nullptr);
    CHECK(routed.ok());
    CHECK_EQ(routed.device, std::string("metal"));
  }
  CHECK_EQ(rig["ane"].executeCalls, 1);
  CHECK_EQ(rig["metal"].executeCalls, 4);
}

EDGE_TEST(ane_8_degraded_mode_is_reported_through_the_existing_contract,
          "degradation shows up as device, degraded and degradedReason, no new fields") {
  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 60000);
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);

  const RouteResult routed = rig.router->route("transcribe", nullptr);
  CHECK(routed.degraded);
  CHECK_EQ(routed.device, std::string("metal"));
  CHECK_EQ(routed.degradedReason, std::string("ane:unsupported_operation"));
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("ANE_UNSUPPORTED"));
}

EDGE_TEST(ane_9_recovers_once_its_quarantine_closes_and_its_health_check_passes,
          "an unsupported op quarantines the ane rather than killing it, so a different "
          "model may use it again (fake, no hardware)") {
  // No session boundary needed: the device was never gone, so window plus health check is the gate.
  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 30);
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  CHECK(rig.router->route("an unsupported graph", nullptr).ok());
  CHECK(rig.router->ladder().degraded());

  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("metal"));

  rig["ane"].nextFault = DeviceFault::kNone;
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("ane"));
  CHECK(!rig.router->ladder().degraded());
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("ANE_AVAILABLE"));
}

EDGE_TEST(a_backend_that_cannot_execute_is_never_credited_with_running_anything,
          "an adapter that reports it cannot execute is routed past, not silently believed") {
  Rig rig = makeRig({"npu", "cpu"}, 60000);
  rig["npu"].setExecutable(false);
  rig["npu"].reply = "this would be a lie";
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("npu"));

  const RouteResult routed = rig.router->route("prompt", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK(routed.degraded);
}

EDGE_TEST(the_real_qualcomm_adapter_refuses_instead_of_faking_success,
          "the shipped hexagon adapter reports that the backend is not compiled into this build") {
  QualcommHexagonBackend hexagon;
  CHECK_EQ(hexagon.name(), std::string("npu"));
  CHECK(!hexagon.executable());

  const BackendExecution result = hexagon.execute("prompt", nullptr);
  CHECK(!result.ok());
  CHECK(result.detail.find("not compiled into this build") != std::string::npos);
  CHECK(result.text.empty());
}

EDGE_TEST(the_real_core_ml_adapter_refuses_instead_of_faking_success,
          "the shipped core ml adapter reports that it has no compiled ane model in this build") {
  CoreMlAneBackend coreMl;
  CHECK_EQ(coreMl.name(), std::string("ane"));
  CHECK(!coreMl.executable());

  const BackendExecution result = coreMl.execute("prompt", nullptr);
  CHECK(!result.ok());
  CHECK(result.detail.find("core ml") != std::string::npos);
  CHECK(result.text.empty());
}

EDGE_TEST(routing_stops_instead_of_looping_when_every_tier_has_faulted,
          "with no tier left the router returns the failure rather than retrying forever") {
  Rig rig = makeRig({"npu", "cpu"}, 60000);
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRuntimeError;
  rig["npu"].faultDetail = "npu broke";
  rig["cpu"].nextFault = DeviceFault::kRuntimeError;
  rig["cpu"].faultDetail = "cpu broke";

  const RouteResult routed = rig.router->route("prompt", nullptr);
  CHECK(!routed.ok());
  CHECK_EQ(routed.detail, std::string("cpu broke"));
  CHECK_EQ(rig["npu"].executeCalls, 1);
  CHECK_EQ(rig["cpu"].executeCalls, 1);
}

EDGE_TEST(a_known_failed_backend_is_not_reprobed_for_every_request,
          "the ladder's probe cache stops a benched tier from being asked once per request") {
  // probeIntervalMs is non-zero here on purpose: this case is about the probe cache.
  Rig rig;
  rig.router = std::make_unique<BackendRouter>(std::vector<std::string>{"npu", "cpu"}, 60000,
                                               60000);
  for (const std::string& name : {std::string("npu"), std::string("cpu")}) {
    auto backend = std::make_shared<FakeBackend>(name);
    rig.backends.push_back(backend);
    rig.router->registerBackend(backend);
  }
  rig["npu"].probeResult = ProbeResult::kDeviceMissing;
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("cpu"));

  const int afterSelect = rig["npu"].probeCalls;
  for (int i = 0; i < 10; ++i) {
    CHECK(rig.router->route("prompt", nullptr).ok());
  }
  CHECK_EQ(rig["npu"].probeCalls, afterSelect);
}

EDGE_TEST(the_fallback_hook_is_told_which_tiers_the_ladder_moved_between,
          "every tier change calls the hook once with the previous and next tier names") {
  Rig rig = makeRig({"ane", "metal", "cpu"}, 60000);
  std::vector<std::string> moves;
  rig.router->setFallbackHook([&](const std::string& previous, const std::string& next) {
    moves.push_back(previous + "->" + next);
    return true;
  });
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  rig["metal"].nextFault = DeviceFault::kRuntimeError;

  CHECK(rig.router->route("prompt", nullptr).ok());
  CHECK_EQ(moves.size(), std::size_t{2});
  CHECK_EQ(moves[0], std::string("ane->metal"));
  CHECK_EQ(moves[1], std::string("metal->cpu"));
}

EDGE_TEST(a_hook_that_cannot_bring_up_the_next_tier_stops_the_walk,
          "routing aborts rather than trying a third tier with an engine that failed to reload") {
  Rig rig = makeRig({"ane", "metal", "cpu"}, 60000);
  rig.router->setFallbackHook([](const std::string&, const std::string&) { return false; });
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  rig["ane"].faultDetail = "kCMErrorUnsupportedOperation";

  const RouteResult routed = rig.router->route("prompt", nullptr);
  CHECK(!routed.ok());
  CHECK_EQ(routed.detail, std::string("kCMErrorUnsupportedOperation"));
  CHECK_EQ(rig["metal"].executeCalls, 0);
  CHECK_EQ(rig["cpu"].executeCalls, 0);
}

EDGE_TEST(a_request_served_by_the_first_tier_never_calls_the_fallback_hook,
          "no fault means no tier change, so nothing is reloaded on the happy path") {
  Rig rig = makeRig({"cuda", "cpu"}, 60000);
  int hookCalls = 0;
  rig.router->setFallbackHook([&](const std::string&, const std::string&) {
    ++hookCalls;
    return true;
  });
  CHECK(rig.router->select());

  CHECK(rig.router->route("prompt", nullptr).ok());
  CHECK_EQ(hookCalls, 0);
}

EDGE_TEST(streaming_tokens_reach_the_caller_through_the_router,
          "route passes a token sink down to the backend so the streaming path is not a special case") {
  Rig rig = makeRig({"cpu"}, 0);
  CHECK(rig.router->select());
  rig["cpu"].reply = "streamed";

  std::string seen;
  const RouteResult routed = rig.router->route("prompt", [&](const std::string& token) {
    seen += token;
  });
  CHECK(routed.ok());
  CHECK_EQ(seen, std::string("streamed"));
}
