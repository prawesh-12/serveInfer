cmake_minimum_required(VERSION 3.18)

option(EDGE_ENABLE_LLAMA "Build worker with llama.cpp backend when available" ON)
set(EDGE_LLAMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp" CACHE PATH "Path to llama.cpp source")

find_package(Threads REQUIRED)

add_executable(edge-inference-worker
  main.cpp
  worker.cpp
)

target_include_directories(edge-inference-worker
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(edge-inference-worker PRIVATE Threads::Threads rt)

if(EDGE_ENABLE_LLAMA)
  if(EXISTS "${EDGE_LLAMA_DIR}/CMakeLists.txt")
    add_subdirectory("${EDGE_LLAMA_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/llama.cpp")
    if(TARGET llama)
      target_link_libraries(edge-inference-worker PRIVATE llama)
      target_compile_definitions(edge-inference-worker PRIVATE EDGE_USE_LLAMA=1)
    else()
      message(WARNING "llama.cpp added but 'llama' target not found. Building mock backend.")
    endif()
  else()
    message(WARNING "EDGE_ENABLE_LLAMA=ON but llama.cpp not found at ${EDGE_LLAMA_DIR}. Building mock backend.")
  endif()
endif()
