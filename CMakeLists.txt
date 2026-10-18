cmake_minimum_required(VERSION 3.24)
project(arbiter_x VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ARBITER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ARBITER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(arbiter_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(ARBITER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(ARBITER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(ARBITER_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

set(ARBITER_CORE_SOURCES
  src/core/config.cpp
  src/core/crc32.cpp
  src/core/errors.cpp
  src/core/event.cpp
  src/core/histogram.cpp
  src/counterfactual/experiment.cpp
  src/counterfactual/flagship.cpp
  src/counterfactual/latency.cpp
  src/data/data.cpp
  src/exec/replay.cpp
  src/exec/world.cpp
  src/exec/world_snapshot.cpp
  src/journal/journal.cpp
  src/journal/snapshot.cpp
  src/orderbook/book_common.cpp
  src/orderbook/opt_book.cpp
  src/orderbook/ref_book.cpp
  src/profiling/metrics.cpp
  src/protocol/wire.cpp
  src/research/execution_research.cpp
  src/research/features.cpp
  src/research/labels.cpp
  src/research/pipeline.cpp
  src/research/quant_model.cpp
  src/research/research.cpp
  src/research/stats.cpp
  src/risk/risk.cpp
  src/sim/sim.cpp
  src/strategy/strategy.cpp
  src/verification/differential.cpp
)

add_library(arbiter_core STATIC ${ARBITER_CORE_SOURCES})
target_include_directories(arbiter_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
arbiter_configure_target(arbiter_core)

add_executable(arbiter src/cli/main.cpp src/cli/research_cli.cpp)
target_link_libraries(arbiter PRIVATE arbiter_core)
arbiter_configure_target(arbiter)

add_executable(arbiter_bench benchmarks/bench_main.cpp)
target_link_libraries(arbiter_bench PRIVATE arbiter_core)
arbiter_configure_target(arbiter_bench)

include(CTest)
if(BUILD_TESTING)
  set(ARBITER_TESTS
    test_core
    test_book
    test_diff
    test_journal
    test_world
    test_worldsnap
    test_spsc
    test_quant
    test_flagship)

  set(AX_TEST_LABELS_test_core "fast;unit")
  set(AX_TEST_LABELS_test_book "fast;unit")
  set(AX_TEST_LABELS_test_diff "stress;differential")
  set(AX_TEST_LABELS_test_journal "integration;recovery")
  set(AX_TEST_LABELS_test_world "integration")
  set(AX_TEST_LABELS_test_worldsnap "stress;snapshot")
  set(AX_TEST_LABELS_test_spsc "fast;concurrency")
  set(AX_TEST_LABELS_test_quant "fast;research")
  set(AX_TEST_LABELS_test_flagship "integration;research")

  foreach(test_name IN LISTS ARBITER_TESTS)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(${test_name} PRIVATE AX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${test_name} PRIVATE arbiter_core)
    arbiter_configure_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "${AX_TEST_LABELS_${test_name}}")
  endforeach()

  add_test(NAME test_cli_help COMMAND arbiter --help)
  add_test(NAME test_bench_help COMMAND arbiter_bench --help)
  add_test(NAME test_cli_rejects_unknown COMMAND arbiter definitely-not-a-command)
  add_test(NAME test_bench_rejects_unknown COMMAND arbiter_bench definitely-not-a-suite)
  foreach(bench_suite IN ITEMS exchange protocol journal pipeline)
    add_test(NAME test_bench_${bench_suite}_smoke COMMAND arbiter_bench ${bench_suite} --quick)
    set_tests_properties(test_bench_${bench_suite}_smoke PROPERTIES LABELS "benchmark;smoke")
  endforeach()
  set_tests_properties(
    test_cli_help test_bench_help test_cli_rejects_unknown test_bench_rejects_unknown
    PROPERTIES LABELS "fast;cli")
  set_tests_properties(
    test_cli_rejects_unknown test_bench_rejects_unknown
    PROPERTIES WILL_FAIL TRUE)
endif()