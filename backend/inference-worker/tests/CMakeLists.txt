# Standalone assert-based test binaries. No framework, no fetched dependency,
# and nothing here needs a model file, a GPU or a running stack.

add_executable(edge-device-tests
  testMain.cpp
  deviceLadderTests.cpp
  deviceBackendsTests.cpp
  ../deviceLadder.cpp
  ../deviceBackends.cpp
)

target_include_directories(edge-device-tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_compile_definitions(edge-device-tests
  PRIVATE EDGE_TEST_SUITE_NAME="device ladder and device backends"
)

# workerJsonTests.cpp includes worker.cpp so it can reach the anonymous
# namespace parsers. worker.cpp must not also be listed here.
add_executable(edge-worker-json-tests
  testMain.cpp
  workerJsonTests.cpp
  ../deviceLadder.cpp
  ../deviceBackends.cpp
  ../inferenceBackend.cpp
  ../remoteTransport.cpp
  ../backendRouter.cpp
  ../inferEngine.cpp
  ../gpuResourceOwner.cpp
)

target_include_directories(edge-worker-json-tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

# Built without EDGE_USE_LLAMA on purpose: the JSON frame parsing under test has
# nothing to do with the backend, and the mock engine keeps the suite fast.
target_compile_definitions(edge-worker-json-tests
  PRIVATE EDGE_TEST_SUITE_NAME="worker JSON frame parsing"
)

target_link_libraries(edge-worker-json-tests PRIVATE Threads::Threads rt)

# Hardware discovery, capacity planning and backend assignment. Every case here
# runs on a machine with no GPU, which is the point: the claim "a CPU worker
# never initializes CUDA" is an assertion about the environment the supervisor
# builds, not about a device being present.
#
# workerReassignment.cpp is the supervisor's own source, compiled straight into
# the test binary, so the cases drive the same function
# Supervisor::handleWorkerReassignment calls with only fork/exec injected.
add_executable(edge-hardware-tests
  testMain.cpp
  capacityPlanTests.cpp
  hardwareReportTests.cpp
  backendRouterTests.cpp
  resourceLifecycleTests.cpp
  fallbackContractTests.cpp
  capacityInputTests.cpp
  workerReassignmentTests.cpp
  hexagonRouteTests.cpp
  cudaInitTests.cpp
  ../deviceLadder.cpp
  ../deviceBackends.cpp
  ../inferenceBackend.cpp
  ../remoteTransport.cpp
  ../backendRouter.cpp
  ../inferEngine.cpp
  ../gpuResourceOwner.cpp
  ../../hardware/hardwareReport.cpp
  ../../hardware/capacityPlan.cpp
  ../../supervisor/workerReassignment.cpp
)

target_include_directories(edge-hardware-tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_compile_definitions(edge-hardware-tests
  PRIVATE EDGE_TEST_SUITE_NAME="hardware discovery, capacity and backend routing"
)

# Remote fallback and production recovery. Its own binary so the suites above
# stay untouched. Every remote case here drives an injected fake transport:
# nothing in this target opens a socket or contacts a real service.
add_executable(edge-remote-recovery-tests
  testMain.cpp
  remoteFallbackTests.cpp
  ../deviceLadder.cpp
  ../deviceBackends.cpp
  ../inferenceBackend.cpp
  ../remoteTransport.cpp
  ../backendRouter.cpp
  ../inferEngine.cpp
  ../gpuResourceOwner.cpp
)

target_include_directories(edge-remote-recovery-tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_compile_definitions(edge-remote-recovery-tests
  PRIVATE EDGE_TEST_SUITE_NAME="remote fallback and production recovery"
)

