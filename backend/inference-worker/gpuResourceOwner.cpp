#include "gpuResourceOwner.h"

#include <string>

bool requiresProcessRestartForCpuOnly(const GpuResourceOwner& owner, const std::string& previous,
                                      const std::string& next) {
  if (previous == "cpu" || next != "cpu") {
    return false;
  }
  return owner.deviceResourcesResident();
}

DeviceFallbackAction deviceFallbackAction(const GpuResourceOwner& owner,
                                          const std::string& previous, const std::string& next,
                                          bool reexecEnabled) {
  if (!reexecEnabled) {
    return DeviceFallbackAction::kReloadInPlace;
  }
  return requiresProcessRestartForCpuOnly(owner, previous, next)
             ? DeviceFallbackAction::kReexecAsCpu
             : DeviceFallbackAction::kReloadInPlace;
}

bool deviceFallbackModeIsReexec(const char* rawMode) {
  if (rawMode == nullptr) {
    return true;
  }
  return std::string(rawMode) != "reload";
}
