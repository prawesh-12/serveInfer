#include "capacityPlan.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace {

std::string jsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const char ch : input) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += ch;
    }
  }
  return out;
}

int clampToRange(long long value, int ceiling) {
  if (value <= 0) {
    return 0;
  }
  if (value > ceiling) {
    return ceiling;
  }
  return static_cast<int>(value);
}

// A zero budget is a plausible .env typo, and dividing by it is undefined.
bool budgetUsable(long long budgetMb) {
  return budgetMb > 0;
}

// A negative reserve would subtract backwards and over-assign workers.
long long reserveOrZero(long long reserveMb) {
  return reserveMb > 0 ? reserveMb : 0;
}

// CUDA_VISIBLE_DEVICES takes the driver ordinal in the ggml device name, not the report position.
int deviceOrdinalFromName(const std::string& name, int fallback) {
  std::size_t start = name.size();
  while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9') {
    --start;
  }
  if (start == name.size()) {
    return fallback;
  }
  long long ordinal = 0;
  for (std::size_t i = start; i < name.size(); ++i) {
    ordinal = ordinal * 10 + (name[i] - '0');
    // Keeps a long digit run in a vendor string from overflowing the accumulator.
    if (ordinal > 1024) {
      return fallback;
    }
  }
  return static_cast<int>(ordinal);
}

}  // namespace

const char* workerBackendName(WorkerBackend backend) {
  switch (backend) {
    case WorkerBackend::kCuda:
      return "cuda";
    case WorkerBackend::kCpu:
      return "cpu";
  }
  return "cpu";
}

CapacityPlan planCapacity(const HardwareReport& report, const CapacityLimits& limits) {
  CapacityPlan plan;
  const int ceiling = std::max(0, limits.maxWorkers);

  if (!report.probeOk) {
    plan.gpuReason = "hardware probe did not report: " +
                     (report.note.empty() ? std::string("no detail") : report.note);
  } else if (report.gpus.empty()) {
    plan.gpuReason = "no gpu device registered in this build";
  } else {
    const GpuDevice* best = &report.gpus.front();
    std::size_t bestPosition = 0;
    for (std::size_t i = 1; i < report.gpus.size(); ++i) {
      if (report.gpus[i].freeBytes > best->freeBytes) {
        best = &report.gpus[i];
        bestPosition = i;
      }
    }
    plan.gpuName = best->description.empty() ? best->name : best->description;
    plan.gpuIndex = deviceOrdinalFromName(best->name, static_cast<int>(bestPosition));
    plan.totalVramMb = bytesToMb(best->totalBytes);
    plan.freeVramMb = bytesToMb(best->freeBytes);
    plan.usableGpuMb = std::max(0LL, plan.freeVramMb - reserveOrZero(limits.gpuReserveMb));

    if (!budgetUsable(limits.workerGpuMb)) {
      plan.gpuReason = "EDGE_WORKER_GPU_MB is not a positive budget";
    } else if (plan.usableGpuMb < limits.workerGpuMb) {
      plan.gpuReason = "usable vram " + std::to_string(plan.usableGpuMb) +
                       "MB is below the per-worker budget " +
                       std::to_string(limits.workerGpuMb) + "MB";
    } else {
      plan.gpuWorkerCapacity = clampToRange(plan.usableGpuMb / limits.workerGpuMb, ceiling);
      plan.gpuReason = "free " + std::to_string(plan.freeVramMb) + "MB - reserve " +
                       std::to_string(limits.gpuReserveMb) + "MB = " +
                       std::to_string(plan.usableGpuMb) + "MB usable / " +
                       std::to_string(limits.workerGpuMb) + "MB per worker";
    }
  }

  const int cpuCeiling = std::max(0, ceiling - plan.gpuWorkerCapacity);
  plan.totalRamMb = bytesToMb(report.ram.totalBytes);
  plan.availableRamMb = bytesToMb(report.ram.availableBytes);
  plan.usableRamMb = std::max(0LL, plan.availableRamMb - reserveOrZero(limits.ramReserveMb));

  if (report.ram.availableBytes == 0) {
    plan.cpuReason = "available ram unknown, assuming none";
  } else if (!budgetUsable(limits.workerRamMb)) {
    plan.cpuReason = "EDGE_WORKER_RAM_MB is not a positive budget";
  } else {
    plan.cpuWorkerCapacity = clampToRange(plan.usableRamMb / limits.workerRamMb, cpuCeiling);
    plan.cpuReason = "available " + std::to_string(plan.availableRamMb) + "MB - reserve " +
                     std::to_string(limits.ramReserveMb) + "MB = " +
                     std::to_string(plan.usableRamMb) + "MB usable / " +
                     std::to_string(limits.workerRamMb) + "MB per worker";
  }

  return plan;
}

