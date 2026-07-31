cmake_minimum_required(VERSION 3.16)
project(cpusim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(cpusim_core
  src/trace.cpp
  src/isa.cpp
  src/assembler.cpp
  src/memory.cpp
  src/pipeline.cpp
  src/scheduler.cpp
  src/simulator.cpp
  src/report.cpp
  src/stats.cpp
)
target_include_directories(cpusim_core PUBLIC include)

add_executable(cpusim src/main.cpp)
target_link_libraries(cpusim PRIVATE cpusim_core)

enable_testing()
add_executable(run_tests tests/test_sim.cpp)
target_link_libraries(run_tests PRIVATE cpusim_core)
add_test(NAME unit COMMAND run_tests)
