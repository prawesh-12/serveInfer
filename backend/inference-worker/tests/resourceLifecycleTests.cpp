#include "testHarness.h"

#include "../gpuResourceOwner.h"
#include "../inferEngine.h"

#include <string>

// The cleanup path driven through an injectable owner, deterministically and with no GPU.

namespace {

// Can be told to behave like a backend whose device state outlives the release: the CUDA case.
class FakeResourceOwner : public GpuResourceOwner {
 public:
  bool releaseDeviceResources() override {
    ++releaseCalls;
    modelLoaded = false;
    contextLoaded = false;
    if (!contextSurvivesRelease) {
      resident = false;
    }
    return releaseSucceeds;
  }

  bool deviceResourcesResident() const override {
    return resident;
  }

  void initializeOnDevice() {
    modelLoaded = true;
    contextLoaded = true;
    resident = true;
  }

  int releaseCalls = 0;
  bool releaseSucceeds = true;
  bool modelLoaded = false;
  bool contextLoaded = false;
  bool resident = false;
  // True models the CUDA primary context in llama.cpp e85caa81: no teardown in-process.
  bool contextSurvivesRelease = true;
};

}  // namespace

EDGE_TEST(cleanup_is_invoked_on_a_gpu_to_cpu_transition,
          "falling from a device tier to the cpu releases the model and context exactly once "
          "(fake, no hardware)") {
  FakeResourceOwner owner;
  owner.initializeOnDevice();
  CHECK(owner.modelLoaded);

  CHECK(owner.releaseDeviceResources());
  CHECK_EQ(owner.releaseCalls, 1);
  CHECK(!owner.modelLoaded);
  CHECK(!owner.contextLoaded);
}

EDGE_TEST(a_release_that_fails_is_reported_rather_than_assumed,
          "releaseDeviceResources returning false is visible to the caller") {
  FakeResourceOwner owner;
  owner.initializeOnDevice();
  owner.releaseSucceeds = false;

  CHECK(!owner.releaseDeviceResources());
  CHECK_EQ(owner.releaseCalls, 1);
}

EDGE_TEST(a_backend_with_no_teardown_still_reports_its_state_as_resident,
          "releasing the model does not let the owner claim the device context went with it") {
  FakeResourceOwner owner;
  owner.initializeOnDevice();
  owner.contextSurvivesRelease = true;

  CHECK(owner.releaseDeviceResources());
  CHECK(owner.deviceResourcesResident());
}

EDGE_TEST(a_backend_with_a_real_teardown_reports_nothing_resident,
          "an owner whose device state can be freed says so, and no re-exec is needed") {
  FakeResourceOwner owner;
  owner.initializeOnDevice();
  owner.contextSurvivesRelease = false;

  CHECK(owner.releaseDeviceResources());
  CHECK(!owner.deviceResourcesResident());
  CHECK(!requiresProcessRestartForCpuOnly(owner, "cuda", "cpu"));
}

EDGE_TEST(a_cpu_only_worker_is_only_guaranteed_by_a_new_process,
          "after a cuda to cpu fallback the worker asks to be respawned, because the primary "
          "context cannot be released in-process (fake, no hardware)") {
  FakeResourceOwner owner;
  owner.initializeOnDevice();
  owner.releaseDeviceResources();

  CHECK(requiresProcessRestartForCpuOnly(owner, "cuda", "cpu"));
}

EDGE_TEST(a_fallback_that_stays_within_the_device_class_does_not_force_a_respawn,
          "cuda to metal, or any move that is not a drop to the cpu, reloads in place "
          "(fake, no hardware)") {
  FakeResourceOwner owner;
  owner.initializeOnDevice();

  CHECK(!requiresProcessRestartForCpuOnly(owner, "cuda", "metal"));
  CHECK(!requiresProcessRestartForCpuOnly(owner, "npu", "directml"));
}

EDGE_TEST(a_worker_that_never_touched_a_device_does_not_respawn_itself,
          "a cpu worker whose tier changes has nothing to give back, so it keeps running") {
  FakeResourceOwner owner;  // never initialized on a device
  CHECK(!owner.deviceResourcesResident());
  CHECK(!requiresProcessRestartForCpuOnly(owner, "cpu", "cpu"));
  CHECK(!requiresProcessRestartForCpuOnly(owner, "vulkan", "cpu"));
}

// Built without EDGE_USE_LLAMA, so these check the wrapper's bookkeeping, not llama.

EDGE_TEST(the_engine_reports_no_resident_device_state_without_a_llama_backend,
          "an InferEngine in a build with no llama backend never claims a device context") {
  InferConfig config;
  config.modelPath = "/nonexistent/model.gguf";
  InferEngine engine(config);

  CHECK(!engine.deviceResourcesResident());
  CHECK(engine.releaseDeviceResources());
  CHECK(!engine.deviceResourcesResident());
}

EDGE_TEST(releasing_engine_resources_is_safe_to_repeat,
          "calling releaseDeviceResources twice does not double free") {
  InferConfig config;
  config.modelPath = "/nonexistent/model.gguf";
  InferEngine engine(config);

  CHECK(engine.releaseDeviceResources());
  CHECK(engine.releaseDeviceResources());
}

EDGE_TEST(reloading_on_the_cpu_flips_the_engines_own_gpu_flag,
          "reloadOn(true) leaves the engine reporting cpu execution") {
  InferConfig config;
  config.modelPath = "/nonexistent/model.gguf";
  InferEngine engine(config);

  CHECK(engine.reloadOn(true));
  CHECK(!engine.isUsingGPU());
}
