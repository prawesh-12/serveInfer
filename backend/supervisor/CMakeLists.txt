cmake_minimum_required(VERSION 3.18)

# The supervisor deliberately does not link llama: enumerating devices through
# ggml costs the calling process a CUDA primary context it can never give back.
add_executable(edge-supervisor
  main.cpp
  supervisor.cpp
  workerReassignment.cpp
  ../hardware/hardwareReport.cpp
  ../hardware/capacityPlan.cpp
)

target_include_directories(edge-supervisor
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(edge-supervisor PRIVATE rt)
