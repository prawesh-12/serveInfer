#pragma once

#include "../model-cache/model_cache.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <unistd.h>

namespace EdgeIPC {

enum class ModelReadyState {
  kMalformed,
  kForeignRun,
  kNotReady,
  kReady,
};

// The shm objects outlive the run that made them, so ready alone cannot say whose flag it is.
// Only a header carrying this run's nonce may be trusted, whatever its ready byte says.
inline ModelReadyState evaluateModelHeader(const void* bytes, std::size_t size,
                                           std::uint64_t expectedNonce) {
  if (bytes == nullptr || size < sizeof(SharedModelHeader) || expectedNonce == 0) {
    return ModelReadyState::kMalformed;
  }

  SharedModelHeader header{};
  std::memcpy(&header, bytes, sizeof(header));
  if (std::memcmp(header.magic, "EDGE", 4) != 0) {
    return ModelReadyState::kMalformed;
  }
  if (header.runNonce != expectedNonce) {
    return ModelReadyState::kForeignRun;
  }
  // Pairs with the release fence the model-cache issues before it sets ready.
  std::atomic_thread_fence(std::memory_order_acquire);
  return header.ready == 1 ? ModelReadyState::kReady : ModelReadyState::kNotReady;
}

inline const char* modelReadyStateName(ModelReadyState state) {
  switch (state) {
    case ModelReadyState::kMalformed:
      return "malformed";
    case ModelReadyState::kForeignRun:
      return "foreign_run";
    case ModelReadyState::kNotReady:
      return "not_ready";
    case ModelReadyState::kReady:
      return "ready";
  }
  return "unknown";
}

// Never returns 0: a zeroed or truncated header reads as 0, and that must never match.
inline std::uint64_t generateRunNonce() {
  std::uint64_t nonce = 0;
  if (std::FILE* urandom = std::fopen("/dev/urandom", "rb")) {
    if (std::fread(&nonce, sizeof(nonce), 1, urandom) != 1) {
      nonce = 0;
    }
    std::fclose(urandom);
  }

  if (nonce == 0) {
    std::random_device entropy;
    std::mt19937_64 mix((static_cast<std::uint64_t>(entropy()) << 32) ^ entropy());
    mix.discard(static_cast<std::uint64_t>(getpid()));
    nonce = mix() ^ static_cast<std::uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count());
  }

  return nonce == 0 ? 1 : nonce;
}

}  // namespace EdgeIPC
