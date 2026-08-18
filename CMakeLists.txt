cmake_minimum_required(VERSION 3.20)
project(trident LANGUAGES CXX VERSION 0.1.0)

# The library is meant to be embedded, so it must configure and build on a clean
# machine with nothing but a C++20 compiler and CMake. No package manager, no
# network access at configure time, no optional dependency that is not optional.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(TRIDENT_BUILD_TESTS "Build the test binary and register it with CTest" ON)
option(TRIDENT_BUILD_TOOLS "Build the demo and benchmark executables" ON)

add_library(trident STATIC
    src/term.cpp
    src/dictionary.cpp
    src/store.cpp
    src/lexer.cpp
    src/turtle.cpp
    src/sparql_ast.cpp
    src/sparql_parser.cpp
    src/expression.cpp
    src/planner.cpp
    src/exec.cpp
    src/rdfs.cpp
    src/query.cpp
    src/generator.cpp
)
target_include_directories(trident PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(MSVC)
    target_compile_options(trident PRIVATE /W4 /permissive-)
else()
    target_compile_options(trident PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TRIDENT_BUILD_TOOLS)
    add_executable(trident_demo tools/demo.cpp)
    target_link_libraries(trident_demo PRIVATE trident)

    add_executable(trident_bench tools/bench.cpp)
    target_link_libraries(trident_bench PRIVATE trident)

    add_executable(trident_cli tools/cli.cpp)
    target_link_libraries(trident_cli PRIVATE trident)

    # `cmake --build build --target demo` runs the demo straight after building it.
    add_custom_target(demo
        COMMAND $<TARGET_FILE:trident_demo>
        DEPENDS trident_demo
        USES_TERMINAL
        COMMENT "Running the Trident demo")
endif()

if(TRIDENT_BUILD_TESTS)
    enable_testing()
    add_executable(trident_tests
        tests/main.cpp
        tests/test_dictionary.cpp
        tests/test_turtle.cpp
        tests/test_store.cpp
        tests/test_sparql_parser.cpp
        tests/test_planner.cpp
        tests/test_exec.cpp
        tests/test_aggregate.cpp
        tests/test_rdfs.cpp
        tests/test_syntax_suite.cpp
        tests/test_demo_queries.cpp
    )
    target_link_libraries(trident_tests PRIVATE trident)
    target_include_directories(trident_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(trident_tests PRIVATE
        TRIDENT_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

    # One CTest entry per group. The binary filters its own registry by prefix,
    # so a group name is the only thing duplicated between here and the sources.
    foreach(group dictionary turtle store sparql_parser planner exec aggregate rdfs syntax_suite demo_queries)
        add_test(NAME ${group} COMMAND trident_tests ${group})
    endforeach()
endif()
