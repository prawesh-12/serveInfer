// The Hexagon route: the branch exists and is gated on GGML_USE_HEXAGON, which is closed here.

#include "testHarness.h"

#include "../inferEngine.h"
#include "../inferenceBackend.h"

#include <cstdlib>
#include <string>

namespace {

InferConfig mockEngineConfig() {
  InferConfig config;
  // This suite is built without EDGE_USE_LLAMA, so no file is opened.
  config.modelPath = "/nonexistent/model.gguf";
  return config;
}

}  // namespace

EDGE_TEST(hexagon_is_not_compiled_into_this_build,
          "GGML_USE_HEXAGON is undefined here, so the adapter's build gate is closed and it says "
          "so rather than pretending") {
  CHECK(!QualcommHexagonBackend::hexagonCompiledIn());

  QualcommHexagonBackend hexagon;
  CHECK(!hexagon.routable());
  CHECK(!hexagon.executable());

  const BackendExecution refused = hexagon.execute("prompt", nullptr);
  CHECK(!refused.ok());
  CHECK(refused.fault == DeviceFault::kUnavailable);
  CHECK(refused.detail.find("not compiled into this build") != std::string::npos);
  CHECK(refused.text.empty());
}

EDGE_TEST(binding_an_engine_does_not_make_a_hexagon_less_build_open_the_route,
          "an engine bound on a build with no hexagon still refuses: the gate is the build, not "
          "the wiring") {
  InferEngine engine(mockEngineConfig());
  QualcommHexagonBackend hexagon(&engine);

  CHECK(!hexagon.executable());
  const BackendExecution refused = hexagon.execute("prompt", nullptr);
  CHECK(!refused.ok());
  CHECK(refused.detail.find("not compiled into this build") != std::string::npos);
}

EDGE_TEST(with_the_build_gate_open_and_no_engine_the_adapter_still_refuses,
          "hexagon present but nothing bound to route into is a refusal, never a fabricated "
          "success (build gate lifted, no hardware)") {
  QualcommHexagonBackend hexagon;
  hexagon.setRouteEnabledForTest(true);

  CHECK(hexagon.routable());
  CHECK(!hexagon.executable());

  const BackendExecution refused = hexagon.execute("prompt", nullptr);
  CHECK(!refused.ok());
  CHECK(refused.fault == DeviceFault::kUnavailable);
  CHECK(refused.detail.find("no inference engine is bound") != std::string::npos);
}

EDGE_TEST(with_the_build_gate_open_and_an_engine_bound_the_adapter_routes_through_llama,
          "the route exists and ends in the same InferEngine every other tier runs through "
          "(build gate lifted, no hardware)") {
  InferEngine engine(mockEngineConfig());
  QualcommHexagonBackend hexagon;
  hexagon.bindEngine(&engine);
  hexagon.setRouteEnabledForTest(true);

  CHECK(hexagon.executable());

  const BackendExecution routed = hexagon.execute("what is 2+2", nullptr);
  CHECK(routed.ok());
  // The stub wrapper's own answer, which is what proves the prompt reached the engine.
  CHECK_EQ(routed.text, std::string("Inference response: what is 2+2"));
}

EDGE_TEST(the_hexagon_route_streams_through_the_same_token_sink,
          "routing is not a special case: streaming reaches the caller's sink unchanged "
          "(build gate lifted, no hardware)") {
  InferEngine engine(mockEngineConfig());
  QualcommHexagonBackend hexagon(&engine);
  hexagon.setRouteEnabledForTest(true);

  std::string streamed;
  const BackendExecution routed =
      hexagon.execute("hello", [&](const std::string& token) { streamed += token; });

  CHECK(routed.ok());
  CHECK(!streamed.empty());
  CHECK_EQ(streamed, routed.text);
}

EDGE_TEST(a_fault_on_the_hexagon_route_propagates_instead_of_being_swallowed,
          "an engine fault raised while routing on npu comes back as a fault, so the ladder can "
          "quarantine the tier and fall back (build gate lifted, no hardware)") {
  InferEngine engine(mockEngineConfig());
  QualcommHexagonBackend hexagon(&engine);
  hexagon.setRouteEnabledForTest(true);

  setenv("EDGE_SIMULATE_DEVICE_FAULT", "removed", 1);
  const BackendExecution routed = hexagon.execute("prompt", nullptr);
  unsetenv("EDGE_SIMULATE_DEVICE_FAULT");

  CHECK(!routed.ok());
  CHECK(routed.fault == DeviceFault::kRemoved);
}

EDGE_TEST(the_core_ml_adapter_has_no_build_gate_because_llama_cpp_has_no_core_ml,
          "the ane adapter is deliberately not given hexagon's routed branch: there is no ggml "
          "core ml backend and so no macro to gate on") {
  CoreMlAneBackend coreMl;
  CHECK(!coreMl.executable());

  const BackendExecution refused = coreMl.execute("prompt", nullptr);
  CHECK(!refused.ok());
  CHECK(refused.fault == DeviceFault::kUnavailable);
  CHECK(refused.text.empty());
  CHECK(refused.detail.find("core ml") != std::string::npos);
}
