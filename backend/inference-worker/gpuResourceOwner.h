#pragma once

#include <string>

// llama.cpp e85caa81 cannot free the CUDA primary context, so
// deviceResourcesResident() can stay true after a successful release.
class GpuResourceOwner {
 public:
  virtual ~GpuResourceOwner() = default;

  virtual bool releaseDeviceResources() = 0;
  virtual bool deviceResourcesResident() const = 0;
};

bool requiresProcessRestartForCpuOnly(const GpuResourceOwner& owner, const std::string& previous,
                                      const std::string& next);

enum class DeviceFallbackAction {
  kReloadInPlace,
  kReexecAsCpu,
};

DeviceFallbackAction deviceFallbackAction(const GpuResourceOwner& owner,
                                          const std::string& previous, const std::string& next,
                                          bool reexecEnabled);

// Only the exact string "reload" turns re-exec off; anything else keeps it on.
bool deviceFallbackModeIsReexec(const char* rawMode);
