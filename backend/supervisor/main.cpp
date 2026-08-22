#include "supervisor.h"

#include "../ipc/paths.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
Supervisor* gSupervisor = nullptr;

void signalHandler(int) {
  if (gSupervisor != nullptr) {
    gSupervisor->requestStop();
  }
}

bool parseLongLongArg(const std::string& raw, long long& out) {
  try {
    out = std::stoll(raw);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  } catch (const std::out_of_range&) {
    return false;
  }
}

bool parseIntArg(const std::string& raw, int& out) {
  try {
    out = std::stoi(raw);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  } catch (const std::out_of_range&) {
    return false;
  }
}

void printUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " --model-path <path> [--worker-count N] [--model-cache-bin PATH]"
               " [--worker-bin PATH] [--api-bin PATH] [--api-entry PATH]"
               " [--shm-name NAME] [--supervisor-socket PATH] [--api-notify-socket PATH]\n";
}
}  // namespace

int main(int argc, char** argv) {
  SupervisorConfig config;
  config.shmName = EdgeIPC::shmName();
  config.supervisorSocketPath = EdgeIPC::supervisorSock();
  config.apiNotifySocketPath = EdgeIPC::apiNotifySock();

  if (const char* workerEnv = std::getenv("EDGE_WORKER_COUNT")) {
    parseIntArg(workerEnv, config.workerCount);
  }
  if (const char* modelEnv = std::getenv("EDGE_MODEL_PATH")) {
    config.modelPath = modelEnv;
  }

  // Optional, unlike the rest of the config: an .env missing these still boots on the defaults.
  if (const char* value = std::getenv("EDGE_GPU_RESERVE_MB")) {
    parseLongLongArg(value, config.capacity.gpuReserveMb);
  }
  if (const char* value = std::getenv("EDGE_WORKER_GPU_MB")) {
    parseLongLongArg(value, config.capacity.workerGpuMb);
  }
  if (const char* value = std::getenv("EDGE_RAM_RESERVE_MB")) {
    parseLongLongArg(value, config.capacity.ramReserveMb);
  }
  if (const char* value = std::getenv("EDGE_WORKER_RAM_MB")) {
    parseLongLongArg(value, config.capacity.workerRamMb);
  }
  if (const char* value = std::getenv("EDGE_HW_PROBE_TIMEOUT_MS")) {
    parseIntArg(value, config.hardwareProbeTimeoutMs);
  }
  if (const char* value = std::getenv("EDGE_WORKER_HEARTBEAT_GRACE_MS")) {
    parseLongLongArg(value, config.liveness.startupGraceMs);
  }
  if (const char* value = std::getenv("EDGE_WORKER_HEARTBEAT_TIMEOUT_MS")) {
    parseLongLongArg(value, config.liveness.heartbeatTimeoutMs);
  }
  if (const char* value = std::getenv("EDGE_WORKER_STUCK_REQUEST_MS")) {
    parseLongLongArg(value, config.liveness.stuckRequestMs);
  }
  if (const char* value = std::getenv("EDGE_MEMINFO_PATH")) {
    config.meminfoPath = value;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto takeValue = [&](const std::string& flag, std::string& dest) -> bool {
      if (arg.rfind(flag + "=", 0) == 0) {
        dest = arg.substr(flag.size() + 1);
        return true;
      }
      if (arg == flag && i + 1 < argc) {
        dest = argv[++i];
        return true;
      }
      return false;
    };

    std::string value;
    if (takeValue("--model-path", value)) {
      config.modelPath = value;
      continue;
    }
    if (takeValue("--worker-count", value)) {
      if (!parseIntArg(value, config.workerCount) || config.workerCount <= 0) {
        std::cerr << "Invalid --worker-count value: " << value << '\n';
        return 1;
      }
      continue;
    }
    if (takeValue("--model-cache-bin", value)) {
      config.modelCacheBinary = value;
      continue;
    }
    if (takeValue("--worker-bin", value)) {
      config.workerBinary = value;
      continue;
    }
    if (takeValue("--api-bin", value)) {
      config.apiBinary = value;
      continue;
    }
    if (takeValue("--api-entry", value)) {
      config.apiEntry = value;
      continue;
    }
    if (takeValue("--shm-name", value)) {
      config.shmName = value;
      continue;
    }
    if (takeValue("--supervisor-socket", value)) {
      config.supervisorSocketPath = value;
      continue;
    }
    if (takeValue("--api-notify-socket", value)) {
      config.apiNotifySocketPath = value;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << '\n';
    printUsage(argv[0]);
    return 1;
  }

  if (config.modelPath.empty()) {
    std::cerr << "--model-path is required (or set EDGE_MODEL_PATH)\n";
    printUsage(argv[0]);
    return 1;
  }
  if (config.shmName.empty()) {
    std::cerr << "--shm-name is required (or set EDGE_SHM_NAME)\n";
    printUsage(argv[0]);
    return 1;
  }
  if (config.supervisorSocketPath.empty()) {
    std::cerr << "--supervisor-socket is required (or set EDGE_SUPERVISOR_SOCK)\n";
    printUsage(argv[0]);
    return 1;
  }
  if (config.apiNotifySocketPath.empty()) {
    std::cerr << "--api-notify-socket is required (or set EDGE_API_NOTIFY_SOCK)\n";
    printUsage(argv[0]);
    return 1;
  }
  if (EdgeIPC::workerSockPrefix().empty()) {
    std::cerr << "EDGE_WORKER_SOCKET_PREFIX is required\n";
    return 1;
  }
  if (EdgeIPC::crashLog().empty()) {
    std::cerr << "EDGE_CRASH_LOG is required\n";
    return 1;
  }
  if (EdgeIPC::modelConfigPath().empty()) {
    std::cerr << "EDGE_MODEL_CONFIG_PATH is required\n";
    return 1;
  }

  Supervisor supervisor(config);
  gSupervisor = &supervisor;
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  if (!supervisor.start()) {
    return 1;
  }
  return supervisor.monitorLoop();
}
