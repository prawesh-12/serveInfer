#include "testHarness.h"

#include "../../hardware/capacityPlan.h"

#include <string>
#include <vector>

namespace {

CapacityLimits limits(long long gpuReserveMb, long long workerGpuMb, int maxWorkers) {
  CapacityLimits out;
  out.maxWorkers = maxWorkers;
  out.gpuReserveMb = gpuReserveMb;
  out.workerGpuMb = workerGpuMb;
  // Generous on the host side so the GPU cases are not silently RAM-limited.
  out.ramReserveMb = 0;
  out.workerRamMb = 512;
  return out;
}

HardwareReport withGpu(long long freeMb, long long totalMb, long long availableRamMb) {
  HardwareReport report;
  report.probeOk = true;
  GpuDevice gpu;
  gpu.name = "CUDA0";
  gpu.description = "NVIDIA GeForce RTX 2050";
  gpu.freeBytes = mbToBytes(freeMb);
  gpu.totalBytes = mbToBytes(totalMb);
  report.gpus.push_back(gpu);
  report.ram.totalBytes = mbToBytes(16384);
  report.ram.availableBytes = mbToBytes(availableRamMb);
  return report;
}

HardwareReport withoutGpu(long long availableRamMb) {
  HardwareReport report;
  report.probeOk = true;
  report.note = "ggml registered 1 device(s), none of them a gpu";
  report.ram.totalBytes = mbToBytes(16384);
  report.ram.availableBytes = mbToBytes(availableRamMb);
  return report;
}

std::string backendOf(const std::vector<WorkerAssignment>& assignments, int workerId) {
  for (const WorkerAssignment& assignment : assignments) {
    if (assignment.workerId == workerId) {
      return workerBackendName(assignment.backend);
    }
  }
  return "missing";
}

std::string envValue(const WorkerAssignment& assignment, const std::string& key) {
  for (const auto& entry : workerBackendEnv(assignment)) {
    if (entry.first == key) {
      return entry.second;
    }
  }
  return "<unset>";
}

WorkerAssignment assignmentAt(const std::vector<WorkerAssignment>& assignments, int workerId) {
  for (const WorkerAssignment& assignment : assignments) {
    if (assignment.workerId == workerId) {
      return assignment;
    }
  }
  return WorkerAssignment{};
}

}  // namespace

