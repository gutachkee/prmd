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
# Sanitizer builds (use a separate build directory):
#   -DPRMD_SANITIZE=address,undefined   ASan + UBSan
#   -DPRMD_SANITIZE=thread              TSan (the multithreaded encoder)
set(PRMD_SANITIZE "" CACHE STRING
    "Sanitizers for every target, e.g. address,undefined or thread (empty: none)")

add_compile_options(-Wall -Wextra -Wpedantic)
if(PRMD_SANITIZE)
  # Every finding is fatal, so ctest fails on it.
  add_compile_options(-fsanitize=${PRMD_SANITIZE} -fno-sanitize-recover=all
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${PRMD_SANITIZE})
endif()
# 64-bit off_t on 32-bit targets (armv7): files may approach 4 GiB.
add_compile_definitions(_FILE_OFFSET_BITS=64)
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
find_package(Threads REQUIRED)

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
  src/parallel.cpp
)

add_library(prmd_core STATIC ${PRMD_CORE_SOURCES})
target_include_directories(prmd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prmd_core PUBLIC JPEG::JPEG PNG::PNG PkgConfig::ZSTD PkgConfig::OPENJP2
                      Threads::Threads)
if(PRMD_PROFILE)
  # PUBLIC, so tools linking prmd_core see the same PRMD_TIMED and kEnabled.
  target_compile_definitions(prmd_core PUBLIC PRMD_PROFILE)
endif()

add_executable(prmd tools/prmd_cli.cpp)
target_link_libraries(prmd PRIVATE prmd_core)

add_executable(prmdbench tools/prmd_bench.cpp)
target_link_libraries(prmdbench PRIVATE prmd_core)
# Report header states the real build type and flags.
get_directory_property(PRMD_COMPILE_OPTS COMPILE_OPTIONS)
list(JOIN PRMD_COMPILE_OPTS " " PRMD_COMPILE_OPTS_STR)
string(TOUPPER "${CMAKE_BUILD_TYPE}" PRMD_BT_UPPER)
string(STRIP "${CMAKE_BUILD_TYPE} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${PRMD_BT_UPPER}} ${PRMD_COMPILE_OPTS_STR}" PRMD_BUILD_DESC)
string(REGEX REPLACE " +" " " PRMD_BUILD_DESC "${PRMD_BUILD_DESC}")
target_compile_definitions(prmdbench PRIVATE PRMD_BUILD_DESC="${PRMD_BUILD_DESC}")

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
    tests/test_cli.cpp
    tests/test_parallel.cpp
    tests/reference.cpp
  )
  add_executable(prmd_tests ${PRMD_TEST_SOURCES})
  target_link_libraries(prmd_tests PRIVATE prmd_core GTest::gtest GTest::gtest_main)
  # test_cli.cpp runs the `prmd` executable as a child process.
  add_dependencies(prmd_tests prmd)
  target_compile_definitions(prmd_tests PRIVATE PRMD_CLI_PATH="$<TARGET_FILE:prmd>")
  include(GoogleTest)
  gtest_discover_tests(prmd_tests DISCOVERY_TIMEOUT 60)
endif()
