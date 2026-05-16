cmake_minimum_required(VERSION 3.18)

add_executable(edge-supervisor
  main.cpp
  supervisor.cpp
)

target_include_directories(edge-supervisor
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(edge-supervisor PRIVATE rt)
