// CUDA initialization as one path: plan -> assign -> env -> the ladder selects cuda, undegraded.

#include "testHarness.h"

#include "../../hardware/capacityPlan.h"
#include "../backendRouter.h"
#include "../deviceBackends.h"
#include "../inferenceBackend.h"

#include <memory>
#include <string>
#include <vector>

namespace {

// Stands in for a CUDA device that came up, because a CI machine has no GPU.
class InitializedDevice : public InferenceBackend {
 public:
  explicit InitializedDevice(std::string name) : name_(std::move(name)) {}

  const std::string& name() const override {
    return name_;
  }
  ProbeResult available() override {
    return up ? ProbeResult::kAvailable : ProbeResult::kRuntimeMissing;
  }
  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override {
    ++executeCalls;
    BackendExecution result;
    result.text = name_ + " ran: " + prompt;
    if (onToken) {
      onToken(result.text);
    }
    return result;
  }
  bool healthCheck() override {
    return up;
  }
  bool executable() const override {
    return true;
  }

  bool up = true;
  int executeCalls = 0;

 private:
  std::string name_;
};

HardwareReport reportWithGpu(long long freeMb) {
  HardwareReport report;
  report.probeOk = true;
  GpuDevice gpu;
  gpu.name = "CUDA0";
  gpu.description = "NVIDIA GeForce RTX 2050";
  gpu.freeBytes = mbToBytes(freeMb);
  gpu.totalBytes = mbToBytes(4096);
  report.gpus.push_back(gpu);
  report.ram.totalBytes = mbToBytes(16384);
  report.ram.availableBytes = mbToBytes(12288);
  return report;
}

CapacityLimits gpuFriendlyLimits() {
  CapacityLimits out;
  out.maxWorkers = 4;
  out.gpuReserveMb = 512;
  out.workerGpuMb = 2048;
  out.ramReserveMb = 0;
  out.workerRamMb = 512;
  return out;
}

std::string envValue(const WorkerAssignment& assignment, const std::string& key) {
  for (const auto& entry : workerBackendEnv(assignment)) {
    if (entry.first == key) {
      return entry.second;
    }
  }
  return "<unset>";
}

}  // namespace

EDGE_TEST(cuda_initialization_success_serves_the_request_on_cuda_and_undegraded,
          "a gpu the probe found is planned as cuda, selected by the ladder, and the request is "
          "reported as served on cuda with no degradation (fake, no hardware)") {

  const CapacityPlan plan = planCapacity(reportWithGpu(3758), gpuFriendlyLimits());
  CHECK(plan.gpuWorkerCapacity >= 1);


  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 4);
  CHECK_EQ(workerBackendName(assignments[0].backend), std::string("cuda"));
  CHECK_EQ(envValue(assignments[0], "CUDA_VISIBLE_DEVICES"), std::string("0"));

  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  auto cuda = std::make_shared<InitializedDevice>("cuda");
  auto cpu = std::make_shared<InitializedDevice>("cpu");
  router.registerBackend(cuda);
  router.registerBackend(cpu);

  CHECK(router.select());
  CHECK_EQ(router.active(), std::string("cuda"));
  CHECK(!router.ladder().degraded());

  const RouteResult routed = router.route("what is 2+2", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cuda"));
  CHECK(!routed.degraded);
  CHECK(routed.degradedReason.empty());
  CHECK_EQ(cuda->executeCalls, 1);
  // An untouched cpu tier separates a real cuda success from a silent fallback that says cuda.
  CHECK_EQ(cpu->executeCalls, 0);
}

EDGE_TEST(a_cuda_worker_that_never_came_up_is_not_reported_as_a_cuda_success,
          "the same chain with the device failing to initialize never runs anything on cuda and "
          "reports cpu, so an init success and an init failure are distinguishable (fake, no "
          "hardware)") {
  BackendRouter router({"cuda", "cpu"}, 60000, 0);
  auto cuda = std::make_shared<InitializedDevice>("cuda");
  auto cpu = std::make_shared<InitializedDevice>("cpu");
  cuda->up = false;
  router.registerBackend(cuda);
  router.registerBackend(cpu);

  CHECK(router.select());
  CHECK_EQ(router.active(), std::string("cpu"));
  // Degradation is measured against the best tier at startup, so a cpu-only machine is not degraded.
  CHECK(!router.ladder().degraded());
  CHECK_EQ(router.ladder().baselineIndex(), router.ladder().activeIndex());
  CHECK_EQ(router.ladder().tierState(0), std::string("GPU_UNAVAILABLE"));

  const RouteResult routed = router.route("what is 2+2", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK_EQ(cuda->executeCalls, 0);
  CHECK_EQ(cpu->executeCalls, 1);
}

EDGE_TEST(a_cpu_only_plan_never_produces_a_worker_that_could_initialize_cuda,
          "every worker in a cpu-only plan is started with cuda hidden, so no cpu worker can pay "
          "for a primary context it will not use") {
  HardwareReport noGpu;
  noGpu.probeOk = true;
  noGpu.note = "ggml registered 1 device(s), none of them a gpu";
  noGpu.ram.totalBytes = mbToBytes(16384);
  noGpu.ram.availableBytes = mbToBytes(12288);

  const CapacityPlan plan = planCapacity(noGpu, gpuFriendlyLimits());
  CHECK_EQ(plan.gpuWorkerCapacity, 0);

  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 4);
  CHECK(!assignments.empty());
  for (const WorkerAssignment& assignment : assignments) {
    CHECK_EQ(workerBackendName(assignment.backend), std::string("cpu"));
    CHECK_EQ(envValue(assignment, "CUDA_VISIBLE_DEVICES"), std::string("-1"));
  }
}
