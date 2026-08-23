#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// runNonce and the padding before it come out of the old reserved[219]; the header is still
// 256 bytes, so a build that predates the nonce reads the same offsets for everything else.
struct SharedModelHeader {
  char magic[8];
  std::uint64_t modelSize;
  std::uint64_t checksum;
  std::int64_t loadedAt;
  std::uint32_t version;
  std::uint8_t ready;
  std::uint8_t pad[3];
  std::uint64_t runNonce;
  std::uint8_t reserved[208];
};

static_assert(sizeof(SharedModelHeader) == 256, "SharedModelHeader must be 256 bytes");
static_assert(offsetof(SharedModelHeader, runNonce) == 40, "runNonce must stay at offset 40");

constexpr std::uint32_t kSharedModelHeaderVersion = 2;

struct ModelCacheConfig {
  std::string modelPath;
  std::string shmName;
  std::uint64_t runNonce = 0;
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
  void claimHeader();
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
