#include "model_cache.h"

#include "../ipc/paths.h"

#include <csignal>
#include <iostream>
#include <string>

namespace {
ModelCache* gModelCache = nullptr;

void signalHandler(int) {
  if (gModelCache != nullptr) {
    gModelCache->requestStop();
  }
}

void printUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " --model-path <path> [--shm-name <name>]\n";
}
}  // namespace

int main(int argc, char** argv) {
  ModelCacheConfig config;
  config.shmName = EdgeIPC::shmName();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--model-path" && i + 1 < argc) {
      config.modelPath = argv[++i];
      continue;
    }
    if (arg.rfind("--model-path=", 0) == 0) {
      config.modelPath = arg.substr(std::string("--model-path=").size());
      continue;
    }
    if (arg == "--shm-name" && i + 1 < argc) {
      config.shmName = argv[++i];
      continue;
    }
    if (arg.rfind("--shm-name=", 0) == 0) {
      config.shmName = arg.substr(std::string("--shm-name=").size());
      continue;
    }

    std::cerr << "Unknown argument: " << arg << '\n';
    printUsage(argv[0]);
    return 1;
  }

  if (config.modelPath.empty()) {
    std::cerr << "--model-path is required\n";
    printUsage(argv[0]);
    return 1;
  }
  if (config.shmName.empty()) {
    std::cerr << "--shm-name is required (or set EDGE_SHM_NAME)\n";
    printUsage(argv[0]);
    return 1;
  }

  ModelCache cache(config);
  gModelCache = &cache;
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  if (!cache.initialize()) {
    return 1;
  }

  cache.waitUntilStopped();
  return 0;
}
