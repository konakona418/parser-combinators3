cmake_minimum_required(VERSION 3.20)
project(parser_combinators2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-freflection" PC_HAS_REFLECTION_FLAG)

add_library(parser_combinators2 INTERFACE)
add_library(parser_combinators2::parser_combinators2 ALIAS parser_combinators2)
target_include_directories(parser_combinators2 INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_compile_features(parser_combinators2 INTERFACE cxx_std_26)

if(PC_HAS_REFLECTION_FLAG)
    target_compile_options(parser_combinators2 INTERFACE -freflection)
else()
    message(WARNING
        "This compiler does not accept -freflection. "
        "GCC >= 16 or the clang-p2996 fork is required.")
endif()

option(PARSER_COMBINATORS2_BUILD_EXAMPLES "Build the examples" ON)
option(PARSER_COMBINATORS2_BUILD_TESTS "Build the tests" ON)
option(PARSER_COMBINATORS2_BUILD_BENCH "Build the benchmark if Google Benchmark is found" ON)

if(PARSER_COMBINATORS2_BUILD_EXAMPLES)
    add_executable(json_parser_example json_parser_example.cpp)
    target_link_libraries(json_parser_example PRIVATE parser_combinators2)
endif()

if(PARSER_COMBINATORS2_BUILD_TESTS)
    enable_testing()
    add_executable(parser_combinators2_tests tests/test_parser_combinators.cpp)
    target_link_libraries(parser_combinators2_tests PRIVATE parser_combinators2)
    add_test(NAME parser_combinators2_tests COMMAND parser_combinators2_tests)
endif()

if(PARSER_COMBINATORS2_BUILD_BENCH)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(parser_combinators2_bench bench.cpp)
        target_link_libraries(parser_combinators2_bench PRIVATE
            parser_combinators2 benchmark::benchmark)
    else()
        message(STATUS "Google Benchmark not found; skipping benchmark target")
    endif()
endif()
