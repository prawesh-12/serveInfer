#include "testHarness.h"

#include "../backendRouter.h"
#include "../gpuResourceOwner.h"
#include "../inferenceBackend.h"

#include <memory>
#include <string>
#include <vector>

// What the runtime promises a client when a tier faults mid-request. Every tier below is a fake.

namespace {

// The multi-token case the single-token fake in backendRouterTests cannot express.
class StreamingFake : public InferenceBackend {
 public:
  explicit StreamingFake(std::string name) : name_(std::move(name)) {}

  const std::string& name() const override {
    return name_;
  }

  ProbeResult available() override {
    return probeResult;
  }

  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override {
    (void)prompt;
    ++executeCalls;
    BackendExecution result;
    for (const std::string& token : tokens) {
      result.text += token;
      if (onToken) {
        onToken(token);
      }
    }
    result.fault = nextFault;
    result.detail = faultDetail;
    return result;
  }

  bool healthCheck() override {
    return probeResult == ProbeResult::kAvailable;
  }

  bool executable() const override {
    return true;
  }

  ProbeResult probeResult = ProbeResult::kAvailable;
  DeviceFault nextFault = DeviceFault::kNone;
  std::string faultDetail;
  std::vector<std::string> tokens;
  int executeCalls = 0;

 private:
  std::string name_;
};

// The only situation in which the fallback mode changes anything.
class ResidentOwner : public GpuResourceOwner {
 public:
  bool releaseDeviceResources() override {
    ++releaseCalls;
    return true;
  }
  bool deviceResourcesResident() const override {
    return true;
  }
  int releaseCalls = 0;
};

class CleanOwner : public GpuResourceOwner {
 public:
  bool releaseDeviceResources() override {
    return true;
  }
  bool deviceResourcesResident() const override {
    return false;
  }
};

}  // namespace

// The worker's final result frame is built from RouteResult.text, so that field is wire contract.

EDGE_TEST(a_fallback_mid_stream_leaves_the_result_frame_carrying_only_the_serving_tier,
          "tokens already sent stay sent, but the final frame is the answer of the tier that "
          "finished, never the two concatenated") {
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  auto cuda = std::make_shared<StreamingFake>("cuda");
  cuda->tokens = {"par", "tial"};
  cuda->nextFault = DeviceFault::kRuntimeError;
  cuda->faultDetail = "decode failed";
  auto cpu = std::make_shared<StreamingFake>("cpu");
  cpu->tokens = {"the ", "real ", "answer"};
  router.registerBackend(cuda);
  router.registerBackend(cpu);
  CHECK(router.select());

  std::string streamed;
  const RouteResult routed = router.route("prompt", [&](const std::string& token) {
    streamed += token;
  });

  CHECK(routed.ok());
  CHECK_EQ(streamed, std::string("partialthe real answer"));
  CHECK_EQ(routed.text, std::string("the real answer"));
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK(routed.degraded);
}

EDGE_TEST(a_total_failure_still_returns_the_partial_text_the_client_already_saw,
          "with no tier left the result frame carries what the failing tier produced, so it "
          "does not contradict the tokens already streamed") {
  BackendRouter router({"cpu"}, 60000, 0);
  auto cpu = std::make_shared<StreamingFake>("cpu");
  cpu->tokens = {"half an ", "answer"};
  cpu->nextFault = DeviceFault::kRuntimeError;
  cpu->faultDetail = "llama_decode rc=-1";
  router.registerBackend(cpu);
  CHECK(router.select());

  std::string streamed;
  const RouteResult routed = router.route("prompt", [&](const std::string& token) {
    streamed += token;
  });

  CHECK(!routed.ok());
  CHECK_EQ(streamed, std::string("half an answer"));
  CHECK_EQ(routed.text, streamed);
  CHECK_EQ(routed.detail, std::string("llama_decode rc=-1"));
}

EDGE_TEST(a_hook_failure_also_preserves_the_partial_text,
          "an engine that cannot reload on the next tier does not cost the client the text the "
          "previous tier had already produced") {
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  auto cuda = std::make_shared<StreamingFake>("cuda");
  cuda->tokens = {"before the fault"};
  cuda->nextFault = DeviceFault::kRuntimeError;
  cuda->faultDetail = "device fell over";
  auto cpu = std::make_shared<StreamingFake>("cpu");
  router.registerBackend(cuda);
  router.registerBackend(cpu);
  router.setFallbackHook([](const std::string&, const std::string&) { return false; });
  CHECK(router.select());

  const RouteResult routed = router.route("prompt", nullptr);
  CHECK(!routed.ok());
  CHECK_EQ(routed.text, std::string("before the fault"));
  CHECK_EQ(cpu->executeCalls, 0);
}

EDGE_TEST(the_router_never_injects_a_token_the_backend_did_not_produce,
          "the token stream carries model output only, so no synthetic error string is ever "
          "rendered in a browser as if the model had said it") {
  BackendRouter router({"npu", "cpu"}, 60000, 0);
  auto npu = std::make_shared<StreamingFake>("npu");
  npu->nextFault = DeviceFault::kRemoved;
  npu->faultDetail = "ERROR_DEVICE_REMOVED";
  auto cpu = std::make_shared<StreamingFake>("cpu");
  cpu->nextFault = DeviceFault::kRuntimeError;
  router.registerBackend(npu);
  router.registerBackend(cpu);
  CHECK(router.select());

  std::vector<std::string> seen;
  const RouteResult routed = router.route("prompt", [&](const std::string& token) {
    seen.push_back(token);
  });
  CHECK(!routed.ok());
  CHECK_EQ(seen.size(), std::size_t{0});
}

EDGE_TEST(reload_mode_keeps_the_process_alive_after_a_device_to_cpu_fallback,
          "EDGE_DEVICE_FALLBACK_MODE=reload reloads in place even though the device context "
          "stays resident") {
  ResidentOwner owner;
  CHECK(deviceFallbackAction(owner, "cuda", "cpu", false) ==
        DeviceFallbackAction::kReloadInPlace);
  // The mode does not make the context go away, only whether to act on it.
  CHECK(requiresProcessRestartForCpuOnly(owner, "cuda", "cpu"));
}

EDGE_TEST(reexec_mode_is_the_default_and_asks_for_a_respawn,
          "the shipped mode ends the process so the replacement is genuinely cpu-only") {
  ResidentOwner owner;
  CHECK(deviceFallbackAction(owner, "cuda", "cpu", true) == DeviceFallbackAction::kReexecAsCpu);
}

EDGE_TEST(reexec_mode_still_reloads_in_place_when_there_is_nothing_to_give_back,
          "a worker holding no device state keeps running whatever the mode says") {
  CleanOwner owner;
  CHECK(deviceFallbackAction(owner, "cuda", "cpu", true) == DeviceFallbackAction::kReloadInPlace);
  ResidentOwner resident;
  CHECK(deviceFallbackAction(resident, "cuda", "metal", true) ==
        DeviceFallbackAction::kReloadInPlace);
}

EDGE_TEST(only_the_exact_string_reload_turns_the_reexec_off,
          "an unset or misspelled EDGE_DEVICE_FALLBACK_MODE keeps the safe default") {
  CHECK(deviceFallbackModeIsReexec(nullptr));
  CHECK(deviceFallbackModeIsReexec("reexec"));
  CHECK(deviceFallbackModeIsReexec(""));
  CHECK(deviceFallbackModeIsReexec("Reload"));
  CHECK(deviceFallbackModeIsReexec("reload-in-place"));
  CHECK(!deviceFallbackModeIsReexec("reload"));
}
