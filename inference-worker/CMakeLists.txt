cmake_minimum_required(VERSION 3.18)

option(EDGE_ENABLE_LLAMA "Build worker with vendored llama backend" ON)
option(EDGE_ENABLE_CUDA "Enable CUDA backend for vendored llama" ON)

find_package(Threads REQUIRED)

add_executable(edge-inference-worker
  main.cpp
  worker.cpp
  inferEngine.cpp
)

target_include_directories(edge-inference-worker
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(edge-inference-worker PRIVATE Threads::Threads rt)

if(EDGE_ENABLE_LLAMA)
  set(EDGE_LLAMA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/llama-src")
  if(EXISTS "${EDGE_LLAMA_SRC_DIR}/CMakeLists.txt")
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
    set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    if(EDGE_ENABLE_CUDA)
      set(GGML_CUDA ON CACHE BOOL "" FORCE)
    endif()

    add_subdirectory("${EDGE_LLAMA_SRC_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/llama-src")
    if(TARGET llama)
      target_link_libraries(edge-inference-worker PRIVATE llama)
      target_compile_definitions(edge-inference-worker PRIVATE EDGE_USE_LLAMA=1)
      target_include_directories(edge-inference-worker
        PRIVATE
          "${EDGE_LLAMA_SRC_DIR}/include"
          "${EDGE_LLAMA_SRC_DIR}/ggml/include"
      )
    else()
      message(WARNING "vendored llama-src did not produce 'llama' target, using mock backend.")
    endif()
  else()
    message(WARNING "EDGE_ENABLE_LLAMA=ON but llama-src not found at ${EDGE_LLAMA_SRC_DIR}. Building mock backend.")
  endif()
endif()

