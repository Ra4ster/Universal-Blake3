if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are blocked to protect the custom Makefile. Please run 'cmake -B build' instead.")
endif()

cmake_minimum_required(VERSION 3.13)
project(Blake3 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED) # Multithreading
if (NOT MSVC)
    add_compile_options(-Wall -Wextra -march=native)
endif()

add_executable(Blake3 src/main.cpp) # Blake3 exe
target_include_directories(Blake3 PRIVATE include external/highway)
target_link_libraries(Blake3 PRIVATE Threads::Threads)

add_executable(Blake3_bench bench.cpp) # Benchmark
target_include_directories(Blake3_bench PRIVATE include external/highway)
target_link_libraries(Blake3_bench PRIVATE Threads::Threads)

add_custom_target(bench # Run with `cmake --build build --target bench`
    COMMAND bench
    DEPENDS Blake3_bench
    USES_TERMINAL
)
