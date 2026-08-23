#include "hardwareProbe.h"

#if defined(EDGE_USE_LLAMA)
#include "ggml-backend.h"
#include "llama.h"
#endif

#include <cstdio>
#include <string>

HardwareReport probeHardware(const std::string& meminfoPath) {
  HardwareReport report;
  report.ram = readHostMemory(meminfoPath);

#if defined(EDGE_USE_LLAMA)
  llama_backend_init();

  const std::size_t deviceCount = ggml_backend_dev_count();
  for (std::size_t i = 0; i < deviceCount; ++i) {
    ggml_backend_dev_t device = ggml_backend_dev_get(i);
    if (device == nullptr) {
      continue;
    }
    const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
    if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
      // ggml's CPU device reports free == total on Linux, sizing the pool off total RAM.
      continue;
    }

    GpuDevice gpu;
    const char* name = ggml_backend_dev_name(device);
    const char* description = ggml_backend_dev_description(device);
    gpu.name = name != nullptr ? name : "";
    gpu.description = description != nullptr ? description : "";
    // cudaMemGetInfo underneath, so it accounts for other processes on the card.
    ggml_backend_dev_memory(device, &gpu.freeBytes, &gpu.totalBytes);
    report.gpus.push_back(std::move(gpu));
  }

  report.probeOk = true;
  report.note = report.gpus.empty()
                    ? "ggml registered " + std::to_string(deviceCount) +
                          " device(s), none of them a gpu"
                    : "ggml registered " + std::to_string(report.gpus.size()) + " gpu device(s)";
#else
  report.probeOk = true;
  report.note = "built without EDGE_USE_LLAMA, no gpu backend in this binary";
#endif

  return report;
}

int runHardwareProbe(const std::string& meminfoPath) {
  const HardwareReport report = probeHardware(meminfoPath);
  // The supervisor parses exactly this one stdout line; everything else goes to stderr.
  std::printf("%s\n", hardwareReportToJson(report).c_str());
  std::fflush(stdout);
  return 0;
}
