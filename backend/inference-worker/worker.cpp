#include "worker.h"

#include "../ipc/exitCodes.h"
#include "../ipc/paths.h"
#include "../model-cache/model_cache.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <regex>
#include <utility>

namespace {

bool extractString(const std::string& json, const std::string& key, std::string& out) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return false;
  }
  std::string value = match[1].str();
  std::string unescaped;
  unescaped.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      ++i;
      switch (value[i]) {
        case 'n':
          unescaped.push_back('\n');
          break;
        case 'r':
          unescaped.push_back('\r');
          break;
        case 't':
          unescaped.push_back('\t');
          break;
        case '\\':
        case '"':
          unescaped.push_back(value[i]);
          break;
        default:
          unescaped.push_back(value[i]);
          break;
      }
    } else {
      unescaped.push_back(value[i]);
    }
  }
  out = std::move(unescaped);
  return true;
}

bool extractBool(const std::string& json, const std::string& key, bool defaultValue) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return defaultValue;
  }
  return match[1] == "true";
}

}  // namespace

Worker::Worker(WorkerConfig config) : config_(std::move(config)) {}

Worker::~Worker() {
  requestStop();
  stopHeartbeat();
  delete engine_;
  engine_ = nullptr;

  if (serverFd_ >= 0) {
    close(serverFd_);
    serverFd_ = -1;
  }
  if (!config_.socketPath.empty()) {
    unlink(config_.socketPath.c_str());
  }
  if (shmPtr_ != nullptr && shmSize_ > 0) {
    munmap(shmPtr_, shmSize_);
    shmPtr_ = nullptr;
  }
  if (shmFd_ >= 0) {
    close(shmFd_);
    shmFd_ = -1;
  }
}

bool Worker::init() {
  if (!attachSharedMemory()) {
    return false;
  }
  if (!selectDevice()) {
    return false;
  }

  InferConfig inferConfig;
  inferConfig.modelPath = config_.modelPath;
  inferConfig.gpuLayers = config_.gpuLayers;
  inferConfig.maxTokens = config_.maxTokens;
  inferConfig.temperature = config_.temperature;
  inferConfig.seed = config_.seed;
  // Testing "not cuda" would run an npu build's whole graph on the cpu while reporting npu.
  const std::string& startTier = router_->active();
  const bool tierOffloads =
      startTier == "cuda" ||
      (startTier == "npu" && QualcommHexagonBackend::hexagonCompiledIn());
  inferConfig.forceCpu = config_.forceCpu || !tierOffloads;

  engine_ = new InferEngine(inferConfig);
  if (!engine_->init()) {
    std::cerr << "[worker] failed to initialize inference engine\n";
    return false;
  }

  // Reported before the fallback hook is installed: the engine already settled on its tier.
  if (router_->active() == "cuda" && !engine_->isUsingGPU()) {
    router_->ladder().reportFault(DeviceFault::kUnavailable, "gpu model load failed at startup");
  }
  for (const std::string& tier : knownBackends()) {
    if (tierIsLlamaServed(tier)) {
      router_->registerBackend(std::make_shared<LlamaInferenceBackend>(tier, engine_));
    }
  }
  // The loop above skips npu, so the Qualcomm adapter still needs an engine to route into.
  if (hexagonBackend_) {
    hexagonBackend_->bindEngine(engine_);
  }
  router_->setFallbackHook([this](const std::string& previous, const std::string& next) {
    return onTierChanged(previous, next);
  });
  activeDevice_ = router_->active();
  cudaAvailable_.store(activeDevice_ == "cuda");

  if (!setupSocketServer()) {
    return false;
  }
  return true;
}

