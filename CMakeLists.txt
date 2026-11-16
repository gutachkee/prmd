cmake_minimum_required(VERSION 3.16)
project(Prmd LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PRMD_NATIVE "Tune for the build host CPU (-mcpu=native)" ON)
option(PRMD_BUILD_TESTS "Build unit tests" ON)
option(PRMD_PROFILE "Compile the PRMD_TIMED pipeline timers in (include/prmd/profile.h)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O2)
  if(PRMD_NATIVE)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-mcpu=native HAS_MCPU_NATIVE)
    check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
    if(HAS_MCPU_NATIVE)
      add_compile_options(-mcpu=native)
    elseif(HAS_MARCH_NATIVE)
      add_compile_options(-march=native)
    endif()
  endif()
endif()

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(OPENJP2 REQUIRED IMPORTED_TARGET libopenjp2)

set(PRMD_CORE_SOURCES
  src/image.cpp
  src/image_io.cpp
  src/pyramid.cpp
  src/transform.cpp
  src/blockcoder.cpp
  src/blockcoder_range.cpp
  src/blockcoder_bytes.cpp
  src/prmd_format.cpp
  src/prmd_encoder.cpp
  src/prmd_decoder.cpp
  src/crop_decoder.cpp
  src/encoder.cpp
  src/j2k_codec.cpp
  src/profile.cpp
)

add_library(prmd_core STATIC ${PRMD_CORE_SOURCES})
target_include_directories(prmd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prmd_core PUBLIC JPEG::JPEG PNG::PNG PkgConfig::ZSTD PkgConfig::OPENJP2)
if(PRMD_PROFILE)
  # PUBLIC, so tools linking prmd_core see the same PRMD_TIMED and kEnabled.
  target_compile_definitions(prmd_core PUBLIC PRMD_PROFILE)
endif()

add_executable(prmd tools/prmd_cli.cpp)
target_link_libraries(prmd PRIVATE prmd_core)

add_executable(prmdbench bench/prmdbench.cpp)
target_link_libraries(prmdbench PRIVATE prmd_core)

add_executable(prmdpipeline tools/prmd_pipeline.cpp)
target_link_libraries(prmdpipeline PRIVATE prmd_core)

if(PRMD_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  set(PRMD_TEST_SOURCES
    tests/test_image.cpp
    tests/test_rangecoder.cpp
    tests/test_transform.cpp
    tests/test_blockcoder.cpp
    tests/test_codec.cpp
    tests/test_j2k.cpp
    tests/test_lossy.cpp
    tests/reference.cpp
  )
  add_executable(prmd_tests ${PRMD_TEST_SOURCES})
  target_link_libraries(prmd_tests PRIVATE prmd_core GTest::gtest GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(prmd_tests DISCOVERY_TIMEOUT 60)
endif()
