set(root "${BUILD_DIR}/package-consumer")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/source")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${root}/prefix" RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Core installation failed")
endif()
file(RENAME "${root}/prefix" "${root}/relocated prefix")
file(WRITE "${root}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(consumer LANGUAGES CXX)
find_package(ArbiterX 1.1.0...<1.2.0 CONFIG REQUIRED)
get_target_property(api ArbiterX::core ARBITER_X_API_VERSION)
if(NOT api STREQUAL "1")
  message(FATAL_ERROR "Unexpected API generation")
endif()
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ArbiterX::core)
]=])
file(WRITE "${root}/source/main.cpp" [=[
#include <arbiter/world.hpp>
#include <arbiter/world_overrides.hpp>
int main() {
  arbiter::WorldConfig cfg; cfg.journal_events=false;
  cfg.horizon_ns=50'000'000;
  std::string error;
  if(!arbiter::apply_world_override(cfg,"fee_per_lot","0",error)) return 2;
  arbiter::World world(cfg,42); world.run();
  return world.state_hash()==12505037182163188822ull ? 0 : 1;
}
]=])
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/source" -B "${root}/build" "-DCMAKE_PREFIX_PATH=${root}/relocated prefix" "-DCMAKE_CXX_COMPILER=${COMPILER}" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed consumer configuration failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${root}/build" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed consumer build failed")
endif()
execute_process(COMMAND "${root}/build/consumer" WORKING_DIRECTORY "${root}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed consumer deterministic check failed")
endif()
