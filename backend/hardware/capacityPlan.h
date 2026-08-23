#pragma once

#include <string>
#include <utility>
#include <vector>

#include "hardwareReport.h"

struct CapacityLimits {
  int maxWorkers = 4;
  long long gpuReserveMb = 512;
  // Not the GGUF size: the weights are one shared /dev/shm copy, so this is context, KV and compute.
  long long workerGpuMb = 2048;
  long long ramReserveMb = 1024;
  long long workerRamMb = 1024;
};

struct CapacityPlan {
  int gpuWorkerCapacity = 0;
  int cpuWorkerCapacity = 0;

  long long totalVramMb = 0;
  long long freeVramMb = 0;
  long long usableGpuMb = 0;
  long long totalRamMb = 0;
  long long availableRamMb = 0;
  long long usableRamMb = 0;

  std::string gpuName;
  int gpuIndex = 0;
  std::string gpuReason;
  std::string cpuReason;
};

CapacityPlan planCapacity(const HardwareReport& report, const CapacityLimits& limits);

// The supervisor is the only process that probes; these go into the model-config file.
std::string capacityPlanToJson(const CapacityPlan& plan);

enum class WorkerBackend {
  kCuda,
  kCpu,
};

const char* workerBackendName(WorkerBackend backend);

struct WorkerAssignment {
  int workerId = 0;
  WorkerBackend backend = WorkerBackend::kCpu;
  int gpuIndex = 0;
  std::string reason;
};

// configuredCount is a ceiling, not a promise; the floor is one.
int placeableWorkerCount(const CapacityPlan& plan, int configuredCount);

std::vector<WorkerAssignment> assignWorkers(const CapacityPlan& plan, int workerCount);

std::string workerAssignmentsToJson(const std::vector<WorkerAssignment>& assignments);

std::vector<std::pair<std::string, std::string>> workerBackendEnv(
    const WorkerAssignment& assignment);

WorkerAssignment demoteToCpu(const WorkerAssignment& assignment, const std::string& reason);
