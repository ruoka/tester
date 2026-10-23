# FILE_SET CXX_MODULES works from 3.28, but this project supports one CMake line:
# 4.x, which is what CI and the dev container install (4.1.2) and what macOS
# development uses. Older releases are not tested and not supported.
cmake_minimum_required(VERSION 4.0)

# --- Toolchain ---------------------------------------------------------------
# Same assumption as config/compiler.mk: Clang 21+ with libc++ modules, from a
# locally built LLVM on macOS and the apt.llvm.org packages on Linux. Override
# both together if you move it: -DLLVM_PREFIX=... -DCMAKE_CXX_COMPILER=...
if(CMAKE_HOST_APPLE)
  set(_llvm_default "/usr/local/llvm")
else()
  set(_llvm_default "/usr/lib/llvm-21")
endif()
set(LLVM_PREFIX "${_llvm_default}" CACHE PATH "LLVM installation prefix")

if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX}
   AND EXISTS "${LLVM_PREFIX}/bin/clang++")
  set(CMAKE_CXX_COMPILER "${LLVM_PREFIX}/bin/clang++")
endif()

# --- Language ----------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The compiler checks project() and find_package() run have to see the same
# standard library as the build itself, or they configure against libstdc++.
# add_compile_options() runs too late for those, CMAKE_CXX_FLAGS_INIT does not.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++ -fexperimental-library")
if(CMAKE_HOST_APPLE)
  # Same header selection as cb: only this LLVM's libc++, never the SDK's.
  string(APPEND CMAKE_CXX_FLAGS_INIT " -nostdinc++ -isystem ${LLVM_PREFIX}/include/c++/v1")
else()
  # Debian's clang++-21 does not find libc++-21-dev's headers on its own.
  string(APPEND CMAKE_CXX_FLAGS_INIT " -I${LLVM_PREFIX}/include/c++/v1")
endif()

# The Makefile optimizes by default and drops to -O0 under DEBUG=1.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(tester LANGUAGES CXX)

option(TESTER_STATIC "Link statically (config/compiler.mk STATIC=1)" OFF)

# Config flag sets mirror tools/cb.c++ (clang_driver::initialize_flags) rather
# than CMake's defaults, so CB and Ninja compile the same work and
# docs/cb-vs-ninja-benchmark.md compares build systems, not flag sets.
# CMake puts the CXX flags on the link line too, as cb's link_argv does.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
if(APPLE)
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "")
  set(CMAKE_EXE_LINKER_FLAGS_DEBUG "")
else()
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-O3")
  set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-O3 -g3")
endif()

set(CMAKE_LINKER_TYPE LLD)
add_compile_options(
  "-B${LLVM_PREFIX}/bin"
  -pthread
  -fPIC
  -Wall
  -Wextra
  -Wno-reserved-module-identifier
  -Wno-unused-command-line-argument
  -fno-implicit-modules
  -fno-implicit-module-maps
)
add_link_options("-B${LLVM_PREFIX}/bin" -pthread)

if(APPLE)
  # Link this LLVM's libc++ / libunwind, not the SDK's: -fexperimental-library
  # needs the former, and Apple Silicon exception catching needs the matching
  # libunwind (see docs/clang-modules-macos.md).
  add_link_options("-L${LLVM_PREFIX}/lib" "-Wl,-rpath,${LLVM_PREFIX}/lib" -Wl,-dead_strip)
  set(_libunwind_reexports "/usr/lib/system/introspection/libunwind.reexported_symbols")
  if(EXISTS "${_libunwind_reexports}")
    add_link_options("-Wl,-unexported_symbols_list,${_libunwind_reexports}")
  endif()
  # -stdlib=libc++ already pulls -lc++; name c++abi and unwind so the link
  # cannot fall back to the SDK unwinder.
  link_libraries(c++abi unwind)
