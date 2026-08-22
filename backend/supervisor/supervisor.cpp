#include "supervisor.h"

#include "workerReassignment.h"

#include "../ipc/modelReady.h"
#include "../ipc/paths.h"
#include "../model-cache/model_cache.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <thread>
#include <utility>

namespace {
constexpr int kCircuitKeyModelCache = -1001;
constexpr int kCircuitKeyApiServer = -1002;

std::string nowEpochSeconds() {
  using Clock = std::chrono::system_clock;
  const auto now = Clock::now().time_since_epoch();
  return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

bool sendAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = write(fd, data.data() + sent, data.size() - sent);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}
}  // namespace

bool CircuitBreaker::registerCrash(int workerId) {
  const auto now = std::chrono::steady_clock::now();
  auto& queue = history_[workerId];
  queue.push_back(now);
  pruneLocked(workerId, now);
  return queue.size() >= kCrashThreshold;
}

void CircuitBreaker::pruneLocked(int workerId, std::chrono::steady_clock::time_point now) {
  auto it = history_.find(workerId);
  if (it == history_.end()) {
    return;
  }
  auto& queue = it->second;
  const auto minAllowed = now - kWindow;
  while (!queue.empty() && queue.front() < minAllowed) {
    queue.pop_front();
  }
}

Supervisor::Supervisor(SupervisorConfig config) : config_(std::move(config)) {}

Supervisor::~Supervisor() {
  requestStop();
  shutdownChildren();
  cleanupSocket();
}

bool Supervisor::start() {
  // Before any fork: every backend decision must be made while nothing can race for VRAM.
  discoverHardware();

  if (!setupSupervisorSocket()) {
    return false;
  }

  running_.store(true);
  if (!startModelCache()) {
    return false;
  }
  writeModelConfig();
  if (!waitForModelReady()) {
    std::cerr << "[supervisor] model cache ready flag was not observed\n";
    return false;
  }
  if (!startApiServer()) {
    return false;
  }
  if (!startWorkers()) {
    return false;
  }
  return true;
}

int Supervisor::monitorLoop() {
  while (running_.load()) {
    reapChildren();
    drainSupervisorSocket();
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.pollIntervalMs));
  }

  shutdownChildren();
  cleanupSocket();
  return 0;
}

void Supervisor::requestStop() {
  running_.store(false);
}

HardwareReport Supervisor::runHardwareProbeChild() const {
  HardwareReport report;
  report.ram = readHostMemory(config_.meminfoPath);

  int pipeFds[2] = {-1, -1};
  if (pipe(pipeFds) != 0) {
    report.note = std::string("pipe for hardware probe failed: ") + std::strerror(errno);
    return report;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipeFds[0]);
    close(pipeFds[1]);
    report.note = std::string("fork for hardware probe failed: ") + std::strerror(errno);
    return report;
  }

  if (pid == 0) {
    close(pipeFds[0]);
    dup2(pipeFds[1], STDOUT_FILENO);
    close(pipeFds[1]);
    const std::string binary = config_.workerBinary;
    const std::string flag = "--probe-hardware";
    char* argv[] = {const_cast<char*>(binary.c_str()), const_cast<char*>(flag.c_str()), nullptr};
    execvp(argv[0], argv);
    _exit(127);
  }

  close(pipeFds[1]);
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::max(0, config_.hardwareProbeTimeoutMs));
  bool timedOut = false;
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      timedOut = true;
      break;
    }
    pollfd waiting{};
    waiting.fd = pipeFds[0];
    waiting.events = POLLIN;
    const int ready = poll(&waiting, 1, static_cast<int>(remaining.count()));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      timedOut = true;
      break;
    }
    char buffer[4096];
    const ssize_t n = read(pipeFds[0], buffer, sizeof(buffer));
    if (n > 0) {
      output.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0 || errno != EINTR) {
      break;
    }
  }
  close(pipeFds[0]);

  if (timedOut) {
    kill(pid, SIGKILL);
  }
  int status = 0;
  waitpid(pid, &status, 0);

  if (timedOut) {
    report.note = "hardware probe timed out after " +
                  std::to_string(config_.hardwareProbeTimeoutMs) + "ms";
    return report;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    report.note = "hardware probe exited " + statusToReason(status);
    return report;
  }

  HardwareReport parsed;
  if (!parseHardwareReport(output, parsed)) {
    report.note = "hardware probe emitted output this build cannot parse";
    return report;
  }
  // Overwrites the child's own reading: only this one can be pointed at a test fixture.
  parsed.ram = report.ram;
  return parsed;
}

