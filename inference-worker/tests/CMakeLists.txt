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
  ../inferEngine.cpp
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
