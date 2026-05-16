#include "model_cache.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace {
constexpr std::size_t kHeaderSize = sizeof(SharedModelHeader);
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
}  // namespace

ModelCache::ModelCache(ModelCacheConfig config) : config_(std::move(config)) {}

ModelCache::~ModelCache() {
  cleanup();
}

bool ModelCache::initialize() {
  std::size_t modelSize = 0;
  if (!openModelFile(modelSize)) {
    return false;
  }

  const std::size_t totalSize = kHeaderSize + modelSize;
  if (!createSharedMemory(totalSize)) {
    return false;
  }

  if (!loadModelIntoSharedMemory(modelSize)) {
    return false;
  }

  std::cerr << "[model-cache] ready, shared memory: " << config_.shmName << '\n';
  return true;
}

void ModelCache::requestStop() {
  stopRequested_.store(true);
}

void ModelCache::waitUntilStopped() const {
  while (!stopRequested_.load()) {
    pause();
  }
}

std::uint64_t ModelCache::updateChecksum(std::uint64_t seed, const char* data, std::size_t len) {
  std::uint64_t hash = seed;
  for (std::size_t i = 0; i < len; ++i) {
    hash ^= static_cast<std::uint8_t>(data[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

bool ModelCache::openModelFile(std::size_t& modelSize) {
  modelFd_ = open(config_.modelPath.c_str(), O_RDONLY);
  if (modelFd_ < 0) {
    std::cerr << "[model-cache] failed to open model file " << config_.modelPath << ": "
              << std::strerror(errno) << '\n';
    return false;
  }

  struct stat st {};
  if (fstat(modelFd_, &st) != 0) {
    std::cerr << "[model-cache] fstat failed for " << config_.modelPath << ": "
              << std::strerror(errno) << '\n';
    return false;
  }

  if (st.st_size <= 0) {
    std::cerr << "[model-cache] model file is empty: " << config_.modelPath << '\n';
    return false;
  }

  modelSize = static_cast<std::size_t>(st.st_size);
  return true;
}

bool ModelCache::createSharedMemory(std::size_t totalSize) {
  shmFd_ = shm_open(config_.shmName.c_str(), O_CREAT | O_RDWR, 0666);
  if (shmFd_ < 0) {
    std::cerr << "[model-cache] shm_open failed for " << config_.shmName << ": " << std::strerror(errno)
              << '\n';
    return false;
  }

  if (ftruncate(shmFd_, static_cast<off_t>(totalSize)) != 0) {
    std::cerr << "[model-cache] ftruncate failed: " << std::strerror(errno) << '\n';
    return false;
  }

  shmBase_ = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
  if (shmBase_ == MAP_FAILED) {
    shmBase_ = nullptr;
    std::cerr << "[model-cache] mmap failed: " << std::strerror(errno) << '\n';
    return false;
  }

  shmSize_ = totalSize;
  std::memset(shmBase_, 0, shmSize_);
  return true;
}

bool ModelCache::loadModelIntoSharedMemory(std::size_t modelSize) {
  auto* header = reinterpret_cast<SharedModelHeader*>(shmBase_);
  std::memset(header, 0, sizeof(SharedModelHeader));
  std::memcpy(header->magic, "EDGE", 4);
  header->version = 1;
  header->modelSize = static_cast<std::uint64_t>(modelSize);
  header->ready = 0;

  auto* weights = reinterpret_cast<std::uint8_t*>(shmBase_) + kHeaderSize;
  std::vector<char> buffer(1 << 20);
  std::size_t written = 0;
  std::uint64_t checksum = kFnvOffsetBasis;

  while (written < modelSize) {
    const std::size_t need = std::min(buffer.size(), modelSize - written);
    const ssize_t n = read(modelFd_, buffer.data(), need);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "[model-cache] read failed: " << std::strerror(errno) << '\n';
      return false;
    }
    if (n == 0) {
      std::cerr << "[model-cache] unexpected EOF while reading model\n";
      return false;
    }

    std::memcpy(weights + written, buffer.data(), static_cast<std::size_t>(n));
    checksum = updateChecksum(checksum, buffer.data(), static_cast<std::size_t>(n));
    written += static_cast<std::size_t>(n);
  }

  header->checksum = checksum;
  header->loadedAt = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  header->ready = 1;

  if (msync(shmBase_, shmSize_, MS_SYNC) != 0) {
    std::cerr << "[model-cache] msync failed: " << std::strerror(errno) << '\n';
    return false;
  }

  std::cerr << "[model-cache] loaded model bytes: " << modelSize << '\n';
  return true;
}

void ModelCache::cleanup() {
  if (shmBase_ != nullptr && shmSize_ > 0) {
    munmap(shmBase_, shmSize_);
    shmBase_ = nullptr;
    shmSize_ = 0;
  }

  if (shmFd_ >= 0) {
    close(shmFd_);
    shmFd_ = -1;
  }

  if (modelFd_ >= 0) {
    close(modelFd_);
    modelFd_ = -1;
  }

  if (!config_.shmName.empty()) {
    if (shm_unlink(config_.shmName.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "[model-cache] shm_unlink failed for " << config_.shmName << ": "
                << std::strerror(errno) << '\n';
    }
  }
}
