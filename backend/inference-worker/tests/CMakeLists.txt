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

add_executable(edge-model-cache-tests
  testMain.cpp
  modelCacheHandshakeTests.cpp
)

target_include_directories(edge-model-cache-tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_compile_definitions(edge-model-cache-tests
  PRIVATE EDGE_TEST_SUITE_NAME="model cache ready handshake"
)

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
  faultInjectionTests.cpp
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