void Supervisor::discoverHardware() {
  hardware_ = runHardwareProbeChild();
  config_.capacity.maxWorkers = config_.workerCount;
  plan_ = planCapacity(hardware_, config_.capacity);
  effectiveWorkerCount_ = placeableWorkerCount(plan_, config_.workerCount);
  assignments_ = assignWorkers(plan_, effectiveWorkerCount_);

  std::cerr << "[supervisor] discovery probeOk=" << (hardware_.probeOk ? "true" : "false")
            << " gpus=" << hardware_.gpus.size() << " note=\"" << hardware_.note << "\"\n";
  for (std::size_t i = 0; i < hardware_.gpus.size(); ++i) {
    const GpuDevice& gpu = hardware_.gpus[i];
    std::cerr << "[supervisor] discovery gpu position=" << i << " name=" << gpu.name
              << " description=\"" << gpu.description
              << "\" totalVramMb=" << bytesToMb(gpu.totalBytes)
              << " freeVramMb=" << bytesToMb(gpu.freeBytes) << '\n';
  }
  std::cerr << "[supervisor] discovery gpuReserveMb=" << config_.capacity.gpuReserveMb
            << " workerGpuMb=" << config_.capacity.workerGpuMb
            << " usableGpuMb=" << plan_.usableGpuMb << " chosenGpu=\"" << plan_.gpuName
            << "\" cudaVisibleDevice=" << plan_.gpuIndex
            << " gpuWorkerCapacity=" << plan_.gpuWorkerCapacity << " reason=\"" << plan_.gpuReason
            << "\"\n";
  std::cerr << "[supervisor] discovery ramTotalMb=" << plan_.totalRamMb
            << " ramAvailableMb=" << plan_.availableRamMb
            << " ramReserveMb=" << config_.capacity.ramReserveMb
            << " workerRamMb=" << config_.capacity.workerRamMb
            << " cpuWorkerCapacity=" << plan_.cpuWorkerCapacity << " reason=\"" << plan_.cpuReason
            << "\"\n";

  const int placeable = plan_.gpuWorkerCapacity + plan_.cpuWorkerCapacity;
  std::cerr << "[supervisor] discovery placeableWorkers=" << placeable << " configuredWorkers="
            << config_.workerCount << " startingWorkers=" << effectiveWorkerCount_ << '\n';
  if (effectiveWorkerCount_ < config_.workerCount) {
    std::cerr << "[supervisor] discovery capacity holds " << placeable << " worker(s), so "
              << (config_.workerCount - effectiveWorkerCount_)
              << " of the " << config_.workerCount
              << " configured will not be started\n";
  }
  if (placeable < 1) {
    std::cerr << "[supervisor] discovery WARNING: the plan affords no worker at all; starting one "
                 "anyway so the runtime can answer, but it is running below its budget\n";
  }

  for (const WorkerAssignment& assignment : assignments_) {
    std::cerr << "[supervisor] assignment workerId=" << assignment.workerId
              << " backend=" << workerBackendName(assignment.backend) << " reason=\""
              << assignment.reason << "\"\n";
  }
}

