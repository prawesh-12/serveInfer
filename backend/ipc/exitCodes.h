#pragma once

namespace EdgeExit {

// Planned exit, not a crash: process death is llama.cpp's only complete CUDA teardown.
constexpr int kReassignCpu = 70;

}  // namespace EdgeExit
