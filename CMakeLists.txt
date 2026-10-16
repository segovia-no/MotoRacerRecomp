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

# Translator optimisations (docs/23-translator-speed.md): guest registers in C locals, local memory
# base. OFF builds the plain translation, for benchmarks.
option(MOTORACER_GUEST_OPT "Optimised translation (registers in locals, local memory base)" ON)
set(MOTORACER_TRANSLATOR_FLAGS "" CACHE STRING "Extra recomp_main.py flags (benchmarks, e.g. --no-x87-locals)")
mark_as_advanced(MOTORACER_TRANSLATOR_FLAGS)
set(RECOMP_TRANSLATOR_FLAGS ${MOTORACER_TRANSLATOR_FLAGS})
if(NOT MOTORACER_GUEST_OPT)
  list(APPEND RECOMP_TRANSLATOR_FLAGS --no-regcache)
endif()
file(GLOB RECOMP_TOOL_SOURCES "${CMAKE_SOURCE_DIR}/tools/recomp/*.py" "${CMAKE_SOURCE_DIR}/tools/recomp/recomp/*.py")
add_custom_command(
  OUTPUT ${GEN_SOURCES} "${GEN_DIR}/recomp_funcs.h" "${GEN_DIR}/imports.h"
  COMMAND "${RECOMP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/recomp/recomp_main.py"
          "${GAME_EXE}" "${GEN_DIR}" --files ${GEN_FILE_COUNT} ${RECOMP_TRANSLATOR_FLAGS}
  DEPENDS "${GAME_EXE}" ${RECOMP_TOOL_SOURCES}
  COMMENT "Recompiling game.EXE to C"
  VERBATIM)

# Generated guest code: plain C, warnings off (it is machine output).
add_library(gamecode OBJECT ${GEN_SOURCES})
target_include_directories(gamecode PRIVATE runtime/include "${GEN_DIR}")
set(MOTORACER_GUEST_OPT_LEVEL "-O2" CACHE STRING "Optimisation level for the generated game code")
target_compile_options(gamecode PRIVATE -w ${MOTORACER_GUEST_OPT_LEVEL} -fno-strict-aliasing)
if(MOTORACER_GUEST_OPT)
  target_compile_definitions(gamecode PRIVATE RECOMP_GENERATED)
endif()

# SDL3: the system package by default. MOTORACER_BUNDLED_SDL builds a pinned release and links it
# statically; SDL then loads X11/Wayland/Vulkan/audio libraries at run time, so the binary needs
# only glibc (Steam Deck / any Linux, see docs/15-ports-plan.md).
option(MOTORACER_BUNDLED_SDL "Build SDL3 from source and link it statically" OFF)
# Profiling for small targets (docs/21-psp-feasibility.md): x87 in single precision, and a counter of
# executed x86 instructions (MOTORACER_ICOUNT=1 logs them per frame).
option(MOTORACER_FPU_FLOAT "Emulate the x87 FPU in single precision" OFF)
# PSP (docs/25-psp-port.md; configure with psp-cmake from pspdev): single-precision x87 (the
# Allegrex FPU has no doubles), the compact guest address space (no virtual memory), small data
# section off (-G0: the game has more globals than gp-relative addressing reaches).
if(PSP)
  set(MOTORACER_FPU_FLOAT ON CACHE BOOL "" FORCE)
  add_compile_definitions(RECOMP_COMPACT_GUEST)
  add_compile_options(-G0)
endif()
option(MOTORACER_COUNT_INSNS "Count executed x86 instructions (slow)" OFF)
if(MOTORACER_FPU_FLOAT)
  add_compile_definitions(RECOMP_FPU_FLOAT)
endif()
if(MOTORACER_COUNT_INSNS)
  add_compile_definitions(RECOMP_COUNT_INSNS)
endif()
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
elseif(WIN32)
  list(APPEND RUNTIME_SOURCES ${PLAT}/win32.cpp)
elseif(PSP)
  # no SDL_GPU on the PSP: the GE backend (runtime/src/psp) replaces the SDL_GPU presenter and
  # Direct3D backend
  list(REMOVE_ITEM RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/src/host_gpu.cpp
       ${CMAKE_SOURCE_DIR}/runtime/src/dx/d3d_gpu.cpp ${CMAKE_SOURCE_DIR}/runtime/src/gpu/shaders.cpp)
  list(APPEND RUNTIME_SOURCES ${PLAT}/psp.cpp ${PLAT}/stack_mips.S runtime/src/psp/ge.cpp
       runtime/src/psp/gpu_psp.cpp runtime/src/psp/d3d_ge.cpp)
elseif(UNIX)
  list(APPEND RUNTIME_SOURCES ${PLAT}/posix.cpp ${PLAT}/paths_xdg.cpp)
else()
  message(FATAL_ERROR "no platform adapters for ${CMAKE_SYSTEM_NAME} yet (see docs/15-ports-plan.md)")
endif()
if(WIN32 OR PSP)
  # Windows: the deep stack is a fiber (win32.cpp); PSP: stack_mips.S (above)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
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
if(PSP)
  target_link_libraries(motoracer PRIVATE pspgu pspge pspdisplay psppower)
endif()

get_filename_component(GAME_DIR_FOR_ICON "${GAME_EXE}" DIRECTORY)
if(WIN32)
  # GUI program (no console window), one self-contained .exe (static C++ runtime), manifest
  # (UTF-8 code page, DPI awareness) and the game's own icon from the user's GOG files.
  enable_language(RC)
  set(_rc "1 24 \"${CMAKE_SOURCE_DIR}/runtime/src/platform/motoracer.manifest\"\n")
  if(EXISTS "${GAME_DIR_FOR_ICON}/goggame-1207658895.ico")
    string(APPEND _rc "1 ICON \"${GAME_DIR_FOR_ICON}/goggame-1207658895.ico\"\n")
  endif()
  file(WRITE "${CMAKE_BINARY_DIR}/motoracer.rc" "${_rc}")
  target_sources(motoracer PRIVATE "${CMAKE_BINARY_DIR}/motoracer.rc")
  target_link_options(motoracer PRIVATE -mwindows -static)
endif()

# PSP: EBOOT.PBP next to the build (copy it to ms0:/PSP/GAME/MOTORACER/ with the game files in game/).
# MEMSIZE 1: the extended memory of PSP-2000/3000 (64 MB).
if(PSP)
  create_pbp_file(TARGET motoracer TITLE "Moto Racer" MEMSIZE 1)
endif()

# `cmake --build build/cmake --target app` -> build/cmake/MotoRacer.app
if(APPLE)
  add_custom_target(app
  COMMAND "${CMAKE_SOURCE_DIR}/tools/make_app.sh" "$<TARGET_FILE:motoracer>" "${CMAKE_BINARY_DIR}" "${GAME_DIR_FOR_ICON}"
  DEPENDS motoracer
  COMMENT "Assembling MotoRacer.app"
  VERBATIM)
endif()