else()
  # Same set config/compiler.mk and cb use on Linux; libc++experimental carries
  # the symbols -fexperimental-library refers to.
  link_directories("/usr/lib/${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-gnu" "${LLVM_PREFIX}/lib")
  link_libraries(c++ c++abi c++experimental)
  add_link_options("-Wl,-rpath,${LLVM_PREFIX}/lib")
endif()

if(TESTER_STATIC)
  add_link_options(-static -lc++ -lc++abi -lunwind -ldl -pthread)
endif()

find_package(Threads REQUIRED)

# --- std module --------------------------------------------------------------
# Compiled from libc++'s own source, the way config/compiler.mk (STD_LLVM_PREFIX)
# and cb do it. CMake can provide `std` itself, but it finds the source through
# `clang++ -print-file-name=libc++.modules.json`, and apt.llvm.org's libc++-21-dev
# installs a second copy of that manifest under /usr/lib/<triple> whose relative
# source-path only resolves from ${LLVM_PREFIX}/lib. Clang answers with that copy,
# so the build looks for /lib/share/libc++/v1/std.cppm and configure fails. Naming
# the file keeps both platforms on one path, and off a CMake-version-specific gate.
set(TESTER_STD_MODULE_DIR "${LLVM_PREFIX}/share/libc++/v1")
if(NOT EXISTS "${TESTER_STD_MODULE_DIR}/std.cppm")
  message(FATAL_ERROR
    "libc++'s std module source is missing: ${TESTER_STD_MODULE_DIR}/std.cppm\n"
    "Install libc++ for this toolchain, or point -DLLVM_PREFIX at one that ships it.")
endif()

add_library(std STATIC)
target_sources(std
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS ${TESTER_STD_MODULE_DIR}
    FILES "${TESTER_STD_MODULE_DIR}/std.cppm"
)

# --- Library -----------------------------------------------------------------
file(GLOB MODULE_INTERFACES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tester/*.c++m"
)

add_library(tester STATIC)
target_sources(tester
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tester
    FILES ${MODULE_INTERFACES}
)
target_include_directories(tester PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tester)
target_link_libraries(tester PUBLIC std Threads::Threads)

# --- Test runner -------------------------------------------------------------
# Tests self-register from static initializers, so their objects go straight
# into the executable the way $(test-objects) does; an intermediate archive
# would let the linker drop them.
file(GLOB FRAMEWORK_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tester/*.test.c++"
)
# Covers *.test.c++ and plain sources such as test_regex.c++, which also
# registers cases from a static initializer.
file(GLOB EXAMPLE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c++"
)
file(GLOB EXAMPLE_MODULE_INTERFACES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c++m"
)

add_executable(test_runner
  ${FRAMEWORK_TEST_SOURCES}
  ${EXAMPLE_SOURCES}
  "${CMAKE_CURRENT_SOURCE_DIR}/tester/test_runner.c++"
)
target_sources(test_runner
  PRIVATE
    FILE_SET examples TYPE CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/examples
    FILES ${EXAMPLE_MODULE_INTERFACES}
)
target_link_libraries(test_runner PRIVATE tester)

# --- Convenience targets -----------------------------------------------------
# TEST_RUNNER: the self tests re-spawn the runner and otherwise look for
# <cwd>/build-*/bin/test_runner, which does not exist in a CMake build tree.
add_custom_target(run_tests
  COMMAND ${CMAKE_COMMAND} -E env TEST_RUNNER=$<TARGET_FILE:test_runner>
          $<TARGET_FILE:test_runner> --jsonl=failures --tags=[self]
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  USES_TERMINAL
  COMMENT "Running framework tests"
)
add_dependencies(run_tests test_runner)

add_custom_target(run_all_tests
  COMMAND ${CMAKE_COMMAND} -E env TEST_RUNNER=$<TARGET_FILE:test_runner>
          $<TARGET_FILE:test_runner> --jsonl=failures
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  USES_TERMINAL
  COMMENT "Running the full suite"
)
add_dependencies(run_all_tests test_runner)
