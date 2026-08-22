#pragma once

#include <string>

#include "../hardware/hardwareReport.h"

// Initializes CUDA for the life of the process, so this runs only in the --probe-hardware child.
HardwareReport probeHardware(const std::string& meminfoPath);

int runHardwareProbe(const std::string& meminfoPath);
