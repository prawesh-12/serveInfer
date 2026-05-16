cmake_minimum_required(VERSION 3.18)

add_executable(edge-model-cache
  main.cpp
  model_cache.cpp
)

target_include_directories(edge-model-cache
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(edge-model-cache PRIVATE rt)
