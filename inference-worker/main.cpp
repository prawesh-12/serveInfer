#include "worker.h"

#include "../ipc/paths.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
Worker* gWorker = nullptr;

void signalHandler(int) {
  if (gWorker != nullptr) {
    gWorker->requestStop();
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

bool parseSizeArg(const std::string& raw, std::size_t& out) {
  try {
    out = static_cast<std::size_t>(std::stoull(raw));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  } catch (const std::out_of_range&) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  WorkerConfig config;
  config.workerId = 0;
  config.socketPath = EdgeIPC::workerSock(config.workerId);
  config.supervisorSocketPath = EdgeIPC::SUPERVISOR_SOCK;
  config.shmName = EdgeIPC::SHM_NAME;
  config.heartbeatIntervalMs = 50;

  if (const char* forceCpu = std::getenv("EDGE_FORCE_CPU")) {
    config.forceCpu = std::string(forceCpu) == "1";
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
    if (takeValue("--worker-id", value)) {
      if (!parseIntArg(value, config.workerId)) {
        std::cerr << "Invalid --worker-id value: " << value << '\n';
        return 1;
      }
      continue;
    }
    if (takeValue("--socket-path", value)) {
      config.socketPath = value;
      continue;
    }
    if (takeValue("--supervisor-socket", value)) {
      config.supervisorSocketPath = value;
      continue;
    }
    if (takeValue("--shm-name", value)) {
      config.shmName = value;
      continue;
    }
    if (takeValue("--model-size-bytes", value)) {
      if (!parseSizeArg(value, config.modelSizeBytes)) {
        std::cerr << "Invalid --model-size-bytes value: " << value << '\n';
        return 1;
      }
      continue;
    }
    if (takeValue("--heartbeat-ms", value)) {
      if (!parseIntArg(value, config.heartbeatIntervalMs)) {
        std::cerr << "Invalid --heartbeat-ms value: " << value << '\n';
        return 1;
      }
      continue;
    }
    if (arg == "--force-cpu") {
      config.forceCpu = true;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << '\n';
    return 1;
  }

  if (config.socketPath.empty()) {
    config.socketPath = EdgeIPC::workerSock(config.workerId);
  }

  Worker worker(config);
  gWorker = &worker;
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  if (!worker.init()) {
    return 1;
  }
  return worker.run();
}
