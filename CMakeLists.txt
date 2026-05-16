cmake_minimum_required(VERSION 3.18)
project(edge_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(inference-worker)
add_subdirectory(model-cache)
add_subdirectory(supervisor)