EDGE_TEST(no_gpu_yields_zero_gpu_capacity,
          "a machine with no gpu device gets no gpu workers and says why") {
  const CapacityPlan plan = planCapacity(withoutGpu(8192), limits(512, 2048, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 0);
  CHECK(plan.gpuReason.find("no gpu device") != std::string::npos);
  CHECK(plan.cpuWorkerCapacity > 0);
}

EDGE_TEST(a_probe_that_did_not_report_is_treated_as_no_gpu,
          "a failed or unparseable hardware probe plans as no gpu rather than guessing one") {
  HardwareReport failed;
  failed.probeOk = false;
  failed.note = "hardware probe timed out after 10000ms";
  failed.ram.availableBytes = mbToBytes(8192);

  const CapacityPlan plan = planCapacity(failed, limits(512, 2048, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 0);
  CHECK(plan.gpuReason.find("timed out") != std::string::npos);
}

EDGE_TEST(zero_free_vram_yields_zero_gpu_capacity,
          "a card whose vram is entirely spoken for gets no gpu workers") {
  const CapacityPlan plan = planCapacity(withGpu(0, 4096, 8192), limits(512, 2048, 4));
  CHECK_EQ(plan.freeVramMb, 0LL);
  CHECK_EQ(plan.usableGpuMb, 0LL);
  CHECK_EQ(plan.gpuWorkerCapacity, 0);
}

EDGE_TEST(insufficient_vram_yields_zero_gpu_capacity,
          "free vram above the reserve but below one worker budget still fits no gpu worker") {
  // 2000 free - 512 reserve = 1488 usable, one worker needs 2048.
  const CapacityPlan plan = planCapacity(withGpu(2000, 4096, 8192), limits(512, 2048, 4));
  CHECK_EQ(plan.usableGpuMb, 1488LL);
  CHECK_EQ(plan.gpuWorkerCapacity, 0);
  CHECK(plan.gpuReason.find("below the per-worker budget") != std::string::npos);
}

EDGE_TEST(exactly_one_worker_fits,
          "usable vram equal to one worker budget fits exactly one gpu worker") {
  // 2560 free - 512 reserve = 2048 usable, exactly one 2048MB worker.
  const CapacityPlan plan = planCapacity(withGpu(2560, 4096, 8192), limits(512, 2048, 4));
  CHECK_EQ(plan.usableGpuMb, 2048LL);
  CHECK_EQ(plan.gpuWorkerCapacity, 1);
}

EDGE_TEST(multiple_workers_fit_and_the_floor_is_not_rounded_up,
          "capacity is the floor of the division, so leftover vram never buys a partial worker") {
  // 7000 free - 1000 reserve = 6000 usable, 6000 / 2048 = 2 remainder 1904.
  const CapacityPlan plan = planCapacity(withGpu(7000, 8192, 16384), limits(1000, 2048, 8));
  CHECK_EQ(plan.usableGpuMb, 6000LL);
  CHECK_EQ(plan.gpuWorkerCapacity, 2);
}

EDGE_TEST(the_reserve_is_subtracted_before_the_division,
          "raising the reserve removes gpu workers even though free vram did not change") {
  const HardwareReport machine = withGpu(6144, 8192, 16384);
  CHECK_EQ(planCapacity(machine, limits(0, 2048, 8)).gpuWorkerCapacity, 3);
  CHECK_EQ(planCapacity(machine, limits(2048, 2048, 8)).gpuWorkerCapacity, 2);
  CHECK_EQ(planCapacity(machine, limits(6144, 2048, 8)).gpuWorkerCapacity, 0);
}

EDGE_TEST(the_reserve_can_never_drive_usable_vram_negative,
          "a reserve larger than free vram clamps usable vram to zero") {
  const CapacityPlan plan = planCapacity(withGpu(1024, 4096, 8192), limits(4096, 2048, 4));
  CHECK_EQ(plan.usableGpuMb, 0LL);
  CHECK_EQ(plan.gpuWorkerCapacity, 0);
}

EDGE_TEST(the_per_worker_budget_scales_capacity_inversely,
          "a smaller per-worker gpu budget fits more gpu workers out of the same vram") {
  const HardwareReport machine = withGpu(8192, 8192, 16384);
  CHECK_EQ(planCapacity(machine, limits(0, 8192, 8)).gpuWorkerCapacity, 1);
  CHECK_EQ(planCapacity(machine, limits(0, 2048, 8)).gpuWorkerCapacity, 4);
  CHECK_EQ(planCapacity(machine, limits(0, 1024, 8)).gpuWorkerCapacity, 8);
}

EDGE_TEST(a_zero_worker_budget_is_refused_instead_of_dividing_by_zero,
          "a non-positive per-worker budget yields zero capacity and names the setting") {
  CapacityLimits broken = limits(0, 0, 4);
  const CapacityPlan plan = planCapacity(withGpu(8192, 8192, 8192), broken);
  CHECK_EQ(plan.gpuWorkerCapacity, 0);
  CHECK(plan.gpuReason.find("EDGE_WORKER_GPU_MB") != std::string::npos);

  broken.workerGpuMb = 2048;
  broken.workerRamMb = 0;
  const CapacityPlan ramBroken = planCapacity(withGpu(8192, 8192, 8192), broken);
  CHECK_EQ(ramBroken.cpuWorkerCapacity, 0);
  CHECK(ramBroken.cpuReason.find("EDGE_WORKER_RAM_MB") != std::string::npos);
}

EDGE_TEST(gpu_capacity_is_clamped_to_the_configured_worker_count,
          "a card with room for more workers than are configured does not invent extra ones") {
  const CapacityPlan plan = planCapacity(withGpu(32768, 32768, 65536), limits(0, 1024, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 4);
}

EDGE_TEST(the_roomiest_gpu_is_the_one_planned_against,
          "with several cards, capacity is sized off the one with the most free vram") {
  HardwareReport twoCards = withGpu(1024, 4096, 16384);
  GpuDevice second;
  second.name = "CUDA1";
  second.freeBytes = mbToBytes(6144);
  second.totalBytes = mbToBytes(8192);
  twoCards.gpus.push_back(second);

  const CapacityPlan plan = planCapacity(twoCards, limits(0, 2048, 4));
  CHECK_EQ(plan.freeVramMb, 6144LL);
  CHECK_EQ(plan.gpuWorkerCapacity, 3);
}

EDGE_TEST(cpu_capacity_comes_from_available_ram_not_total,
          "cpu capacity is sized off MemAvailable minus the reserve, divided by the worker budget") {
  CapacityLimits budget = limits(0, 2048, 4);
  budget.ramReserveMb = 1024;
  budget.workerRamMb = 1024;

  // No gpu, so all four slots are candidates. 5120 - 1024 = 4096 / 1024 = 4.
  const CapacityPlan plan = planCapacity(withoutGpu(5120), budget);
  CHECK_EQ(plan.availableRamMb, 5120LL);
  CHECK_EQ(plan.usableRamMb, 4096LL);
  CHECK_EQ(plan.cpuWorkerCapacity, 4);
}

EDGE_TEST(a_ram_constrained_machine_gets_fewer_cpu_workers_than_configured,
          "scarce ram caps cpu capacity below the configured worker count instead of overcommitting") {
  CapacityLimits budget = limits(0, 2048, 4);
  budget.ramReserveMb = 1024;
  budget.workerRamMb = 1024;

  // 2560 - 1024 = 1536 usable, which is one 1024MB worker, not four.
  const CapacityPlan plan = planCapacity(withoutGpu(2560), budget);
  CHECK_EQ(plan.cpuWorkerCapacity, 1);
}

EDGE_TEST(unknown_available_ram_is_treated_as_none,
          "a meminfo read that produced nothing plans zero cpu workers rather than assuming plenty") {
  HardwareReport blind = withoutGpu(0);
  blind.ram.availableBytes = 0;
  const CapacityPlan plan = planCapacity(blind, limits(0, 2048, 4));
  CHECK_EQ(plan.cpuWorkerCapacity, 0);
  CHECK(plan.cpuReason.find("available ram unknown") != std::string::npos);
}

EDGE_TEST(cpu_capacity_only_covers_the_slots_the_gpu_did_not_take,
          "gpu and cpu capacity together never exceed the configured worker count") {
  CapacityLimits budget = limits(512, 2048, 4);
  budget.ramReserveMb = 0;
  budget.workerRamMb = 256;

  const CapacityPlan plan = planCapacity(withGpu(6656, 8192, 16384), budget);
  CHECK_EQ(plan.gpuWorkerCapacity, 3);
  CHECK_EQ(plan.cpuWorkerCapacity, 1);
}


EDGE_TEST(gpu_only_capacity_puts_every_worker_on_cuda,
          "when gpu capacity covers every worker, no worker is assigned to the cpu") {
  const CapacityPlan plan = planCapacity(withGpu(16384, 16384, 16384), limits(0, 2048, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 4);

  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 4);
  CHECK_EQ(assignments.size(), std::size_t{4});
  for (int workerId = 0; workerId < 4; ++workerId) {
    CHECK_EQ(backendOf(assignments, workerId), std::string("cuda"));
  }
}

EDGE_TEST(mixed_capacity_fills_gpu_slots_first,
          "one gpu slot on a four-worker pool gives worker 0 cuda and workers 1..3 cpu") {
  // The reference machine: 4GB card, ~3758MB free, 512MB reserve, 2048MB budget.
  const CapacityPlan plan = planCapacity(withGpu(3758, 4096, 12288), limits(512, 2048, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 1);

  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 4);
  CHECK_EQ(backendOf(assignments, 0), std::string("cuda"));
  CHECK_EQ(backendOf(assignments, 1), std::string("cpu"));
  CHECK_EQ(backendOf(assignments, 2), std::string("cpu"));
  CHECK_EQ(backendOf(assignments, 3), std::string("cpu"));
}

EDGE_TEST(cpu_only_mode_assigns_every_worker_to_the_cpu,
          "a machine with no gpu assigns every worker to the cpu and names the reason") {
  const CapacityPlan plan = planCapacity(withoutGpu(8192), limits(512, 2048, 4));
  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 4);

  CHECK_EQ(assignments.size(), std::size_t{4});
  for (int workerId = 0; workerId < 4; ++workerId) {
    CHECK_EQ(backendOf(assignments, workerId), std::string("cpu"));
  }
  CHECK(assignmentAt(assignments, 0).reason.find("no gpu slot") != std::string::npos);
}

EDGE_TEST(every_assignment_carries_a_reason,
          "each worker assignment states why it got the backend it got") {
  const CapacityPlan plan = planCapacity(withGpu(3758, 4096, 12288), limits(512, 2048, 4));
  for (const WorkerAssignment& assignment : assignWorkers(plan, 4)) {
    CHECK(!assignment.reason.empty());
  }
}

EDGE_TEST(assigning_zero_workers_produces_no_assignments,
          "a worker count of zero or less yields an empty assignment list, not a crash") {
  const CapacityPlan plan = planCapacity(withGpu(8192, 8192, 8192), limits(0, 2048, 4));
  CHECK(assignWorkers(plan, 0).empty());
  CHECK(assignWorkers(plan, -3).empty());
}

EDGE_TEST(a_cpu_worker_is_started_with_no_visible_cuda_device,
          "a cpu assignment sets CUDA_VISIBLE_DEVICES=-1 so the worker never initializes cuda") {
  const CapacityPlan plan = planCapacity(withGpu(3758, 4096, 12288), limits(512, 2048, 4));
  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 4);

  for (int workerId = 1; workerId < 4; ++workerId) {
    const WorkerAssignment assignment = assignmentAt(assignments, workerId);
    CHECK_EQ(envValue(assignment, "CUDA_VISIBLE_DEVICES"), std::string("-1"));
    CHECK_EQ(envValue(assignment, "EDGE_WORKER_BACKEND"), std::string("cpu"));
  }
}

EDGE_TEST(a_gpu_worker_is_started_with_a_visible_cuda_device,
          "a cuda assignment names a real device index and declares the cuda backend") {
  const CapacityPlan plan = planCapacity(withGpu(3758, 4096, 12288), limits(512, 2048, 4));
  const WorkerAssignment worker0 = assignmentAt(assignWorkers(plan, 4), 0);

  CHECK_EQ(envValue(worker0, "CUDA_VISIBLE_DEVICES"), std::string("0"));
  CHECK_EQ(envValue(worker0, "EDGE_WORKER_BACKEND"), std::string("cuda"));
}

EDGE_TEST(cuda_initialization_failure_leaves_the_whole_pool_on_the_cpu,
          "when discovery reports no usable gpu, no worker is ever handed a cuda device") {
  HardwareReport failed;
  failed.probeOk = false;
  failed.note = "hardware probe exited exit_127";
  failed.ram.availableBytes = mbToBytes(8192);

  const CapacityPlan plan = planCapacity(failed, limits(512, 2048, 4));
  for (const WorkerAssignment& assignment : assignWorkers(plan, 4)) {
    CHECK_EQ(workerBackendName(assignment.backend), std::string("cpu"));
    CHECK_EQ(envValue(assignment, "CUDA_VISIBLE_DEVICES"), std::string("-1"));
  }
}

EDGE_TEST(a_demoted_worker_comes_back_with_cuda_hidden,
          "a gpu worker demoted after a device fault is respawned with no visible cuda device") {
  const CapacityPlan plan = planCapacity(withGpu(3758, 4096, 12288), limits(512, 2048, 4));
  const WorkerAssignment worker0 = assignmentAt(assignWorkers(plan, 4), 0);
  CHECK_EQ(workerBackendName(worker0.backend), std::string("cuda"));

  const WorkerAssignment demoted = demoteToCpu(worker0, "device fault on cuda");
  CHECK_EQ(demoted.workerId, 0);
  CHECK_EQ(workerBackendName(demoted.backend), std::string("cpu"));
  CHECK_EQ(envValue(demoted, "CUDA_VISIBLE_DEVICES"), std::string("-1"));
  CHECK_EQ(envValue(demoted, "EDGE_WORKER_BACKEND"), std::string("cpu"));
  CHECK(demoted.reason.find("device fault") != std::string::npos);
}

EDGE_TEST(a_machine_with_room_for_every_worker_starts_every_worker,
          "when the plan affords the configured count, nothing is held back") {
  const CapacityPlan plan = planCapacity(withGpu(3758, 4096, 12288), limits(512, 2048, 4));
  CHECK_EQ(placeableWorkerCount(plan, 4), 4);
}

EDGE_TEST(ram_scarcity_reduces_the_number_of_workers_actually_started,
          "a machine that can only feed two workers starts two, not the four configured") {
  CapacityLimits tight = limits(512, 2048, 4);
  tight.ramReserveMb = 1024;
  tight.workerRamMb = 1024;
  // 2247MB available - 1024 reserve = 1223MB usable, which is one cpu worker.
  const CapacityPlan plan = planCapacity(withGpu(3679, 3770, 2247), tight);

  CHECK_EQ(plan.gpuWorkerCapacity, 1);
  CHECK_EQ(plan.cpuWorkerCapacity, 1);
  CHECK_EQ(placeableWorkerCount(plan, 4), 2);
}

EDGE_TEST(the_surplus_workers_are_the_ones_that_are_not_started,
          "the workers that do start are the planned ones, each with a backend and a reason") {
  CapacityLimits tight = limits(512, 2048, 4);
  tight.ramReserveMb = 1024;
  tight.workerRamMb = 1024;
  const CapacityPlan plan = planCapacity(withGpu(3679, 3770, 2247), tight);

  const std::vector<WorkerAssignment> assignments =
      assignWorkers(plan, placeableWorkerCount(plan, 4));
  CHECK_EQ(assignments.size(), static_cast<std::size_t>(2));
  CHECK_EQ(backendOf(assignments, 0), std::string("cuda"));
  CHECK_EQ(backendOf(assignments, 1), std::string("cpu"));
  for (const WorkerAssignment& assignment : assignments) {
    CHECK(!assignment.reason.empty());
  }
}

EDGE_TEST(a_plan_that_affords_nothing_still_starts_one_worker,
          "a runtime that starts no worker answers nothing, so the floor is one") {
  CapacityLimits starved = limits(512, 2048, 4);
  starved.ramReserveMb = 8192;
  starved.workerRamMb = 1024;
  const CapacityPlan plan = planCapacity(withoutGpu(1024), starved);

  CHECK_EQ(plan.gpuWorkerCapacity, 0);
  CHECK_EQ(plan.cpuWorkerCapacity, 0);
  CHECK_EQ(placeableWorkerCount(plan, 4), 1);
}

EDGE_TEST(a_configured_count_of_zero_places_no_workers,
          "the floor of one applies to a real pool, not to a pool that was turned off") {
  const CapacityPlan plan = planCapacity(withoutGpu(12288), limits(512, 2048, 4));
  CHECK_EQ(placeableWorkerCount(plan, 0), 0);
  CHECK_EQ(placeableWorkerCount(plan, -3), 0);
}

EDGE_TEST(capacity_never_raises_the_worker_count_above_what_was_configured,
          "a roomy machine does not invent workers beyond EDGE_WORKER_COUNT") {
  const CapacityPlan plan = planCapacity(withGpu(80000, 81920, 262144), limits(512, 2048, 2));
  CHECK_EQ(placeableWorkerCount(plan, 2), 2);
}

EDGE_TEST(the_plan_serializes_the_numbers_the_dashboard_shows,
          "capacityPlanToJson carries the budgets and the chosen gpu, because the supervisor is "
          "the only process that ever probed the machine") {
  const CapacityPlan plan = planCapacity(withGpu(3758, 4096, 12288), limits(512, 2048, 4));
  const std::string json = capacityPlanToJson(plan);

  CHECK(json.find("\"gpuWorkerCapacity\":1") != std::string::npos);
  CHECK(json.find("\"usableGpuMb\":3246") != std::string::npos);
  CHECK(json.find("\"gpuName\":\"NVIDIA GeForce RTX 2050\"") != std::string::npos);
  CHECK(json.find("\"gpuIndex\":0") != std::string::npos);
  CHECK(json.find("\"cpuWorkerCapacity\":") != std::string::npos);
  CHECK(json.front() == '{' && json.back() == '}');
}

EDGE_TEST(a_reason_containing_quotes_does_not_break_the_model_config,
          "the reasons are free text written by the planner, and the file they land in is parsed "
          "by the dashboard") {
  CapacityPlan plan;
  plan.gpuName = "GPU \"zero\"";
  plan.gpuReason = "line one\nline \"two\"";

  const std::string json = capacityPlanToJson(plan);
  CHECK(json.find("GPU \\\"zero\\\"") != std::string::npos);
  CHECK(json.find("line one\\nline \\\"two\\\"") != std::string::npos);
  CHECK(json.find('\n') == std::string::npos);
}

EDGE_TEST(assignments_serialize_one_entry_per_started_worker,
          "the per-worker backend and the reason for it are the only place an operator can see "
          "why a worker landed on cpu") {
  const CapacityPlan plan = planCapacity(withGpu(7000, 8192, 12288), limits(1000, 2048, 4));
  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 3);
  const std::string json = workerAssignmentsToJson(assignments);

  CHECK(json.front() == '[' && json.back() == ']');
  CHECK(json.find("\"workerId\":0") != std::string::npos);
  CHECK(json.find("\"workerId\":2") != std::string::npos);
  CHECK(json.find("\"backend\":\"cuda\"") != std::string::npos);
  CHECK(json.find("\"backend\":\"cpu\"") != std::string::npos);

  CHECK_EQ(workerAssignmentsToJson({}), std::string("[]"));
}
