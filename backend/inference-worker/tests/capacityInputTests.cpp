#include "testHarness.h"

#include "../../hardware/capacityPlan.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

CapacityLimits budgets(long long gpuReserveMb, long long workerGpuMb, long long ramReserveMb,
                       long long workerRamMb, int maxWorkers) {
  CapacityLimits out;
  out.maxWorkers = maxWorkers;
  out.gpuReserveMb = gpuReserveMb;
  out.workerGpuMb = workerGpuMb;
  out.ramReserveMb = ramReserveMb;
  out.workerRamMb = workerRamMb;
  return out;
}

HardwareReport machine(long long freeVramMb, long long availableRamMb) {
  HardwareReport report;
  report.probeOk = true;
  GpuDevice gpu;
  gpu.name = "CUDA0";
  gpu.freeBytes = mbToBytes(freeVramMb);
  gpu.totalBytes = mbToBytes(freeVramMb);
  report.gpus.push_back(gpu);
  report.ram.totalBytes = mbToBytes(16384);
  report.ram.availableBytes = mbToBytes(availableRamMb);
  return report;
}

}  // namespace

EDGE_TEST(a_negative_reserve_is_treated_as_no_reserve_not_as_extra_vram,
          "a mistyped EDGE_GPU_RESERVE_MB cannot inflate usable vram past what the card has") {
  const CapacityPlan plan = planCapacity(machine(4096, 8192), budgets(-4096, 2048, 0, 1024, 8));
  CHECK_EQ(plan.usableGpuMb, 4096LL);
  CHECK_EQ(plan.gpuWorkerCapacity, 2);
}

EDGE_TEST(a_negative_ram_reserve_cannot_inflate_cpu_capacity_either,
          "the same guard on the host side") {
  const CapacityPlan plan = planCapacity(machine(0, 4096), budgets(0, 2048, -8192, 1024, 8));
  CHECK_EQ(plan.usableRamMb, 4096LL);
  CHECK_EQ(plan.cpuWorkerCapacity, 4);
}

EDGE_TEST(one_megabyte_below_the_budget_buys_no_worker_at_all,
          "the boundary is exact: capacity steps at usable == budget, not near it") {
  CHECK_EQ(planCapacity(machine(2047, 8192), budgets(0, 2048, 0, 1024, 4)).gpuWorkerCapacity, 0);
  CHECK_EQ(planCapacity(machine(2048, 8192), budgets(0, 2048, 0, 1024, 4)).gpuWorkerCapacity, 1);
  CHECK_EQ(planCapacity(machine(4095, 8192), budgets(0, 2048, 0, 1024, 4)).gpuWorkerCapacity, 1);
  CHECK_EQ(planCapacity(machine(4096, 8192), budgets(0, 2048, 0, 1024, 4)).gpuWorkerCapacity, 2);
}

EDGE_TEST(a_worker_count_of_zero_or_less_plans_no_workers_of_either_kind,
          "a machine with a huge card still gets no capacity when nothing is configured to run") {
  const CapacityPlan none = planCapacity(machine(65536, 65536), budgets(0, 1024, 0, 512, 0));
  CHECK_EQ(none.gpuWorkerCapacity, 0);
  CHECK_EQ(none.cpuWorkerCapacity, 0);
  const CapacityPlan negative = planCapacity(machine(65536, 65536), budgets(0, 1024, 0, 512, -3));
  CHECK_EQ(negative.gpuWorkerCapacity, 0);
  CHECK_EQ(negative.cpuWorkerCapacity, 0);
  CHECK(assignWorkers(negative, 0).empty());
}

