cmake_minimum_required(VERSION 2.17)
project(duckpg LANGUAGES CXX)

# dependencies
add_subdirectory(third_party/pgwire)

# options
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# main project
include_directories(include)
add_subdirectory(src/duckpg)

# export
export(TARGETS pgwire asio endian promise NAMESPACE duckpg:: FILE DuckPGTargets.cmake)
