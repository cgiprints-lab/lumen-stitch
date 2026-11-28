cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(lumen_longshot_tests LANGUAGES CXX)

add_executable(gen_tiles_cpp gen_tiles_cpp.cpp)
target_include_directories(gen_tiles_cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../common)
