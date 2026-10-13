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

find_package(SDL3 REQUIRED CONFIG)

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS
  runtime/src/*.cpp runtime/src/*.c runtime/src/win32/*.cpp runtime/src/dx/*.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  list(APPEND RUNTIME_SOURCES runtime/src/stack_switch_arm64.S)
endif()

add_executable(motoracer ${RUNTIME_SOURCES} $<TARGET_OBJECTS:gamecode>)
target_include_directories(motoracer PRIVATE runtime/include runtime/src runtime/src/win32 runtime/src/dx "${GEN_DIR}")
target_compile_options(motoracer PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
target_link_libraries(motoracer PRIVATE SDL3::SDL3)
