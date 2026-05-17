#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct SharedModelHeader {
  char magic[8];
  std::uint64_t modelSize;
  std::uint64_t checksum;
  std::int64_t loadedAt;
  std::uint32_t version;
  std::uint8_t ready;
  std::uint8_t reserved[219];
};

static_assert(sizeof(SharedModelHeader) == 256, "SharedModelHeader must be 256 bytes");

struct ModelCacheConfig {
  std::string modelPath;
  std::string shmName;
};

class ModelCache {
 public:
  explicit ModelCache(ModelCacheConfig config);
  ~ModelCache();

  bool initialize();
  void requestStop();
  void waitUntilStopped() const;

 private:
  static std::uint64_t updateChecksum(std::uint64_t seed, const char* data, std::size_t len);

  bool openModelFile(std::size_t& modelSize);
  bool createSharedMemory(std::size_t modelSize);
  bool loadModelIntoSharedMemory(std::size_t modelSize);
  void cleanup();

  ModelCacheConfig config_;
  mutable std::atomic<bool> stopRequested_{false};

  int modelFd_ = -1;
  int shmFd_ = -1;
  int metaFd_ = -1;
  void* shmBase_ = nullptr;
  void* metaBase_ = nullptr;
  std::size_t shmSize_ = 0;
  std::size_t metaSize_ = 0;
};