int Worker::run() {
  running_.store(true);
  startHeartbeat();

  while (running_.load()) {
    sockaddr_un clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    const int clientFd = accept(serverFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (clientFd < 0) {
      if (!running_.load()) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "[worker] accept failed: " << std::strerror(errno) << '\n';
      continue;
    }

    handleClient(clientFd);
    close(clientFd);
  }

  return exitCode_;
}

void Worker::requestStop() {
  running_.store(false);
  if (serverFd_ >= 0) {
    shutdown(serverFd_, SHUT_RDWR);
  }
}

bool Worker::attachSharedMemory() {
  const std::string metaName = EdgeIPC::shmMetaName(config_.shmName);
  const int metaFd = shm_open(metaName.c_str(), O_RDONLY, 0666);
  if (metaFd < 0) {
    std::cerr << "[worker] shm_open(" << metaName << ") failed: " << std::strerror(errno) << '\n';
    return false;
  }

  void* metaPtr = mmap(nullptr, sizeof(SharedModelHeader), PROT_READ, MAP_SHARED, metaFd, 0);
  if (metaPtr == MAP_FAILED) {
    close(metaFd);
    std::cerr << "[worker] metadata mmap failed: " << std::strerror(errno) << '\n';
    return false;
  }

  const auto* header = static_cast<const SharedModelHeader*>(metaPtr);
  const bool validHeader = std::memcmp(header->magic, "EDGE", 4) == 0 && header->ready == 1 &&
                           header->modelSize > 0;
  const std::size_t headerModelSize = static_cast<std::size_t>(header->modelSize);
  if (!validHeader) {
    munmap(metaPtr, sizeof(SharedModelHeader));
    close(metaFd);
    std::cerr << "[worker] shared model metadata is not ready\n";
    return false;
  }

  shmFd_ = shm_open(config_.shmName.c_str(), O_RDONLY, 0666);
  if (shmFd_ < 0) {
    munmap(metaPtr, sizeof(SharedModelHeader));
    close(metaFd);
    std::cerr << "[worker] shm_open(" << config_.shmName << ") failed: " << std::strerror(errno) << '\n';
    return false;
  }

  if (config_.modelSizeBytes > 0) {
    shmSize_ = config_.modelSizeBytes;
  } else {
    struct stat st {};
    if (fstat(shmFd_, &st) != 0) {
      munmap(metaPtr, sizeof(SharedModelHeader));
      close(metaFd);
      std::cerr << "[worker] fstat on shared memory failed: " << std::strerror(errno) << '\n';
      return false;
    }
    shmSize_ = static_cast<std::size_t>(st.st_size);
  }

  if (shmSize_ == 0 || shmSize_ != headerModelSize) {
    munmap(metaPtr, sizeof(SharedModelHeader));
    close(metaFd);
    std::cerr << "[worker] shared memory size mismatch: mapped=" << shmSize_
              << " metadata=" << headerModelSize << '\n';
    return false;
  }

  shmPtr_ = mmap(nullptr, shmSize_, PROT_READ, MAP_SHARED, shmFd_, 0);
  if (shmPtr_ == MAP_FAILED) {
    shmPtr_ = nullptr;
    munmap(metaPtr, sizeof(SharedModelHeader));
    close(metaFd);
    std::cerr << "[worker] mmap failed: " << std::strerror(errno) << '\n';
    return false;
  }

  const std::string sharedModelPath = EdgeIPC::shmFilePath(config_.shmName);
  if (access(sharedModelPath.c_str(), R_OK) == 0) {
    config_.modelPath = sharedModelPath;
  }

  std::cerr << "[worker] attached shared GGUF memory: " << config_.shmName << " (" << shmSize_
            << " bytes, checksum=" << header->checksum << ", path=" << config_.modelPath << ")\n";
  munmap(metaPtr, sizeof(SharedModelHeader));
  close(metaFd);
  return true;
}

bool Worker::selectDevice() {
  std::vector<std::string> order = config_.deviceLadder;
  if (config_.forceCpu || config_.assignedBackend == "cpu") {
    order = {"cpu"};
  }
  router_ = std::make_unique<BackendRouter>(order, config_.deviceQuarantineMs,
                                            config_.deviceProbeIntervalMs);
  hexagonBackend_ = std::make_shared<QualcommHexagonBackend>();
  router_->registerBackend(hexagonBackend_);
  router_->registerBackend(std::make_shared<CoreMlAneBackend>());
  // Safe unconfigured: it probes runtime_missing and select() skips it with a readable reason.
  router_->registerBackend(std::make_shared<RemoteInferenceBackend>(makeRemoteTransport()));

  if (!router_->select()) {
    std::cerr << "[worker] no usable device in ladder\n";
    return false;
  }
  activeDevice_ = router_->active();
  cudaAvailable_.store(activeDevice_ == "cuda");
  std::cerr << "[worker] backend=" << (config_.assignedBackend.empty()
                                           ? std::string("unassigned")
                                           : config_.assignedBackend)
            << " device ladder: " << router_->ladder().toJson() << '\n';
  return true;
}

bool Worker::onTierChanged(const std::string& previous, const std::string& next) {
  activeDevice_ = next;
  cudaAvailable_.store(next == "cuda");

  const DeviceLadder& ladder = router_->ladder();
  const bool recovering = ladder.indexOf(next) < ladder.indexOf(previous);

  if (recovering) {
    // Without this, a remote->cpu recovery would look like a cuda->cpu fallback and exit.
    if (!engine_->reloadOn(next == "cpu")) {
      std::cerr << "[worker] recovery previous=" << previous << " next=" << next
                << " cleanup=released error=reload_failed\n";
      return false;
    }
    std::cerr << "[worker] recovery previous=" << previous
              << " reason=health_check_passed cleanup=released_model_and_context next=" << next
              << " state=" << ladder.tierState(ladder.activeIndex()) << '\n';
    return true;
  }

  // Asked before the reload: this is about what the process already touched.
  const DeviceFallbackAction action = deviceFallbackAction(
      *engine_, previous, next, config_.reexecAfterDeviceClassFallback);
  const bool contextStaysResident = requiresProcessRestartForCpuOnly(*engine_, previous, next);

  if (!engine_->reloadOn(next == "cpu")) {
    std::cerr << "[worker] fallback previous=" << previous << " next=" << next
              << " cleanup=released error=reload_failed\n";
    return false;
  }
  // degradedReason is "<faultedTier>:<faultName>", set by reportFault just before this hook.
  const std::string& reason = router_->ladder().degradedReason();
  const std::size_t colon = reason.find(':');
  const std::string faultName =
      colon == std::string::npos ? std::string("unknown") : reason.substr(colon + 1);
  std::cerr << "[worker] fallback previous=" << previous << " error=" << faultName
            << " cleanup=released_model_and_context next=" << next << " state="
            << router_->ladder().tierState(router_->ladder().activeIndex()) << '\n';

  // reloadOn cannot free the CUDA primary context, so the only way to be cpu-only is to exit.
  if (contextStaysResident) {
    if (action == DeviceFallbackAction::kReexecAsCpu) {
      std::cerr << "[worker] device-class fallback " << previous
                << "->cpu: cuda primary context is still resident and cannot be released "
                   "in-process, exiting for reassignment as a cpu worker\n";
      exitCode_ = EdgeExit::kReassignCpu;
      // running_ alone would not wake the blocking accept; the in-flight reply is still written.
      requestStop();
    } else {
      std::cerr << "[worker] device-class fallback " << previous
                << "->cpu with re-exec disabled: this worker keeps a resident cuda primary "
                   "context for the rest of its life\n";
    }
  }
  return true;
}

std::string Worker::generateWithFallback(const std::string& prompt) {
  std::lock_guard<std::mutex> guard(engineMutex_);
  const RouteResult routed = router_->route(prompt, nullptr);
  activeDevice_ = router_->active();
  cudaAvailable_.store(activeDevice_ == "cuda");
  if (routed.ok()) {
    return routed.text;
  }
  return routed.text.empty() ? "[error: " + routed.detail + "]" : routed.text;
}

void Worker::streamWithFallback(const std::string& prompt,
                                const std::function<void(const std::string&)>& onToken,
                                std::string& merged) {
  std::lock_guard<std::mutex> guard(engineMutex_);
  const RouteResult routed = router_->route(prompt, onToken);
  activeDevice_ = router_->active();
  cudaAvailable_.store(activeDevice_ == "cuda");
  // Nothing synthetic is injected: a fabricated error token would read as model output.
  merged = routed.text;
}

// degraded means "we fell back", not "activeDevice_ happens to be cpu".
std::string Worker::deviceResultFields() const {
  const bool degraded = router_ && router_->ladder().degraded();
  std::string out = ",\"device\":\"" + activeDevice_ + "\",\"degraded\":" +
                    (degraded ? "true" : "false");
  if (degraded) {
    out += ",\"degradedReason\":\"" + jsonEscape(router_->ladder().degradedReason()) + "\"";
  }
  return out;
}

bool Worker::setupSocketServer() {
  if (config_.socketPath.empty()) {
    std::cerr << "[worker] socket path is empty\n";
    return false;
  }

  unlink(config_.socketPath.c_str());
  serverFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (serverFd_ < 0) {
    std::cerr << "[worker] socket creation failed: " << std::strerror(errno) << '\n';
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", config_.socketPath.c_str());
  if (bind(serverFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "[worker] bind failed for " << config_.socketPath << ": " << std::strerror(errno) << '\n';
    return false;
  }
  if (listen(serverFd_, 16) != 0) {
    std::cerr << "[worker] listen failed: " << std::strerror(errno) << '\n';
    return false;
  }

  std::cerr << "[worker] listening on " << config_.socketPath << " device=" << activeDevice_ << '\n';
  return true;
}

bool Worker::parseJob(const std::string& raw, InferenceJob& out, std::string& error) const {
  std::string type;
  if (!extractString(raw, "type", type) || type != "infer") {
    error = "expected type=infer";
    return false;
  }
  if (!extractString(raw, "requestId", out.requestId)) {
    error = "missing requestId";
    return false;
  }
  if (!extractString(raw, "prompt", out.prompt)) {
    error = "missing prompt";
    return false;
  }
  out.stream = extractBool(raw, "stream", false);
  return true;
}

void Worker::handleClient(int clientFd) {
  std::string payload;
  payload.reserve(2048);
  char buffer[2048];

  while (true) {
    const ssize_t n = read(clientFd, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "[worker] read failed: " << std::strerror(errno) << '\n';
      return;
    }
    if (n == 0) {
      break;
    }
    payload.append(buffer, static_cast<std::size_t>(n));
    if (payload.find('\n') != std::string::npos || payload.size() > 1024 * 1024) {
      break;
    }
  }

  const std::size_t lineEnd = payload.find('\n');
  if (lineEnd != std::string::npos) {
    payload.resize(lineEnd);
  }

  InferenceJob job;
  std::string parseError;
  if (!parseJob(payload, job, parseError)) {
    const std::string errorJson = "{\"type\":\"error\",\"requestId\":\"" +
                                  jsonEscape(job.requestId) + "\",\"error\":\"" +
                                  jsonEscape(parseError) + "\"}\n";
    sendAll(clientFd, errorJson);
    return;
  }

  if (engine_ == nullptr) {
    sendAll(clientFd, "{\"type\":\"error\",\"requestId\":\"" + jsonEscape(job.requestId) +
                          "\",\"error\":\"engine_not_initialized\"}\n");
    return;
  }

  if (!job.stream) {
    const std::string text = generateWithFallback(job.prompt);
    const std::string resultJson =
        "{\"type\":\"result\",\"requestId\":\"" + jsonEscape(job.requestId) + "\",\"text\":\"" +
        jsonEscape(text) + "\"" + deviceResultFields() + "}\n";
    sendAll(clientFd, resultJson);
    return;
  }

  bool writeOk = true;
  std::string merged;
  streamWithFallback(job.prompt, [&](const std::string& token) {
    if (!writeOk) {
      return;
    }
    const std::string tokenJson =
        "{\"type\":\"token\",\"requestId\":\"" + jsonEscape(job.requestId) + "\",\"token\":\"" +
        jsonEscape(token) + "\"}\n";
    writeOk = sendAll(clientFd, tokenJson);
  }, merged);
  if (!writeOk) {
    return;
  }

  const std::string doneJson =
      "{\"type\":\"result\",\"requestId\":\"" + jsonEscape(job.requestId) + "\",\"text\":\"" +
      jsonEscape(merged) + "\"" + deviceResultFields() + "}\n";
  sendAll(clientFd, doneJson);
}

void Worker::startHeartbeat() {
  heartbeatThread_ = std::thread(&Worker::heartbeatLoop, this);
}

void Worker::stopHeartbeat() {
  if (heartbeatThread_.joinable()) {
    heartbeatThread_.join();
  }
}

void Worker::heartbeatLoop() {
  int consecutiveFailures = 0;
  while (running_.load()) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", config_.supervisorSocketPath.c_str());
      if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        const std::string heartbeat =
            "{\"type\":\"heartbeat\",\"workerId\":" + std::to_string(config_.workerId) +
            ",\"status\":\"ready\",\"device\":\"" + activeDevice_ + "\"}\n";
        if (sendAll(fd, heartbeat)) {
          consecutiveFailures = 0;
        } else {
          ++consecutiveFailures;
        }
      } else {
        ++consecutiveFailures;
      }
      close(fd);
    } else {
      ++consecutiveFailures;
    }

    if (consecutiveFailures > 0 && consecutiveFailures % 100 == 0) {
      std::cerr << "[worker] heartbeat delivery failing (last error: " << std::strerror(errno) << ")\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeatIntervalMs));
  }
}

bool Worker::sendAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t n = send(fd, data.data() + sent, data.size() - sent, flags);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

std::string Worker::jsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (char ch : input) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
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
        out.push_back(ch);
        break;
    }
  }
  return out;
}
