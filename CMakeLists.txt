cmake_minimum_required(VERSION 3.24)
project(arbiter_x VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ARBITER_BUILD_CLI "Build the core CLI" ${PROJECT_IS_TOP_LEVEL})
option(ARBITER_BUILD_BENCHMARKS "Build benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(ARBITER_BUILD_TESTS "Build core tests" ${PROJECT_IS_TOP_LEVEL})
option(ARBITER_ENABLE_INSTALL "Install core package" ${PROJECT_IS_TOP_LEVEL})
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
      target_compile_definitions(${target} PRIVATE ARBITER_ENABLE_SANITIZERS=1)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

set(ARBITER_CORE_SOURCES
  src/core/build_info.cpp
  src/core/world_overrides.cpp
  src/core/config.cpp
  src/core/crc32.cpp
  src/core/errors.cpp
  src/core/event.cpp
  src/core/histogram.cpp
  src/counterfactual/flagship.cpp
  src/counterfactual/latency.cpp
  src/exec/replay.cpp
  src/exec/world_config.cpp
  src/exec/world_state.cpp
  src/exec/world.cpp
  src/exec/world_snapshot.cpp
  src/journal/journal.cpp
  src/journal/snapshot.cpp
  src/orderbook/book_common.cpp
  src/orderbook/opt_book.cpp
  src/orderbook/ref_book.cpp
  src/profiling/metrics.cpp
  src/protocol/wire.cpp
  src/analysis/stats.cpp
  src/risk/risk.cpp
  src/sim/sim.cpp
  src/strategy/strategy.cpp
  src/verification/differential.cpp
)

include(GNUInstallDirs)

add_library(arbiter_core STATIC ${ARBITER_CORE_SOURCES})
add_library(ArbiterX::core ALIAS arbiter_core)
set_target_properties(arbiter_core PROPERTIES EXPORT_NAME core ARBITER_X_VERSION ${PROJECT_VERSION} ARBITER_X_API_VERSION 1 EXPORT_PROPERTIES "ARBITER_X_VERSION;ARBITER_X_API_VERSION")
target_compile_features(arbiter_core PUBLIC cxx_std_23)
target_include_directories(arbiter_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(arbiter_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_target(arbiter_build_info
  COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DVERSION=${PROJECT_VERSION}" "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/arbiter_build_info_config.hpp" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/arbiter_build_info_config.hpp
  VERBATIM)
add_dependencies(arbiter_core arbiter_build_info)
if(ARBITER_ENABLE_SANITIZERS AND NOT MSVC)
  target_link_options(arbiter_core INTERFACE -fsanitize=address,undefined)
endif()
arbiter_configure_target(arbiter_core)

if(ARBITER_BUILD_CLI)
add_executable(arbiter src/cli/main.cpp)
target_link_libraries(arbiter PRIVATE arbiter_core)
arbiter_configure_target(arbiter)
endif()

if(ARBITER_BUILD_BENCHMARKS)
add_executable(arbiter_bench benchmarks/bench_main.cpp)
target_link_libraries(arbiter_bench PRIVATE arbiter_core)
arbiter_configure_target(arbiter_bench)
endif()

if(PROJECT_IS_TOP_LEVEL OR ARBITER_BUILD_TESTS)
  include(CTest)
endif()
if(BUILD_TESTING AND ARBITER_BUILD_TESTS)
  target_compile_definitions(arbiter_core PRIVATE ARBITER_ENABLE_JOURNAL_TEST_HOOKS)
  set(ARBITER_TESTS
    test_core
    test_stats
    test_book
    test_diff
    test_journal
    test_world
    test_worldsnap
    test_flagship)

  set(AX_TEST_LABELS_test_stats "fast;unit")
  set(AX_TEST_LABELS_test_core "fast;unit")
  set(AX_TEST_LABELS_test_book "fast;unit")
  set(AX_TEST_LABELS_test_diff "stress;differential")
  set(AX_TEST_LABELS_test_journal "integration;recovery")
  set(AX_TEST_LABELS_test_world "integration")
  set(AX_TEST_LABELS_test_worldsnap "stress;snapshot")
  set(AX_TEST_LABELS_test_flagship "integration;research")

  foreach(test_name IN LISTS ARBITER_TESTS)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(${test_name} PRIVATE AX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(test_name STREQUAL "test_world")
      target_compile_definitions(${test_name} PRIVATE ARBITER_ENABLE_JOURNAL_TEST_HOOKS)
    endif()
    target_link_libraries(${test_name} PRIVATE arbiter_core)
    arbiter_configure_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "${AX_TEST_LABELS_${test_name}}")
  endforeach()

  if(ARBITER_BUILD_CLI)
    foreach(legacy IN ITEMS research experiment timetravel demo bench)
      add_test(NAME test_migrated_${legacy} COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:arbiter>" "-DARGS=${legacy}" -DEXPECTED=2 -DMESSAGE_PATTERN=Use -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/expect_exit.cmake)
    endforeach()
    add_test(NAME test_cli_help COMMAND arbiter --help)
    add_test(NAME test_cli_rejects_unknown COMMAND arbiter definitely-not-a-command)
    set_tests_properties(test_cli_help test_cli_rejects_unknown PROPERTIES LABELS "fast;cli")
    set_tests_properties(test_cli_rejects_unknown PROPERTIES WILL_FAIL TRUE)
  endif()
  if(ARBITER_BUILD_BENCHMARKS)
    add_test(NAME test_bench_help COMMAND arbiter_bench --help)
    add_test(NAME test_bench_rejects_unknown COMMAND arbiter_bench definitely-not-a-suite)
    set_tests_properties(test_bench_help test_bench_rejects_unknown PROPERTIES LABELS "fast;cli")
    set_tests_properties(test_bench_rejects_unknown PROPERTIES WILL_FAIL TRUE)
    foreach(bench_suite IN ITEMS exchange protocol journal pipeline)
      add_test(NAME test_bench_${bench_suite}_smoke COMMAND arbiter_bench ${bench_suite} --quick)
      set_tests_properties(test_bench_${bench_suite}_smoke PROPERTIES LABELS "benchmark;smoke")
    endforeach()
  endif()
endif()

if(ARBITER_ENABLE_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ArbiterX)
  install(TARGETS arbiter_core EXPORT ArbiterXTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  if(ARBITER_BUILD_CLI)
    install(TARGETS arbiter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  install(DIRECTORY include/arbiter DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT ArbiterXTargets NAMESPACE ArbiterX:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ArbiterX)
  configure_package_config_file(cmake/ArbiterXConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/ArbiterXConfig.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ArbiterX)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ArbiterXConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ArbiterXConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/ArbiterXConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ArbiterX)
endif()

if(BUILD_TESTING AND ARBITER_BUILD_TESTS AND ARBITER_ENABLE_INSTALL AND NOT ARBITER_ENABLE_SANITIZERS)
  add_test(NAME test_package_consumer COMMAND ${CMAKE_COMMAND}
    "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/package_consumer.cmake)
  set_tests_properties(test_package_consumer PROPERTIES LABELS "integration;package")
endif()