bool Supervisor::setupSupervisorSocket() {
  if (!config_.supervisorSocketPath.empty()) {
    unlink(config_.supervisorSocketPath.c_str());
  }

  supervisorServerFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (supervisorServerFd_ < 0) {
    std::cerr << "[supervisor] socket creation failed: " << std::strerror(errno) << '\n';
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", config_.supervisorSocketPath.c_str());
  if (bind(supervisorServerFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "[supervisor] bind failed for " << config_.supervisorSocketPath << ": "
              << std::strerror(errno) << '\n';
    return false;
  }

  if (listen(supervisorServerFd_, 32) != 0) {
    std::cerr << "[supervisor] listen failed: " << std::strerror(errno) << '\n';
    return false;
  }

  const int flags = fcntl(supervisorServerFd_, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(supervisorServerFd_, F_SETFL, flags | O_NONBLOCK);
  }
  return true;
}

bool Supervisor::startModelCache() {
  runNonce_ = EdgeIPC::generateRunNonce();
  const std::vector<std::string> args = {
      config_.modelCacheBinary,
      "--model-path",
      config_.modelPath,
      "--shm-name",
      config_.shmName,
      "--run-nonce",
      std::to_string(runNonce_),
  };
  const pid_t pid = forkExec(args);
  if (pid <= 0) {
    return false;
  }
  ProcessInfo info;
  info.pid = pid;
  info.type = ProcessType::kModelCache;
  info.crashLog = EdgeIPC::crashLog();
  info.command = args;
  writePidFile(pidFileNameFor(info), pid);
  processesByPid_[pid] = std::move(info);
  std::cerr << "[supervisor] started model-cache pid=" << pid << " runNonce=" << runNonce_ << '\n';
  return true;
}

bool Supervisor::waitForModelReady() const {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(config_.modelReadyTimeoutSeconds);
  const std::string metaName = EdgeIPC::shmMetaName(config_.shmName);
  bool reportedForeignRun = false;

  while (running_.load() && std::chrono::steady_clock::now() < deadline) {
    EdgeIPC::ModelReadyState state = EdgeIPC::ModelReadyState::kMalformed;

    const int fd = shm_open(metaName.c_str(), O_RDONLY, 0666);
    if (fd >= 0) {
      struct stat st {};
      if (fstat(fd, &st) == 0 && st.st_size >= static_cast<off_t>(sizeof(SharedModelHeader))) {
        void* mapped = mmap(nullptr, sizeof(SharedModelHeader), PROT_READ, MAP_SHARED, fd, 0);
        if (mapped != MAP_FAILED) {
          state = EdgeIPC::evaluateModelHeader(mapped, sizeof(SharedModelHeader), runNonce_);
          munmap(mapped, sizeof(SharedModelHeader));
        }
      }
      close(fd);
    }

    if (state == EdgeIPC::ModelReadyState::kReady) {
      return true;
    }
    if (state == EdgeIPC::ModelReadyState::kForeignRun && !reportedForeignRun) {
      reportedForeignRun = true;
      std::cerr << "[supervisor] ignoring shared model metadata from another run, waiting for "
                << "runNonce=" << runNonce_ << '\n';
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool Supervisor::startApiServer() {
  const std::vector<std::string> args = {
      config_.apiBinary,
      config_.apiEntry,
      "--supervisor-socket",
      config_.apiNotifySocketPath,
  };
  const pid_t pid = forkExec(args);
  if (pid <= 0) {
    return false;
  }
  ProcessInfo info;
  info.pid = pid;
  info.type = ProcessType::kApiServer;
  info.socketPath = config_.apiNotifySocketPath;
  info.crashLog = EdgeIPC::crashLog();
  info.command = args;
  writePidFile(pidFileNameFor(info), pid);
  processesByPid_[pid] = std::move(info);
  std::cerr << "[supervisor] started api-server pid=" << pid << '\n';
  return true;
}

bool Supervisor::startWorkers() {
  for (int workerId = 0; workerId < effectiveWorkerCount_; ++workerId) {
    if (!startWorker(workerId)) {
      return false;
    }
  }
  return true;
}

bool Supervisor::startWorker(int workerId) {
  const std::vector<std::string> args = {
      config_.workerBinary,
      "--worker-id",
      std::to_string(workerId),
      "--socket-path",
      EdgeIPC::workerSock(workerId),
      "--supervisor-socket",
      config_.supervisorSocketPath,
      "--shm-name",
      config_.shmName,
      "--model-path",
      config_.modelPath,
  };

  std::vector<std::pair<std::string, std::string>> env;
  const WorkerAssignment* assignment = assignmentFor(workerId);
  if (assignment != nullptr) {
    env = workerBackendEnv(*assignment);
  }

  const pid_t pid = forkExec(args, env);
  if (pid <= 0) {
    return false;
  }

  ProcessInfo info;
  info.pid = pid;
  info.type = ProcessType::kWorker;
  info.workerId = workerId;
  info.socketPath = EdgeIPC::workerSock(workerId);
  info.crashLog = EdgeIPC::crashLog();
  info.command = args;
  writePidFile(pidFileNameFor(info), pid);
  processesByPid_[pid] = std::move(info);
  workerPidById_[workerId] = pid;
  std::cerr << "[supervisor] started worker " << workerId << " pid=" << pid << " backend="
            << (assignment != nullptr ? workerBackendName(assignment->backend) : "unassigned")
            << '\n';
  return true;
}

const WorkerAssignment* Supervisor::assignmentFor(int workerId) const {
  for (const WorkerAssignment& assignment : assignments_) {
    if (assignment.workerId == workerId) {
      return &assignment;
    }
  }
  return nullptr;
}

pid_t Supervisor::forkExec(const std::vector<std::string>& args,
                           const std::vector<std::pair<std::string, std::string>>& env) const {
  if (args.empty()) {
    return -1;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "[supervisor] fork failed: " << std::strerror(errno) << '\n';
    return -1;
  }
  if (pid == 0) {
    for (const auto& entry : env) {
      setenv(entry.first.c_str(), entry.second.c_str(), 1);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    std::cerr << "[supervisor] exec failed for " << args[0] << ": " << std::strerror(errno) << '\n';
    _exit(127);
  }
  return pid;
}

void Supervisor::reapChildren() {
  while (true) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0) {
      break;
    }
    handleCrash(pid, status);
  }
}

void Supervisor::handleCrash(pid_t pid, int status) {
  const auto it = processesByPid_.find(pid);
  if (it == processesByPid_.end()) {
    return;
  }

  const ProcessInfo info = it->second;
  processesByPid_.erase(it);
  removePidFile(pidFileNameFor(info));
  if (info.type == ProcessType::kWorker && info.workerId >= 0) {
    workerPidById_.erase(info.workerId);
  }

  const bool exitedNormally = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (exitedNormally) {
    return;
  }

  if (handleWorkerReassignment(info, status)) {
    return;
  }

  const std::string reason = statusToReason(status);
  writeCrashLog(info, status, reason);

  if (!running_.load()) {
    return;
  }

  if (info.type == ProcessType::kWorker) {
    if (!notifyApiServerWorkerCrash(info.workerId)) {
      std::cerr << "[supervisor] failed to notify api-server for worker crash workerId="
                << info.workerId << '\n';
    }
    const bool open = circuitBreaker_.registerCrash(info.workerId) || crashLimitOpenFromDisk(info);
    if (open) {
      std::cerr << "[supervisor] Worker " << info.workerId << " circuit breaker OPEN\n";
      return;
    }
    if (startWorker(info.workerId)) {
      notifyApiServerWorkerRestarted(info.workerId);
    }
    return;
  }

  if (info.type == ProcessType::kModelCache) {
    const bool open = circuitBreaker_.registerCrash(kCircuitKeyModelCache) || crashLimitOpenFromDisk(info);
    if (open) {
      std::cerr << "[supervisor] model-cache circuit breaker OPEN\n";
      return;
    }
    if (startModelCache()) {
      if (waitForModelReady()) {
        restartWorkersAfterModelCacheRestart();
      }
    }
    return;
  }

  if (info.type == ProcessType::kApiServer) {
    const bool open = circuitBreaker_.registerCrash(kCircuitKeyApiServer) || crashLimitOpenFromDisk(info);
    if (open) {
      std::cerr << "[supervisor] api-server circuit breaker OPEN\n";
      return;
    }
    startApiServer();
  }
}

// false means "not handled": the caller falls through to ordinary crash handling.
bool Supervisor::handleWorkerReassignment(const ProcessInfo& info, int status) {
  if (info.type != ProcessType::kWorker) {
    return false;
  }
  const bool isReassignExit =
      WIFEXITED(status) && WEXITSTATUS(status) == EdgeExit::kReassignCpu;

  ReassignmentHooks hooks;
  hooks.startWorker = [this](int workerId) { return startWorker(workerId); };
  hooks.notifyCrash = [this](int workerId) { notifyApiServerWorkerCrash(workerId); };
  hooks.notifyRestarted = [this](int workerId) { notifyApiServerWorkerRestarted(workerId); };

  const ReassignmentOutcome outcome = applyWorkerReassignment(
      assignments_, info.workerId, isReassignExit, running_.load(), hooks, std::cerr);

  return outcome == ReassignmentOutcome::kRestarted;
}

void Supervisor::shutdownChildren() {
  if (processesByPid_.empty()) {
    return;
  }

  for (const auto& entry : processesByPid_) {
    kill(entry.first, SIGTERM);
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!processesByPid_.empty() && std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid > 0) {
      processesByPid_.erase(pid);
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  for (const auto& entry : processesByPid_) {
    kill(entry.first, SIGKILL);
    waitpid(entry.first, nullptr, 0);
  }
  for (const auto& entry : processesByPid_) {
    removePidFile(pidFileNameFor(entry.second));
  }
  removePidFile("backend-model-cache");
  removePidFile("backend-api-server");
  for (int workerId = 0; workerId < config_.workerCount; ++workerId) {
    removePidFile("backend-worker-" + std::to_string(workerId));
  }
  processesByPid_.clear();
  workerPidById_.clear();
}

void Supervisor::restartWorkersAfterModelCacheRestart() {
  for (int workerId = 0; workerId < config_.workerCount; ++workerId) {
    const auto pidIt = workerPidById_.find(workerId);
    if (pidIt == workerPidById_.end()) {
      continue;
    }
    const pid_t pid = pidIt->second;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    removePidFile("backend-worker-" + std::to_string(workerId));
    processesByPid_.erase(pid);
    workerPidById_.erase(pidIt);
  }

  // The planned count, not the configured ceiling: capacity outlives a model-cache restart.
  for (int workerId = 0; workerId < effectiveWorkerCount_; ++workerId) {
    if (workerPidById_.find(workerId) == workerPidById_.end()) {
      if (startWorker(workerId)) {
        notifyApiServerWorkerRestarted(workerId);
      }
    }
  }
}

void Supervisor::cleanupSocket() {
  if (supervisorServerFd_ >= 0) {
    close(supervisorServerFd_);
    supervisorServerFd_ = -1;
  }
  if (!config_.supervisorSocketPath.empty()) {
    unlink(config_.supervisorSocketPath.c_str());
  }
}

void Supervisor::drainSupervisorSocket() {
  if (supervisorServerFd_ < 0) {
    return;
  }

  while (true) {
    const int clientFd = accept(supervisorServerFd_, nullptr, nullptr);
    if (clientFd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      return;
    }

    char buffer[2048];
    while (true) {
      const ssize_t n = read(clientFd, buffer, sizeof(buffer));
      if (n > 0) {
        continue;
      }
      if (n == 0 || (n < 0 && errno != EINTR)) {
        break;
      }
    }
    close(clientFd);
  }
}

void Supervisor::writeCrashLog(const ProcessInfo& info, int status, const std::string& reason) const {
  std::ofstream out(info.crashLog.empty() ? EdgeIPC::crashLog() : info.crashLog, std::ios::app);
  if (!out.is_open()) {
    return;
  }
  out << "{\"ts\":" << nowEpochSeconds() << ",\"pid\":" << info.pid << ",\"type\":\""
      << processTypeToString(info.type) << "\",\"workerId\":" << info.workerId
      << ",\"status\":" << status << ",\"reason\":\"" << jsonEscape(reason) << "\"}\n";
}

void Supervisor::writeModelConfig() const {
  std::ofstream out(EdgeIPC::modelConfigPath(), std::ios::trunc);
  if (!out.is_open()) {
    return;
  }
  out << "{\"modelPath\":\"" << jsonEscape(config_.modelPath) << "\",\"shmName\":\""
      << jsonEscape(config_.shmName) << "\",\"workerCount\":" << effectiveWorkerCount_
      << ",\"configuredWorkerCount\":" << config_.workerCount
      << ",\"pollIntervalMs\":" << config_.pollIntervalMs
      << ",\"hardware\":" << hardwareReportToJson(hardware_)
      << ",\"capacity\":" << capacityPlanToJson(plan_)
      << ",\"assignments\":" << workerAssignmentsToJson(assignments_) << "}\n";
}

bool Supervisor::crashLimitOpenFromDisk(const ProcessInfo& info) const {
  std::ifstream in(info.crashLog.empty() ? EdgeIPC::crashLog() : info.crashLog);
  if (!in.is_open()) {
    return false;
  }

  const auto now = std::stoll(nowEpochSeconds());
  const auto minTs = now - 60;
  const std::string type = processTypeToString(info.type);
  const std::regex tsPattern("\"ts\"\\s*:\\s*([0-9]+)");
  const std::regex typePattern("\"type\"\\s*:\\s*\"" + type + "\"");
  const std::regex workerPattern("\"workerId\"\\s*:\\s*" + std::to_string(info.workerId));

  int count = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::smatch match;
    if (!std::regex_search(line, match, tsPattern)) {
      continue;
    }
    const auto ts = std::stoll(match[1].str());
    if (ts < minTs) {
      continue;
    }
    if (!std::regex_search(line, typePattern)) {
      continue;
    }
    if (info.type == ProcessType::kWorker && !std::regex_search(line, workerPattern)) {
      continue;
    }
    ++count;
  }

  return count >= 3;
}

bool Supervisor::notifyApiServer(const char* type, int workerId) const {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", config_.apiNotifySocketPath.c_str());
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return false;
  }
  const std::string msg = std::string("{\"type\":\"") + type + "\",\"workerId\":" +
                          std::to_string(workerId) + ",\"requestId\":\"\"}\n";
  const bool ok = sendAll(fd, msg);
  close(fd);
  return ok;
}

bool Supervisor::notifyApiServerWorkerCrash(int workerId) const {
  return notifyApiServer("worker_crashed", workerId);
}

// Without this the api-server's pool gives up probing and never takes the worker back.
bool Supervisor::notifyApiServerWorkerRestarted(int workerId) const {
  return notifyApiServer("worker_restarted", workerId);
}

std::string Supervisor::pidFileNameFor(const ProcessInfo& info) {
  switch (info.type) {
    case ProcessType::kModelCache:
      return "backend-model-cache";
    case ProcessType::kApiServer:
      return "backend-api-server";
    case ProcessType::kWorker:
      return "backend-worker-" + std::to_string(info.workerId);
  }
  return {};
}

void Supervisor::writePidFile(const std::string& name, pid_t pid) {
  const std::string path = EdgeIPC::pidFilePath(name);
  if (path.empty()) {
    return;
  }
  std::ofstream out(path, std::ios::trunc);
  if (out) {
    out << pid << '\n';
  }
}

void Supervisor::removePidFile(const std::string& name) {
  const std::string path = EdgeIPC::pidFilePath(name);
  if (!path.empty()) {
    unlink(path.c_str());
  }
}

std::string Supervisor::processTypeToString(ProcessType type) {
  switch (type) {
    case ProcessType::kModelCache:
      return "model-cache";
    case ProcessType::kApiServer:
      return "api-server";
    case ProcessType::kWorker:
      return "worker";
  }
  return "unknown";
}

std::string Supervisor::jsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
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

std::string Supervisor::statusToReason(int status) {
  if (WIFEXITED(status)) {
    return "exit_" + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return "signal_" + std::to_string(WTERMSIG(status));
  }
  if (WIFSTOPPED(status)) {
    return "stopped_" + std::to_string(WSTOPSIG(status));
  }
  return "unknown";
}
