cmake_minimum_required(VERSION 3.16)
project(cpusim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CPUSIM_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(CPUSIM_WERROR "Treat compiler warnings as errors" OFF)
option(CPUSIM_LIBFUZZER "Build the libFuzzer target (requires Clang)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CPUSIM_WERROR)
  add_compile_options(-Werror)
endif()
if(CPUSIM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

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
add_test(NAME unit COMMAND run_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cli COMMAND ${CMAKE_COMMAND} -E env BIN=$<TARGET_FILE:cpusim>
         ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cli.sh
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB CPUSIM_SEED_PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/programs/*.asm)
add_executable(fuzz_assembler tests/fuzz_assembler.cpp)
target_link_libraries(fuzz_assembler PRIVATE cpusim_core)
add_test(NAME fuzz COMMAND fuzz_assembler 20000 1 ${CPUSIM_SEED_PROGRAMS})

if(CPUSIM_LIBFUZZER)
  add_executable(fuzz_assembler_libfuzzer tests/fuzz_assembler.cpp)
  target_compile_definitions(fuzz_assembler_libfuzzer PRIVATE CPUSIM_LIBFUZZER)
  target_compile_options(fuzz_assembler_libfuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_assembler_libfuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(fuzz_assembler_libfuzzer PRIVATE cpusim_core)
endif()
