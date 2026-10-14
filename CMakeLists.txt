cmake_minimum_required(VERSION 3.24)
project(MotoRacerRecomp LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The user's own copy of the GOG game.EXE. Recompiled C is generated from it at build time
# and never committed.
set(GAME_EXE "${CMAKE_SOURCE_DIR}/extracted/app/game.EXE" CACHE FILEPATH "GOG Moto Racer game.EXE")
set(RECOMP_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python with pefile+capstone")
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(GEN_FILE_COUNT 64)

set(GEN_SOURCES "${GEN_DIR}/dispatch_table.c" "${GEN_DIR}/imports.c")
math(EXPR _last "${GEN_FILE_COUNT} - 1")
foreach(i RANGE ${_last})
  string(LENGTH "${i}" _len)
  if(_len EQUAL 1)
    set(_n "00${i}")
  elseif(_len EQUAL 2)
    set(_n "0${i}")
  else()
    set(_n "${i}")
  endif()
  list(APPEND GEN_SOURCES "${GEN_DIR}/code_${_n}.c")
endforeach()

file(GLOB RECOMP_TOOL_SOURCES "${CMAKE_SOURCE_DIR}/tools/recomp/*.py" "${CMAKE_SOURCE_DIR}/tools/recomp/recomp/*.py")
add_custom_command(
  OUTPUT ${GEN_SOURCES} "${GEN_DIR}/recomp_funcs.h" "${GEN_DIR}/imports.h"
  COMMAND "${RECOMP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/recomp/recomp_main.py"
          "${GAME_EXE}" "${GEN_DIR}" --files ${GEN_FILE_COUNT}
  DEPENDS "${GAME_EXE}" ${RECOMP_TOOL_SOURCES}
  COMMENT "Recompiling game.EXE to C"
  VERBATIM)

# Generated guest code: plain C, warnings off (it is machine output).
add_library(gamecode OBJECT ${GEN_SOURCES})
target_include_directories(gamecode PRIVATE runtime/include "${GEN_DIR}")
target_compile_options(gamecode PRIVATE -w -O1 -fno-strict-aliasing)

# SDL3: the system package by default. MOTORACER_BUNDLED_SDL builds a pinned release and links it
# statically; SDL then loads X11/Wayland/Vulkan/audio libraries at run time, so the binary needs
# only glibc (Steam Deck / any Linux, see docs/15-ports-plan.md).
option(MOTORACER_BUNDLED_SDL "Build SDL3 from source and link it statically" OFF)
if(MOTORACER_BUNDLED_SDL)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
    URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
  FetchContent_MakeAvailable(SDL3)
  set(SDL3_TARGET SDL3::SDL3-static)
else()
  find_package(SDL3 REQUIRED CONFIG)
  set(SDL3_TARGET SDL3::SDL3)
endif()

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS
  runtime/src/*.cpp runtime/src/*.c runtime/src/win32/*.cpp runtime/src/dx/*.cpp runtime/src/gpu/*.cpp)

# Host platform adapters (runtime/src/platform/platform.h): exactly one set per OS / CPU.
set(PLAT runtime/src/platform)
if(APPLE)
  list(APPEND RUNTIME_SOURCES ${PLAT}/posix.cpp ${PLAT}/paths_macos.cpp)
elseif(UNIX)
  list(APPEND RUNTIME_SOURCES ${PLAT}/posix.cpp ${PLAT}/paths_xdg.cpp)
else()
  message(FATAL_ERROR "no platform adapters for ${CMAKE_SYSTEM_NAME} yet (see docs/15-ports-plan.md)")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  list(APPEND RUNTIME_SOURCES ${PLAT}/stack_arm64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  list(APPEND RUNTIME_SOURCES ${PLAT}/stack_x86_64_sysv.S)
else()
  message(FATAL_ERROR "no call_on_stack trampoline for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

add_executable(motoracer ${RUNTIME_SOURCES} $<TARGET_OBJECTS:gamecode>)
target_include_directories(motoracer PRIVATE runtime/include runtime/src runtime/src/win32 runtime/src/dx "${GEN_DIR}")
target_compile_options(motoracer PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
target_link_libraries(motoracer PRIVATE ${SDL3_TARGET})

# `cmake --build build/cmake --target app` -> build/cmake/MotoRacer.app
get_filename_component(GAME_DIR_FOR_ICON "${GAME_EXE}" DIRECTORY)
if(APPLE)
  add_custom_target(app
  COMMAND "${CMAKE_SOURCE_DIR}/tools/make_app.sh" "$<TARGET_FILE:motoracer>" "${CMAKE_BINARY_DIR}" "${GAME_DIR_FOR_ICON}"
  DEPENDS motoracer
  COMMENT "Assembling MotoRacer.app"
  VERBATIM)
endif()
