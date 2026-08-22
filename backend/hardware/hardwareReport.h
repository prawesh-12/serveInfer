#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct GpuDevice {
  std::string name;
  std::string description;
  std::size_t freeBytes = 0;
  std::size_t totalBytes = 0;
};

struct HostMemory {
  std::size_t totalBytes = 0;
  std::size_t availableBytes = 0;
};

struct HardwareReport {
  // Excludes ggml's CPU device, whose free/total is useless (see readHostMemory).
  std::vector<GpuDevice> gpus;
  HostMemory ram;
  std::string note;
  bool probeOk = false;
};

std::string hardwareReportToJson(const HardwareReport& report);

// All-or-nothing: `out` is untouched unless the whole report parses.
bool parseHardwareReport(const std::string& json, HardwareReport& out);

// MemAvailable, not ggml's CPU dev_memory, which reports free == total on Linux.
HostMemory readHostMemory(const std::string& meminfoPath);

std::size_t mbToBytes(long long mb);
long long bytesToMb(std::size_t bytes);
