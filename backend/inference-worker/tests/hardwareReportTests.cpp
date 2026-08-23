#include "testHarness.h"

#include "../../hardware/hardwareReport.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// A fixture file rather than the real /proc/meminfo, which is why readHostMemory takes a path.
class MeminfoFixture {
 public:
  explicit MeminfoFixture(const std::string& body) {
    path_ = "/tmp/edge-meminfo-fixture-" + std::to_string(counter()++) + ".txt";
    std::ofstream out(path_, std::ios::trunc);
    out << body;
  }

  ~MeminfoFixture() {
    std::remove(path_.c_str());
  }

  const std::string& path() const {
    return path_;
  }

 private:
  static int& counter() {
    static int value = 0;
    return value;
  }

  std::string path_;
};

HardwareReport sampleReport() {
  HardwareReport report;
  report.probeOk = true;
  report.note = "ggml registered 1 gpu device(s)";
  report.ram.totalBytes = mbToBytes(16384);
  report.ram.availableBytes = mbToBytes(9001);

  GpuDevice gpu;
  gpu.name = "CUDA0";
  gpu.description = "NVIDIA GeForce RTX 2050";
  gpu.freeBytes = mbToBytes(3758);
  gpu.totalBytes = mbToBytes(4096);
  report.gpus.push_back(gpu);
  return report;
}

}  // namespace

EDGE_TEST(hardware_report_survives_a_json_round_trip,
          "a report serialized by the probe child parses back identically in the supervisor") {
  const HardwareReport original = sampleReport();
  HardwareReport parsed;
  CHECK(parseHardwareReport(hardwareReportToJson(original), parsed));

  CHECK_EQ(parsed.probeOk, true);
  CHECK_EQ(parsed.note, original.note);
  CHECK_EQ(parsed.ram.totalBytes, original.ram.totalBytes);
  CHECK_EQ(parsed.ram.availableBytes, original.ram.availableBytes);
  CHECK_EQ(parsed.gpus.size(), std::size_t{1});
  CHECK_EQ(parsed.gpus[0].name, std::string("CUDA0"));
  CHECK_EQ(parsed.gpus[0].description, std::string("NVIDIA GeForce RTX 2050"));
  CHECK_EQ(parsed.gpus[0].freeBytes, original.gpus[0].freeBytes);
  CHECK_EQ(parsed.gpus[0].totalBytes, original.gpus[0].totalBytes);
}

EDGE_TEST(a_report_with_several_gpus_keeps_each_devices_own_numbers,
          "each gpu object is parsed separately instead of every device reading device 0") {
  HardwareReport original = sampleReport();
  GpuDevice second;
  second.name = "CUDA1";
  second.description = "NVIDIA A2";
  second.freeBytes = mbToBytes(11000);
  second.totalBytes = mbToBytes(16384);
  original.gpus.push_back(second);

  HardwareReport parsed;
  CHECK(parseHardwareReport(hardwareReportToJson(original), parsed));
  CHECK_EQ(parsed.gpus.size(), std::size_t{2});
  CHECK_EQ(parsed.gpus[0].name, std::string("CUDA0"));
  CHECK_EQ(parsed.gpus[1].name, std::string("CUDA1"));
  CHECK_EQ(parsed.gpus[1].freeBytes, mbToBytes(11000));
}

EDGE_TEST(a_report_with_no_gpu_is_valid_and_parses_to_an_empty_list,
          "a cpu-only machine round-trips as probeOk with zero gpus") {
  HardwareReport original;
  original.probeOk = true;
  original.note = "built without EDGE_USE_LLAMA, no gpu backend in this binary";
  original.ram.availableBytes = mbToBytes(4096);

  HardwareReport parsed;
  CHECK(parseHardwareReport(hardwareReportToJson(original), parsed));
  CHECK(parsed.probeOk);
  CHECK(parsed.gpus.empty());
  CHECK_EQ(parsed.note, original.note);
}

EDGE_TEST(a_description_containing_quotes_survives_the_round_trip,
          "a device description with quotes and backslashes does not break the frame") {
  HardwareReport original = sampleReport();
  original.gpus[0].description = "GPU \"fast\" \\ rev 2";

  HardwareReport parsed;
  CHECK(parseHardwareReport(hardwareReportToJson(original), parsed));
  CHECK_EQ(parsed.gpus.size(), std::size_t{1});
}

EDGE_TEST(malformed_probe_output_is_rejected_rather_than_half_read,
          "output the supervisor cannot parse fails outright, which it reads as no gpu") {
  HardwareReport parsed;
  CHECK(!parseHardwareReport("", parsed));
  CHECK(!parseHardwareReport("not json at all", parsed));
  CHECK(!parseHardwareReport("{\"note\":\"missing probeOk\",\"gpus\":[]}", parsed));
  CHECK(!parseHardwareReport("{\"probeOk\":true}", parsed));
  // A device with no memory numbers would be planned against as zero free vram forever.
  CHECK(!parseHardwareReport("{\"probeOk\":true,\"gpus\":[{\"name\":\"CUDA0\"}]}", parsed));
}

EDGE_TEST(a_truncated_probe_line_is_rejected,
          "a report cut off mid-array does not parse as a valid device list") {
  HardwareReport parsed;
  CHECK(!parseHardwareReport("{\"probeOk\":true,\"note\":\"\",\"gpus\":[{\"name\":\"CUD",
                             parsed));
}

EDGE_TEST(meminfo_reads_memtotal_and_memavailable,
          "readHostMemory takes MemAvailable, not MemFree, and converts kB to bytes") {
  const MeminfoFixture fixture(
      "MemTotal:       16316820 kB\n"
      "MemFree:          482164 kB\n"
      "MemAvailable:    9216000 kB\n"
      "Buffers:          123456 kB\n");

  const HostMemory memory = readHostMemory(fixture.path());
  CHECK_EQ(memory.totalBytes, static_cast<std::size_t>(16316820ull * 1024ull));
  CHECK_EQ(memory.availableBytes, static_cast<std::size_t>(9216000ull * 1024ull));
}

EDGE_TEST(meminfo_ignores_fields_whose_names_merely_start_the_same,
          "MemFree and MemAvailableFoo are not mistaken for MemAvailable") {
  const MeminfoFixture fixture(
      "MemFree:          482164 kB\n"
      "MemAvailable:    1048576 kB\n"
      "SwapFree:        2097152 kB\n");

  const HostMemory memory = readHostMemory(fixture.path());
  CHECK_EQ(memory.availableBytes, static_cast<std::size_t>(1048576ull * 1024ull));
  CHECK_EQ(memory.totalBytes, std::size_t{0});
}

EDGE_TEST(a_missing_meminfo_reports_nothing_rather_than_guessing,
          "an unreadable meminfo path yields zero available ram, which plans zero cpu workers") {
  const HostMemory memory = readHostMemory("/tmp/edge-meminfo-does-not-exist-12345");
  CHECK_EQ(memory.totalBytes, std::size_t{0});
  CHECK_EQ(memory.availableBytes, std::size_t{0});
}

EDGE_TEST(megabyte_conversion_truncates_rather_than_rounding,
          "bytesToMb floors, so a partly-used megabyte is never counted as available") {
  CHECK_EQ(bytesToMb(mbToBytes(2048)), 2048LL);
  CHECK_EQ(bytesToMb(mbToBytes(1) - 1), 0LL);
  CHECK_EQ(mbToBytes(-5), std::size_t{0});
  CHECK_EQ(mbToBytes(0), std::size_t{0});
}