EDGE_TEST(a_gpu_that_fills_every_slot_leaves_no_cpu_capacity_to_plan,
          "the two pools share one ceiling, so they can never add up to more than the "
          "configured worker count") {
  const CapacityPlan plan = planCapacity(machine(65536, 65536), budgets(0, 1024, 0, 512, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 4);
  CHECK_EQ(plan.cpuWorkerCapacity, 0);
}

EDGE_TEST(a_vram_number_far_larger_than_any_real_card_does_not_overflow_the_plan,
          "the arithmetic is done in long long and clamped, so an absurd probe reading yields "
          "the ceiling rather than a wrapped negative") {
  HardwareReport huge = machine(0, 65536);
  huge.gpus[0].freeBytes = static_cast<std::size_t>(1) << 62;
  huge.gpus[0].totalBytes = huge.gpus[0].freeBytes;
  const CapacityPlan plan = planCapacity(huge, budgets(512, 1, 0, 512, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 4);
  CHECK(plan.usableGpuMb > 0);
}

EDGE_TEST(a_probe_that_could_not_be_parsed_plans_zero_gpu_workers_and_says_why,
          "malformed probe output degrades to no gpu rather than to a guess") {
  HardwareReport unparsed;
  unparsed.probeOk = false;
  unparsed.note = "hardware probe emitted output this build cannot parse";
  unparsed.ram.totalBytes = mbToBytes(16384);
  unparsed.ram.availableBytes = mbToBytes(8192);
  const CapacityPlan plan = planCapacity(unparsed, budgets(0, 2048, 0, 1024, 4));
  CHECK_EQ(plan.gpuWorkerCapacity, 0);
  CHECK(plan.gpuReason.find("cannot parse") != std::string::npos);
  CHECK_EQ(plan.cpuWorkerCapacity, 4);
  const std::vector<WorkerAssignment> assignments = assignWorkers(plan, 4);
  for (const WorkerAssignment& assignment : assignments) {
    CHECK(assignment.backend == WorkerBackend::kCpu);
  }
}

EDGE_TEST(a_gpu_entry_missing_its_memory_numbers_fails_the_whole_parse,
          "half a device entry is worse than none: it would be planned against as a card with "
          "zero free vram forever") {
  HardwareReport out;
  CHECK(!parseHardwareReport(
      "{\"probeOk\":true,\"note\":\"\",\"ramTotalBytes\":1,\"ramAvailableBytes\":1,"
      "\"gpus\":[{\"name\":\"CUDA0\",\"description\":\"x\",\"freeBytes\":10}]}",
      out));
}

EDGE_TEST(a_missing_probe_ok_flag_fails_the_parse,
          "the flag that says whether the probe ran at all is not optional") {
  HardwareReport out;
  CHECK(!parseHardwareReport("{\"note\":\"\",\"gpus\":[]}", out));
}

EDGE_TEST(an_unclosed_gpu_array_fails_the_parse,
          "a truncated line, which is what a killed probe child leaves behind") {
  HardwareReport out;
  CHECK(!parseHardwareReport("{\"probeOk\":true,\"gpus\":[{\"name\":\"CUDA0\"", out));
}

EDGE_TEST(a_gpus_key_appearing_only_inside_a_string_is_not_mistaken_for_the_array,
          "the slicer looks for the array, and a note that merely mentions gpus does not "
          "produce a device list") {
  HardwareReport out;
  CHECK(!parseHardwareReport("{\"probeOk\":true,\"note\":\"no \\\"gpus\\\" here\"}", out));
}

EDGE_TEST(a_byte_count_too_large_for_the_machine_word_fails_rather_than_wrapping,
          "an out-of-range free-vram number is refused, not silently truncated into a "
          "plausible-looking capacity") {
  HardwareReport out;
  CHECK(!parseHardwareReport(
      "{\"probeOk\":true,\"gpus\":[{\"name\":\"CUDA0\",\"description\":\"\","
      "\"freeBytes\":99999999999999999999999,\"totalBytes\":1}]}",
      out));
}

EDGE_TEST(a_description_holding_braces_and_escapes_does_not_split_the_device_list,
          "device descriptions are vendor strings and are not trusted to be tame") {
  HardwareReport source;
  source.probeOk = true;
  GpuDevice gpu;
  gpu.name = "CUDA0";
  gpu.description = "Weird {Vendor} \"GPU\" \\ v1";
  gpu.freeBytes = 4096;
  gpu.totalBytes = 8192;
  source.gpus.push_back(gpu);
  GpuDevice second;
  second.name = "CUDA1";
  second.description = "plain";
  second.freeBytes = 16384;
  second.totalBytes = 32768;
  source.gpus.push_back(second);

  HardwareReport parsed;
  CHECK(parseHardwareReport(hardwareReportToJson(source), parsed));
  CHECK_EQ(parsed.gpus.size(), std::size_t{2});
  CHECK_EQ(parsed.gpus[0].freeBytes, std::size_t{4096});
  CHECK_EQ(parsed.gpus[1].name, std::string("CUDA1"));
  CHECK_EQ(parsed.gpus[1].freeBytes, std::size_t{16384});
}

EDGE_TEST(meminfo_with_a_malformed_value_reports_nothing_for_that_field,
          "a kernel line that does not match the expected shape is skipped, not guessed at") {
  const std::string path = "/tmp/edge-meminfo-malformed";
  FILE* file = std::fopen(path.c_str(), "w");
  CHECK(file != nullptr);
  std::fputs("MemTotal:       notanumber kB\nMemAvailable:    4096 kB\n", file);
  std::fclose(file);

  const HostMemory memory = readHostMemory(path);
  CHECK_EQ(memory.totalBytes, std::size_t{0});
  CHECK_EQ(memory.availableBytes, std::size_t{4096} * 1024);
  std::remove(path.c_str());
}

EDGE_TEST(meminfo_without_memavailable_yields_zero_and_therefore_no_cpu_capacity,
          "an older kernel with no MemAvailable is planned as unknown, which the capacity plan "
          "reads as none rather than as unlimited") {
  const std::string path = "/tmp/edge-meminfo-nomemavailable";
  FILE* file = std::fopen(path.c_str(), "w");
  CHECK(file != nullptr);
  std::fputs("MemTotal:       16384 kB\nMemFree:         2048 kB\n", file);
  std::fclose(file);

  const HostMemory memory = readHostMemory(path);
  CHECK_EQ(memory.totalBytes, std::size_t{16384} * 1024);
  CHECK_EQ(memory.availableBytes, std::size_t{0});
  std::remove(path.c_str());

  HardwareReport report;
  report.probeOk = true;
  report.ram = memory;
  const CapacityPlan plan = planCapacity(report, budgets(0, 2048, 0, 1024, 4));
  CHECK_EQ(plan.cpuWorkerCapacity, 0);
  CHECK(plan.cpuReason.find("unknown") != std::string::npos);
  std::remove(path.c_str());
}

EDGE_TEST(meminfo_stated_in_a_unit_this_parser_does_not_know_is_ignored,
          "every field in /proc/meminfo is kB; anything else is not this file and is not read "
          "as if it were") {
  const std::string path = "/tmp/edge-meminfo-units";
  FILE* file = std::fopen(path.c_str(), "w");
  CHECK(file != nullptr);
  std::fputs("MemTotal:       16 GB\nMemAvailable:    8 MB\n", file);
  std::fclose(file);

  const HostMemory memory = readHostMemory(path);
  CHECK_EQ(memory.totalBytes, std::size_t{0});
  CHECK_EQ(memory.availableBytes, std::size_t{0});
  std::remove(path.c_str());
}
