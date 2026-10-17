cmake_minimum_required(VERSION 3.28)
project(ultra VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ULTRA_BUILD_TESTS "Build unit tests and harness" ON)
option(ULTRA_FRONTEND "Build the SDL3 frontend (Phase 4)" OFF)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Dependencies are pinned to commit hashes (docs/SPEC.md, Toolchain).
FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 40626af88bd7df9a5fb80be7b25ac85b122d6c21 # 11.2.0
  SYSTEM)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt)

if(ULTRA_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG 56809e5282f104c5c8b570e7c2996cdc352d94f1 # v3.8.1
    SYSTEM)
  FetchContent_MakeAvailable(Catch2)
endif()

if(ULTRA_FRONTEND)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG f6864924f76e1a0b4abaefc76ae2ed22b1a8916e # release-3.2.8
    SYSTEM)
  FetchContent_MakeAvailable(SDL3)
endif()

# Warnings apply to our targets only, never to fetched dependencies.
add_library(ultra_warnings INTERFACE)
target_compile_options(ultra_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Werror
  -Wshadow -Wconversion -Wsign-conversion -Wimplicit-fallthrough)

add_subdirectory(src)

if(ULTRA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(tools)
endif()