int placeableWorkerCount(const CapacityPlan& plan, int configuredCount) {
  if (configuredCount <= 0) {
    return 0;
  }
  const int placeable = plan.gpuWorkerCapacity + plan.cpuWorkerCapacity;
  if (placeable >= configuredCount) {
    return configuredCount;
  }
  return placeable < 1 ? 1 : placeable;
}

std::vector<WorkerAssignment> assignWorkers(const CapacityPlan& plan, int workerCount) {
  std::vector<WorkerAssignment> assignments;
  if (workerCount <= 0) {
    return assignments;
  }
  assignments.reserve(static_cast<std::size_t>(workerCount));

  for (int workerId = 0; workerId < workerCount; ++workerId) {
    WorkerAssignment assignment;
    assignment.workerId = workerId;
    if (workerId < plan.gpuWorkerCapacity) {
      assignment.backend = WorkerBackend::kCuda;
      assignment.gpuIndex = plan.gpuIndex;
      assignment.reason = "gpu slot " + std::to_string(workerId + 1) + " of " +
                          std::to_string(plan.gpuWorkerCapacity) + " (" + plan.gpuReason + ")";
    } else {
      assignment.backend = WorkerBackend::kCpu;
      assignment.reason = plan.gpuWorkerCapacity == 0
                              ? "no gpu slot: " + plan.gpuReason
                              : "gpu capacity " + std::to_string(plan.gpuWorkerCapacity) +
                                    " already filled";
    }
    assignments.push_back(std::move(assignment));
  }
  return assignments;
}

std::string capacityPlanToJson(const CapacityPlan& plan) {
  std::string out = "{";
  out += "\"gpuWorkerCapacity\":" + std::to_string(plan.gpuWorkerCapacity);
  out += ",\"cpuWorkerCapacity\":" + std::to_string(plan.cpuWorkerCapacity);
  out += ",\"totalVramMb\":" + std::to_string(plan.totalVramMb);
  out += ",\"freeVramMb\":" + std::to_string(plan.freeVramMb);
  out += ",\"usableGpuMb\":" + std::to_string(plan.usableGpuMb);
  out += ",\"totalRamMb\":" + std::to_string(plan.totalRamMb);
  out += ",\"availableRamMb\":" + std::to_string(plan.availableRamMb);
  out += ",\"usableRamMb\":" + std::to_string(plan.usableRamMb);
  out += ",\"gpuName\":\"" + jsonEscape(plan.gpuName) + "\"";
  out += ",\"gpuIndex\":" + std::to_string(plan.gpuIndex);
  out += ",\"gpuReason\":\"" + jsonEscape(plan.gpuReason) + "\"";
  out += ",\"cpuReason\":\"" + jsonEscape(plan.cpuReason) + "\"";
  out += "}";
  return out;
}

std::string workerAssignmentsToJson(const std::vector<WorkerAssignment>& assignments) {
  std::string out = "[";
  for (std::size_t i = 0; i < assignments.size(); ++i) {
    const WorkerAssignment& assignment = assignments[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"workerId\":" + std::to_string(assignment.workerId);
    out += ",\"backend\":\"" + std::string(workerBackendName(assignment.backend)) + "\"";
    out += ",\"gpuIndex\":" + std::to_string(assignment.gpuIndex);
    out += ",\"reason\":\"" + jsonEscape(assignment.reason) + "\"";
    out += "}";
  }
  out += "]";
  return out;
}

std::vector<std::pair<std::string, std::string>> workerBackendEnv(
    const WorkerAssignment& assignment) {
  std::vector<std::pair<std::string, std::string>> env;
  env.emplace_back("EDGE_WORKER_BACKEND", workerBackendName(assignment.backend));
  if (assignment.backend == WorkerBackend::kCpu) {
    // The NVIDIA driver reads this below ggml, and truncates the list at the first invalid entry.
    env.emplace_back("CUDA_VISIBLE_DEVICES", "-1");
  } else {
    env.emplace_back("CUDA_VISIBLE_DEVICES", std::to_string(assignment.gpuIndex));
  }
  return env;
}

WorkerAssignment demoteToCpu(const WorkerAssignment& assignment, const std::string& reason) {
  WorkerAssignment demoted = assignment;
  demoted.backend = WorkerBackend::kCpu;
  demoted.reason = reason;
  return demoted;
}
