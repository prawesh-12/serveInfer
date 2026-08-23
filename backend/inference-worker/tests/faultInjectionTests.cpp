#include "testHarness.h"

#include "../backendRouter.h"
#include "../inferEngine.h"
#include "../inferenceBackend.h"

#include <cstdlib>
#include <memory>
#include <string>

namespace {

InferConfig mockEngineConfig() {
  InferConfig config;
  config.modelPath = "/nonexistent/model.gguf";
  return config;
}

// probeDevice("cuda") is false on a machine with no GPU, so the real adapter could never be
// selected here. Only the probe is faked: execute() is still the shipped engine path.
class SelectableLlamaBackend : public LlamaInferenceBackend {
 public:
  using LlamaInferenceBackend::LlamaInferenceBackend;

  ProbeResult available() override {
    return ProbeResult::kAvailable;
  }
  bool healthCheck() override {
    return true;
  }
};

class InjectedFault {
 public:
  explicit InjectedFault(const char* spec) {
    setenv("EDGE_SIMULATE_DEVICE_FAULT", spec, 1);
  }
  ~InjectedFault() {
    unsetenv("EDGE_SIMULATE_DEVICE_FAULT");
  }

  InjectedFault(const InjectedFault&) = delete;
  InjectedFault& operator=(const InjectedFault&) = delete;
};

std::string mockAnswer(const std::string& prompt) {
  return "Inference response: " + prompt;
}

}  // namespace

EDGE_TEST(a_tier_targeted_fault_drops_that_tier_and_the_next_one_answers_normally,
          "cuda:removed faults cuda, and the cpu tier the ladder falls to serves the same request "
          "with real output rather than inheriting the fault (fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cuda", &engine));
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));

  CHECK(router.select());
  CHECK_EQ(router.active(), std::string("cuda"));

  const InjectedFault fault("cuda:removed");
  const RouteResult routed = router.route("what is 2+2", nullptr);

  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK_EQ(routed.text, mockAnswer("what is 2+2"));
  CHECK(routed.degraded);
  CHECK_EQ(routed.degradedReason, std::string("cuda:device_removed"));
}

EDGE_TEST(the_fault_is_spent_after_one_injection_so_later_requests_are_untouched,
          "the same variable stays set for the life of the worker, so a second and third request "
          "must run normally instead of faulting again (fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cuda", &engine));
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));
  CHECK(router.select());

  const InjectedFault fault("cuda:removed");
  CHECK_EQ(router.route("first", nullptr).device, std::string("cpu"));

  const RouteResult second = router.route("second", nullptr);
  CHECK(second.ok());
  CHECK_EQ(second.device, std::string("cpu"));
  CHECK_EQ(second.text, mockAnswer("second"));

  const RouteResult third = router.route("third", nullptr);
  CHECK(third.ok());
  CHECK_EQ(third.device, std::string("cpu"));
  CHECK_EQ(third.text, mockAnswer("third"));
}

EDGE_TEST(a_cuda_targeted_fault_never_fires_on_a_cpu_worker,
          "a respawned cpu-only worker inherits the same environment, so a fault named for cuda "
          "must not touch it (fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));
  CHECK(router.select());
  CHECK_EQ(router.active(), std::string("cpu"));

  const InjectedFault fault("cuda:removed");
  const RouteResult routed = router.route("still working", nullptr);

  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK_EQ(routed.text, mockAnswer("still working"));
  CHECK(!routed.degraded);
}

EDGE_TEST(the_tier_target_is_honoured_in_both_directions,
          "cpu:removed does reach a cpu worker, so the tier check is a real match and not a "
          "blanket exemption for cpu (fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));
  CHECK(router.select());

  const InjectedFault fault("cpu:removed");
  const RouteResult routed = router.route("prompt", nullptr);

  CHECK(!routed.ok());
  CHECK(routed.fault == DeviceFault::kRemoved);
  CHECK_EQ(routed.detail, std::string("EDGE_SIMULATE_DEVICE_FAULT"));
}

EDGE_TEST(an_untargeted_fault_still_works_and_is_also_spent_after_one_request,
          "the documented bare form keeps faulting whichever tier runs first, but only once "
          "(fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cuda", &engine));
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));
  CHECK(router.select());

  const InjectedFault fault("removed");
  const RouteResult first = router.route("first", nullptr);
  CHECK(first.ok());
  CHECK_EQ(first.device, std::string("cpu"));
  CHECK_EQ(first.degradedReason, std::string("cuda:device_removed"));

  const RouteResult second = router.route("second", nullptr);
  CHECK(second.ok());
  CHECK_EQ(second.text, mockAnswer("second"));
}

EDGE_TEST(an_empty_tier_target_is_a_typo_rather_than_a_wildcard,
          "\":removed\" names no tier, so it injects nothing instead of faulting every tier it "
          "meets (fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cuda", &engine));
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));
  CHECK(router.select());

  const InjectedFault fault(":removed");
  const RouteResult routed = router.route("prompt", nullptr);

  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cuda"));
  CHECK(!routed.degraded);
}

EDGE_TEST(inference_with_the_variable_unset_is_unchanged,
          "the injection hook costs an unconfigured worker nothing: every request routes on the "
          "best tier and returns the engine's own answer (fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cuda", &engine));
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));
  CHECK(router.select());

  for (const std::string& prompt : {std::string("one"), std::string("two")}) {
    const RouteResult routed = router.route(prompt, nullptr);
    CHECK(routed.ok());
    CHECK_EQ(routed.device, std::string("cuda"));
    CHECK_EQ(routed.text, mockAnswer(prompt));
    CHECK(!routed.degraded);
  }
}

EDGE_TEST(a_streamed_request_survives_a_spent_fault_the_same_way,
          "the streaming path shares the one-shot flag, so tokens still arrive on the tier the "
          "ladder fell to (fake probe, no hardware)") {
  InferEngine engine(mockEngineConfig());
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cuda", &engine));
  router.registerBackend(std::make_shared<SelectableLlamaBackend>("cpu", &engine));
  CHECK(router.select());

  const InjectedFault fault("cuda:removed");
  std::string streamed;
  const RouteResult routed =
      router.route("stream me", [&](const std::string& token) { streamed += token; });

  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK_EQ(streamed, mockAnswer("stream me"));
  CHECK_EQ(routed.text, streamed);
}
